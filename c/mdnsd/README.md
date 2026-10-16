# mdnsd

A Multicast DNS responder ([RFC 6762](https://www.rfc-editor.org/rfc/rfc6762)) with DNS-SD service
advertising ([RFC 6763](https://www.rfc-editor.org/rfc/rfc6763)). It answers `<host>.local` with r2's
IPv4 address, so other machines on the LAN can reach r2 by name instead of by a DHCP address that
changes. It also browses for the services other machines advertise, and keeps a list of them for
Memento's Network window (its mDNS tab):

```sh
telnet rourex.local                     # TNT
curl http://rourex.local/ui             # GARN
avahi-browse -art                       # every r2 server mdnsd knows of
```

| Argument | Pair value (if any) | Meaning | Required |
|----------|---------------------|---------|----------|
| `--name` | host name | Answer for `<name>.local`. The default is the system name from sysinfo (`rourex`), the same name eth sends to DHCP, followed when Memento's Settings renames the machine. | No |
| `debug`  | *none* | Prints every answer and every advertised service. | No |

## Example

```sh
bg eth
bg tnt eth
bg mdnsd --name lab
```

mdnsd runs beside `eth`, which owns the address. It waits until eth has configured one (syscall
`0x3d`), announces it twice, one second apart, and announces it again if the address changes, for
example when a DHCP lease replaces the fallback address. Without `--name` it reads the system name
every second as well. When Settings renames the machine (syscall `0x01`, `0x04`), the services named
after the old name get their goodbyes, and the new names are announced.

## Services

Every second, mdnsd reads the kernel's TCP port table (syscall `0x46`) and advertises the servers it
knows by port:

| Port | Service type | Server |
|------|--------------|--------|
| 80   | `_http._tcp`   | GARN |
| 8080 | `_http._tcp`   | streamd, chat's web page |
| 23   | `_telnet._tcp` | TNT |
| 5555 | `_telnet._tcp` | bbs-go |
| 7780 | `_rntp._tcp`   | an RNTP export |

Each instance is named after the host and the process holding the port, such as
`rourex garn._http._tcp.local`. A server that starts is announced. A server that stops gets a
goodbye (TTL 0), which removes it from service browsers right away instead of after the PTR record's
75-minute TTL.

To add a server, add its port to `KNOWN` in `main.c`.

## Browsing

mdnsd asks the network what is there (`browse.c`, DNS-SD as in RFC 6763 §4 and §9). It asks
`_services._dns-sd._udp.local` for the service types, and each type it hears of for its instances.
It also asks for `_http`, `_telnet`, `_rntp` and `_ssh` over TCP, whether or not anyone names them.
An instance is listed once its SRV record says where it is, with the address from the target's A
record.

The questions go out when mdnsd has an address, again after 1 s, 2 s, 4 s and so on, doubling up to
an hour (RFC 6762 §5.2). Each record is kept for its TTL. It is asked for again at 80, 85, 90 and
95 % of the TTL, and a server that stops answering drops off the list when its SRV record expires,
two minutes for most. A goodbye removes it at once. Answers go to the group, so answers to other
machines' questions count as well. A new address forgets everything and starts over.

The list is `/mnt/tmp/MDNS.TXT`: 8192 bytes, zero-padded, rewritten whole whenever it changes.

```text
# mdnsd 42
rourex tnt	_telnet._tcp	rourex.local	10.0.2.15	23
Py Web	_http._tcp	pyhost.local	10.0.2.9	8000
# end 42 2
```

There is one line per service, with five tab-separated fields: the instance, the type, the SRV
target, its IPv4 address (empty while there is none), and the port. This machine's own services
come first. Bytes other than printable ASCII become `?`, one per UTF-8 character. A reader that
finds no `# end` line with the first line's number read the file while it was being rewritten, and
reads it again. A list left by an mdnsd that is no longer running says nothing about the network.
Memento shows a blank list unless an `MDNSD` task is in the task table.

The cache holds 24 types, 48 services and 48 addresses. What does not fit is not kept.

## What it does not do

It is minimal on purpose:

- **No probing.** Two machines with the same name both answer for it. Give each r2 its own `--name`.
- **No known-answer suppression and no response delays.** Every query is answered at once, and
  every responder answers each of mdnsd's questions in full.
- **No NSEC records and no IPv6.** An `AAAA` query gets no answer, as do record types mdnsd does not
  have.
- **Names are written uncompressed.** A record that does not fit the 1514-byte frame is left out, and
  so is everything after it. That only happens with far more services than r2 runs.
- **Browsing is for the list only.** r2 programs cannot resolve `.local` names yet. That needs a
  querier in libcr2 and r2net. No TXT records and no IPv6 addresses are kept.

Queries from port 5353 to the group are answered to the group. A query sent to r2's own address, or
from another port (a "legacy" resolver such as `dig -p 5353 @224.0.0.251 rourex.local`), is answered
to its sender alone, with the query ID and question repeated and TTLs of at most 10 seconds.

## Network cards

The kernel hands a UDP frame to whoever bound its destination port, whatever address it was sent
to. The card still has to accept the group address `01:00:5e:00:00:fb` (224.0.0.251):

- **RTL8139:** receives every frame.
- **BCM57765:** accepts every multicast frame.
- **Intel e1000/e1000e/PCH:** has that one group set in its multicast table (`src/net/e1000.rs`).
  Older kernels leave the table empty, so queries never reach mdnsd, although its announcements
  still go out.

If a WireGuard peer's AllowedIPs cover `224.0.0.0/4` (for example `0.0.0.0/0`), the kernel sends
mdnsd's multicast replies into the tunnel instead of out of the card.

## Build and test

```sh
make build            # mdnsd.elf; rebuilds libcr2 when needed
make test             # host tests of the responder (mdns.c) and the browser (browse.c), each with a fuzz pass
make sanitizer-test   # the same with clang's ASan and UBSan
```

`mdns.c` holds the protocol and `browse.c` the browser's cache and questions. Neither makes a
syscall, so the host tests run them as they are. The browser's tests use `mdns.c` as the other
machine's responder. `main.c` is the r2 side: the UDP binding, the port table, the list file, and
the timers.
