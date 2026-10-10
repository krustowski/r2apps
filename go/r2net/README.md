# r2net

The TCP/IP stack for Go on `r2`.  The kernel gives a program a wire and
nothing above it --- raw IPv4 packets over a serial line, or raw Ethernet
frames from the NIC --- so everything between that and a request lives here:
ARP, IPv4, ICMP, UDP, DNS, TCP and an HTTP/1.0 client.  It is the Go
counterpart of the TCP/IP half of `c/libcr2`.

```go
stack, err := r2net.Open(r2net.Options{
	Link:    "eth",
	LocalIP: r2net.IP{10, 3, 4, 2},
	Gateway: r2net.IP{10, 3, 4, 1},
	DNS:     r2net.IP{10, 3, 4, 1},
})
if err != nil {
	return err
}
defer stack.Close()

res, err := stack.Get("http://api.example.com/health", nil, 10*time.Second)
```

| File | Covers |
| ---- | ------ |
| `r2net.go` | The stack: options, the packet loop, dispatch, deadlines. |
| `link.go` | The two links --- SLIP over serial, Ethernet with ARP. |
| `slip.go` | SLIP framing (RFC 1055), decode side. |
| `ipv4.go` | Headers and the checksums everything above borrows. |
| `icmp.go` | `Ping`, and answering the echo requests this machine receives. |
| `tcp.go` | The client: `Dial`, `Read`, `Write`, `Close`. |
| `udp.go` | `Exchange`, one datagram out and one back. |
| `dns.go` | `Resolve`: one A query, first answer wins. |
| `http.go` | `Get`, `Do`, and URL parsing. |
| `addr.go` | `IP` and `MAC`, and how they are written down. |

`libgor2` stays what it says it is --- the binding for the ABI, one function
per syscall --- and this is the layer that uses it.

## Two links

```go
Options{Link: "eth"}   // RTL8139 or E1000, through kernel driver registration
Options{Link: "slip"}  // IPv4 packets over COM1
```

**Ethernet** is the useful one and the one everything below is about.
**SLIP** is a serial line with one host at the other end: no ARP, no
addressing, the kernel does the framing on the way out.  It is simpler and
slower, and it needs the kernel built without `serial_debug` --- that feature
writes the kernel's own trace to COM1, which is the wire.

## The Ethernet driver registration

The kernel polls the NIC only on behalf of the one process that registered as
the global Ethernet driver (syscall `0x37`), and it delivers every frame that
is not claimed by a bound TCP port to that process.  `Open` looks at what is
already registered and takes one of two roads:

| What it finds | What it does | What works |
| ------------- | ------------ | ---------- |
| No driver | Registers as the global driver | Everything: TCP, ICMP, UDP, DNS, and answering ARP for the machine |
| A driver (`eth.elf`, `garn`) | Binds the TCP ports it needs | TCP and HTTP only |

`CanICMP` reports which one happened. In the second case remote ICMP echo replies and
UDP datagrams go to the other process, because the kernel routes by TCP port
and those frames have none; `Ping` and `Resolve` say so with `ErrNoICMP` and
`ErrNoDatagram` rather than timing out, since "another process holds the NIC"
and "the host is down" are different facts.

Two consequences worth knowing before designing around this:

- **The kernel releases registration and TCP port bindings** when the process
  exits, is killed or crashes. `Stack.Close` releases connection bindings;
  global driver ownership lasts until process exit.
- **While registered, this stack is the machine's network stack.**  It answers
  ARP who-has for the local address and ICMP echo requests, because nothing
  else is going to.
- **Two programs on the network need port ranges of their own.**  Local TCP
  ports go round robin from `Options.PortBase` (40000 by default), eight of
  them or `Options.PortCount`, and the kernel's port registry has sixteen
  entries for the whole machine.  A program hosted in a Memento window takes
  its range from the window (`48000 + slot*32`), as telegram.elf does.

Local Ethernet traffic to `127.0.0.0/8` or this machine's address uses the
kernel's loopback device. `Ping` to these addresses works even when another
process owns the driver. Local TCP still uses bound destination ports. The
stack routes loopback addresses directly and accepts the device's zero MAC;
remote UDP/DNS retains the driver ownership requirement.

## One goroutine

Nothing here is safe to call from two goroutines and nothing here starts one.
Every blocking call --- `Dial`, `Read`, `Ping`, `Resolve` --- is the same loop:
drain what has arrived, answer what the link owns, advance every open
connection's timers, sleep a tick.  Connections can be open at once (the
demultiplexer keys on the four-tuple) but they are driven from one place.

