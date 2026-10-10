# wgd

A standalone C WireGuard daemon for r2: one peer, IPv4, a configuration read
once at startup, and a fixed 1420-byte tunnel MTU. It runs alongside `eth.elf`.
An authenticated peer can ping the tunnel address and reach r2 TCP servers.
The protocol and portable crypto come from a pinned BSD-licensed
[wireguard-lwip](https://github.com/smartalock/wireguard-lwip) core; there is no
lwIP dependency. See [vendor/README.md](vendor/README.md) for the exact revision
and changes.

## Build and kernel support

```sh
make build
make check
make sanitizer-test
```

`wgd.elf` links against libcr2. Its private stack is 128 KiB and packet buffers
are fixed. On CPUs without usable hardware random instructions, the entropy
collector allocates up to 2 MiB for its memory noise loop, plus collector state.

The companion kernel changes add syscall `0x44`, UDP delivery for the daemon,
routes to its peer's AllowedIPs, ICMP probe delivery, and authenticated packet injection. Rebuild and
boot the updated kernel; copying `wgd.elf` onto an older image is insufficient.
Startup probes for this ABI and reports a useful error on an older kernel.

[kernel/r2.patch](kernel/r2.patch) upgrades the initial single-address tunnel ABI.
To generate a patch for a fresh checkout or an earlier installation while
preserving its local edits:

```sh
python3 kernel/prepare.py ../../../r2_main /tmp/r2-wgd-kernel
git -C ../../../r2_main apply --check "$PWD/kernel/r2.patch"
git -C ../../../r2_main apply "$PWD/kernel/r2.patch"
make -C ../../../r2_main build
```

The preparation script recognizes an existing installation; repeating it on
an updated checkout generates an empty patch.
The updated kernel Makefile builds and includes `wgd.elf` in the boot image,
plus `opt/wgd/wgd.cfg.example`, rebuilds `sh` and `tnt` with the ICMP probe commands,
and relinks `chat` with the libcr2 reply-address
fix. Relink other existing libcr2 TCP servers before using them through the
tunnel. It does not install keys or start the daemon
automatically.

## Configure and run

Generate separate server and client keys using the host's WireGuard tools:

```sh
umask 077
wg genkey > r2.key
wg pubkey < r2.key > r2.pub
wg genkey > client.key
wg pubkey < client.key > client.pub
```

Copy [WGD.CFG.example](WGD.CFG.example) to `WGD.CFG`, replace `PrivateKey` with
the contents of `r2.key` and `PublicKey` with `client.pub`, and put it on the
r2 floppy. The example deliberately has invalid placeholders rather than
reusable private keys. Keep the private key configuration out of version control.

```sh
# r2 shell; eth supplies the physical address, gateway and ARP
bg eth
fg wgd --check /mnt/fat/WGD.CFG
bg wgd /mnt/fat/WGD.CFG
read /mnt/tmp/WGD.LOG
```

In Memento's Shell window, `bg wgd /mnt/tmp/WGD.CFG` shows the initial daemon
log and the diagnostic path. Background child output normally goes to the
kernel console; `WGD.LOG` keeps startup errors available in the window too.
Use `fg wgd --check <path>` to validate a file without starting a daemon, then
`read /mnt/tmp/WGD.LOG` to see its result from a hosted shell.

With `Endpoint` set and the desired subnets in the peer's `AllowedIPs`, these
shell builtins initiate the tunnel and select the interface's tunnel address:

```sh
ping 10.4.6.68
traceroute 10.4.6.68
```

`ping` sends four ICMP echo probes. `traceroute` sends ICMP echo with increasing
TTL, up to 30 hops, and displays authenticated ICMP time-exceeded and
destination-unreachable replies. Both accept numeric IPv4 addresses and work
in Memento's Shell window, console `sh`, and `tnt`. Only one tunnel probe may
run at a time. The remote WireGuard router must forward to those subnets and
have a return route to the r2 tunnel address. Routers outside `AllowedIPs` have
their replies dropped, so those traceroute hops appear as `*`.

Without an explicit path, the daemon tries `WGD.CFG` in the working directory,
`/mnt/fat/WGD.CFG`, then `/mnt/tar/opt/wgd/wgd.cfg`. Configuration is limited to
2048 bytes. Unknown options, duplicate fields or peers, malformed keys and
addresses are rejected. `--check` validates configuration without starting
networking or requiring the new kernel ABI.

The daemon waits up to ten seconds for `eth` to publish its physical IPv4
configuration. The tunnel addresses must differ from that physical address.
It requires a valid UTC RTC. It first tries RDSEED/RDRAND, checking CPU support
before executing either instruction. If neither is available, or they exhaust
their retries, it uses the pinned upstream
[Jitterentropy library](vendor/jitterentropy/R2.md), collecting CPU execution
noise with the high-resolution timestamp counter. Older Intel i3 CPUs can use
this path without a seed file or a configuration change.

The fallback runs upstream conditioning and timer/noise startup tests and
continuous repetition, adaptive-proportion and lag-predictor health tests.
It runs the timed library code at `-O0` without LTO, with its memory noise loop
enabled and a fixed 2 MiB cap. A startup or runtime health failure stops the
daemon. It never substitutes RTC, uptime, addresses, private keys or a constant
seed as entropy. Successful health tests alone do not certify the entropy rate
of a particular bare-metal machine; this r2 integration has not undergone
platform entropy assessment or FIPS validation.

`WGD.LOG` records the CPU brand, supported instructions, selected entropy source,
and any health-test error code. Startup may take several seconds on the fallback;
wait for `listening; public key follows` before running ping/traceroute.
Use `-cpu max` for QEMU's hardware path, or `--no-rng --cpu SandyBridge` with the
test script to exercise a CPU without random instructions.

A corresponding Linux/client configuration is:

```ini
[Interface]
PrivateKey = <contents of client.key>
Address = 10.77.0.2/32
MTU = 1420

[Peer]
PublicKey = <contents of r2.pub>
AllowedIPs = 10.77.0.1/32
Endpoint = <r2 physical IPv4 address>:51820
PersistentKeepalive = 25
```

Start this configuration using your client's usual WireGuard tooling. Test
`ping 10.77.0.1`. For the existing r2 chat service, start `bg chat s eth` and
open `http://10.77.0.1:8080/messages` or its main page.

`ListenPort` defaults to 51820. `PresharedKey` is optional and must match on
both sides. A listening server learns the endpoint from an authenticated
handshake. An optional numeric IPv4 `Endpoint` makes r2 initiate the tunnel;
`PersistentKeepalive` on r2 accepts 0–60 seconds. `Address` is one IPv4 host
address with an optional `/0`–`/32` prefix; its prefix does not add routes.
`AllowedIPs` accepts up to 16 comma-separated IPv4 addresses or prefixes,
including `/0`. An address without a prefix means `/32`. Prefixes are
normalized to their network address and checked for both outgoing
destinations and authenticated incoming sources.

The daemon logs startup, session establishment and expiry to the console.
`/mnt/tmp/WGD.LOG` retains startup/session events and counters refreshed every five seconds: successful
handshakes, received/sent inner packets, drops, replay drops and session state.
Private keys and packet contents are never logged. Stop the background process
with the shell's normal task/kill commands; the kernel automatically releases
its tunnel registration and queued frames on exit, kill or crash.

## Scope

This first implementation accepts inner IPv4 ICMP echo and TCP packets for its
own tunnel address, from any source in the configured peer's AllowedIPs. It supports authenticated
endpoint roaming, optional preshared keys, replay protection, handshake cookies,
bounded handshake work, keepalives and session rekeying/expiry. Replay checks
include empty keepalives and run before endpoint or session updates.

There is no LAN/Internet forwarding, NAT, IPv6, inner UDP delivery, fragmented
IP reassembly, config reload or `wg` control socket. Existing TCP servers using
libcr2 preserve the incoming tunnel destination in their socket and reply from
that address. New outbound application connections need a network stack that
can select the tunnel source address; the daemon does not change the machine's
physical address or default route. The shell probe commands explicitly select
the tunnel source address. Oversize/fragmented packets are dropped;
clients should use MTU 1420. A full daemon receive queue never falls back to
transmitting plaintext on the physical NIC.

The kernel drops packets received directly from the NIC with the r2 tunnel
address as source or destination, preventing them from bypassing authentication or impersonating
queued tunnel output. Its keys and protocol remain entirely in
the userspace daemon. This is an initial port, not a security-audited VPN product.

## Verification

`make check` builds the r2 ELF and runs host tests for independent X25519,
BLAKE2s and ChaCha20-Poly1305 vectors, configuration validation, handshakes,
encrypted echo, TCP injection, replay checks, spoof rejection, rekeying, key
expiry and timer wraparound. `make kernel-test` checks tunnel ownership,
demultiplexing, bounds, injection, CIDR routes, ICMP probe ownership/quoted
replies, and route removal. `make sanitizer-test`
runs the C tests under Clang AddressSanitizer and UndefinedBehaviorSanitizer.
`make entropy-test` exercises the freestanding fallback and injects a stopped
timer at startup and after initialization, verifying rejection and zeroed
output on failure. Mock timer support is excluded from the production ELF.
In environments that prohibit LeakSanitizer's process inspection, use
`ASAN_OPTIONS=detect_leaks=0 make sanitizer-test`; the tested engine allocates
no memory.

For independent interoperability testing, build [tests/reference.go](tests/reference.go)
inside a checkout of official `wireguard-go` (tested revision
`2631ce99a06f27120d581611cf125d68bc6aa565`):

```sh
cd /tmp/wireguard-go
CGO_ENABLED=0 go build -o /tmp/wgd-reference /path/to/c/wgd/tests/reference.go
cd /path/to/c/wgd
python3 tests/qemu.py --kernel /path/to/updated/kernel.elf --reference /tmp/wgd-reference
```

The script builds a scratch ISO and floppy, generates fresh test keys and a
preshared key, boots r2 with `eth`, `chat` and `wgd`, and tests five encrypted
pings (through MTU 1420) plus three TCP/HTTP requests, including complete chat
pages, through official WireGuard's userspace IP stack. It also checks ordinary
Ethernet HTTP and verifies that no plaintext tunnel packet appears on the NIC.
It uses temporary localhost UDP/TCP forwards and needs no root, host TUN/TAP device,
host routes or permanent network changes. `--long` also checks traffic after
125 seconds, exercising rekeying. `--no-rng` verifies interoperability through
the Jitterentropy fallback with both CPU random instructions disabled.
Artifacts, private test keys, logs and packet
capture stay in the selected `--workdir` (default `build/qemu`). Prerequisites
are QEMU, `grub2-mkrescue`, mtools and Python's `cryptography` package.

For hosted-shell tests with a routed subnet, build [tests/router.go](tests/router.go)
in the same official `wireguard-go` checkout, then:

```sh
make -C ../r2sh build
make hosted-test-driver
python3 tests/qemu.py --routed --kernel /path/to/updated/kernel.elf --reference /tmp/wgd-router
```

This tests visible startup errors, `/24` and `/25` AllowedIPs, four outgoing
pings, an intermediate ICMP traceroute hop, updated counters, and clean hosted
shell exit. `--config /path/to/private.cfg` substitutes a real configuration
and tests its remote `10.4.6.68`; it creates no simulated peer in that mode.
Private configuration copies remain in the protected scratch directory.
