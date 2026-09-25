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
Options{Link: "eth"}   // the RTL8139, through the kernel's driver registration
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

`CanICMP` reports which one happened.  In the second case ICMP echo replies and
UDP datagrams go to the other process, because the kernel routes by TCP port
and those frames have none; `Ping` and `Resolve` say so with `ErrNoICMP` and
`ErrNoDatagram` rather than timing out, since "another process holds the NIC"
and "the host is down" are different facts.

Two consequences worth knowing before designing around this:

- **The registration is never released**, not even when the process exits.  A
  program that registers and leaves takes the machine's ARP responder with it
  until the next boot, and a second run of the same program finds the
  registration still held --- by nobody.
- **While registered, this stack is the machine's network stack.**  It answers
  ARP who-has for the local address and ICMP echo requests, because nothing
  else is going to.

## One goroutine

Nothing here is safe to call from two goroutines and nothing here starts one.
Every blocking call --- `Dial`, `Read`, `Ping`, `Resolve` --- is the same loop:
drain what has arrived, answer what the link owns, advance every open
connection's timers, sleep a tick.  Connections can be open at once (the
demultiplexer keys on the four-tuple) but they are driven from one place.

That is a choice about this machine rather than a limitation of TinyGo.  A
goroutine commits 32 KiB of stack before it runs once, about thirty fit in the
heap, and the cooperative scheduler will not take one off a syscall loop
anyway.

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

It is a client: a three-way handshake, in-order data, cumulative
acknowledgements, **one outstanding segment at a time**, exponential backoff
over five retries, and a close that waits 300 ms for the peer's half before
giving up.

It is not fast.  There is no window beyond one segment, no congestion control,
no selective acknowledgement, and no reassembly --- a segment that arrives out
of order is dropped and re-acknowledged rather than held.  For a check that
sends a few hundred bytes and reads a few kilobytes that costs one round trip
per segment and saves a send queue, a retransmission list and the timers that
go with them, in a program whose whole heap is 1.5 MiB.

## No TLS

There will not be any: no entropy source, no big-integer arithmetic worth the
name, and no room for a certificate chain in a 2 MiB frame.  `Do` refuses an
`https://` URL with `ErrTLS` rather than quietly fetching it in the clear.

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
- **Floating point is not saved across a context switch.**  Nothing here uses
  it; a program that does should read the same note.