That is a choice about this machine rather than a limitation of TinyGo.  The
kernel already queues frames for us while we are busy, and the cooperative
scheduler will not take a goroutine off a syscall loop anyway.  Goroutines
themselves are affordable now --- 16 KiB each, and a whole `dish` run over this
stack writes about 7 KiB of one --- so a goroutine-per-connection API would
need a single goroutine owning the link and channels to the rest, not cheaper
stacks.

## How frames reach this loop

The kernel takes up to **sixteen frames per timer tick** off the NIC and
copies each into a 2 KiB buffer of its own, held for the receiving process
until it reads the frame with syscall `0x35`; up to 64 frames wait in one
process's queue.  A frame the kernel cannot queue --- no free buffer, the queue
full --- stays in the NIC's ring and is tried again next tick, so nothing is
lost between the card and this loop for being read late.  Only a receiver that
takes nothing for 200 ticks has frames dropped in front of it.

So each turn of the loop drains the queue (up to its full depth of 64) before
running the timers, and sleeps a tick only when the queue came up empty.

It used to be one frame per tick through **one shared buffer**, overwritten by
the next frame whether or not the last had been read, so bursts were lost
wholesale and this stack answered a gap with three duplicate
acknowledgements at once to force a fast retransmit.  With per-frame buffers
the segments behind a gap arrive and produce those duplicates themselves, so
`signalGap` is back to the RFC's one per out-of-order segment.

## What TCP here is and is not

It is a client: a three-way handshake (an answer to the SYN that acknowledges
something else --- the server's TIME_WAIT from an earlier connection on the
same ports, on a real network --- is reset, so the SYN sent again goes
through), cumulative acknowledgements, **one outstanding segment at a time**, exponential backoff
over five retries, and a close that waits 300 ms for the peer's half before
giving up. The receiver buffers up to sixteen disjoint out-of-order segments
within its 16 KiB receive capacity and drains them when a gap fills. It repeats
receive-window updates up to three times unless new data confirms progress.

It is not fast.  There is no window beyond one segment, no congestion control,
no selective acknowledgement, and no send queue. Receive segments beyond the
bounded reassembly capacity are dropped and re-acknowledged. For a check that
sends a few hundred bytes and reads a few kilobytes that costs one round trip
per segment and saves a send queue, a retransmission list and the timers that
go with them, in a program whose whole heap is 1.5 MiB.

Outgoing segments honor the server's SYN MSS (536 bytes when omitted) and
are capped at 1024 bytes. Hosted programs cannot receive path-MTU ICMP errors
while another process holds the Ethernet driver, so sending 1460-byte payloads
with DF set could silently stall uploads on a smaller internet path.
`Conn.SetTimeout` gives an idle budget renewed by TCP progress; a large write
may take longer than the budget while acknowledgements keep arriving.
`SetDeadline` keeps an absolute budget for callers that require one.

## TLS adapter

The HTTP helpers in this package handle cleartext HTTP. Certificate-verified
HTTPS is provided by [r2tls](../r2tls), which runs Memento's portable BearSSL
over these TCP connections and is used by the Spotify client.

TCP waits yield to the Go scheduler so an audio/UI goroutine can run while a
request is blocked. `Options.Check` can return an error to cancel a pending
operation; it runs on the stack's owner goroutine. Other goroutines must not
call the stack directly. Reads advertise reopened receive-window space, and
partly overlapping retransmissions preserve their new suffix.

## Addresses

`r2` publishes no netmask and has no DHCP client, so something has to be
assumed, and the defaults are this repository's own setup: `10.3.4.2`, a
`/24`, and the `.1` of that network as the gateway.  `Open` prefers the address
the Ethernet driver published through the kernel's system information block
when there is one.  Anything that runs outside this setup should pass its own.

## Gotchas inherited from the machine

- **Buffers are fields, not locals.**  The ABI takes an address as an integer,
  and TinyGo answers a local whose address goes that way by moving it to the
  heap on every call.  In a packet loop that is an allocation per iteration.
  See "Taking the address of a local costs an allocation" in
  [../README.md](../README.md).
- **A frame longer than 2048 bytes is dropped by the kernel**, so every buffer
  here is that size.  The advertised MSS is 1460, a full Ethernet frame.
- The kernel now preserves floating-point state across context switches.
  This TCP stack continues to use integer arithmetic.

`ResolveTCP(name, timeout)` is also available for DNS servers that accept TCP
queries on port 53. It works while another process owns the Ethernet driver,
using a bound TCP source port. The Go Spotify client uses this path.
