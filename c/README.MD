# C library (libcr2) and apps

| Project name | Purpose | State |
| ------------ | ------- | ----- |
| `bsh` | The base shell: the commands `r2sh` and `tnt` share, compiled into both, and `.BSH` scripts (see its [README](bsh/README.md) and the example scripts in [`../bsh`](../bsh/README.md)). | unstable |
| `c8` | Tackx/c8 CHIP-8 emulator: raylib API backend in a Memento window, with bundled ROMs in `/mnt/tar/opt/c8` (see its [README](c8/README.md)). | experimental |
| `chat` | Chatroom server for multiple peers on TCP/9000, with an HTTP front end on TCP/8080 (see its [README](chat/README.md)). | stable |
| `cube` | Floating-point 3D cube: VGA mode 13h, or a Memento window from Mount / Files (see its [README](cube/README.md)). | stable |
| `eth` | The userland Ethernet driver: registers the NIC (RTL8139 or E1000), answers ARP and ICMP, gets an address by DHCP or `--ip` (see its [README](eth/README.md)). | stable |
| `fsck` | FAT12 filesystem check report parser. | stable |
| `garn` | Sample HTTP server implementation. | stable |
| `gfxtest` | VGA mode 13h test. | stable |
| `hello-fs` | Example with filesystem-related operations. | stable |
| `hello-world` | Example to show minimal C program crafted for use with the `r2` kernel. | stable |
| `icmpresp` | ICMP responder. Sample ICMP Echo service to demonstrate portions of implemented networking. | stable |
| `ipc` | Message passing between processes: waits for data on the kernel's port queue (syscalls `0x35`/`0x36`). | stable |
| `libcr2` | The C library: syscall wrappers, `printf`, strings, and a TCP/IP stack (see below). | usable |
| `mdnsd` | Multicast DNS responder: answers `<host>.local` and advertises the running r2 servers (GARN, TNT, streamd, RNTP, ...) over DNS-SD on UDP/5353, alongside `eth`, and browses for other machines' services for Memento's Network window (see its [README](mdnsd/README.md)). | experimental |
| `nsk` | Networking swiss knife. Lists live hosts in a subnet given in CIDR notation. | stable |
| `r2sh` | Custom userland shell for `r2`, on `bsh`. | unstable |
| `rntp` | RNTP/TCP RAM-disk server and client: FAT12/16 exports, `/mnt/net` mounting and cached free-space statistics (see its [README](rntp/README.md)). | experimental |
| `streamd` | Streams the displayed framebuffer, Memento windows included, as 640x480 MJPEG at up to 30 FPS on `http://<ip>:8080/stream`; skips unchanged frames with syscall `0x1d`'s metadata (see its [README](streamd/README.md)). | unstable |
| `tcc` | TinyCC ported to `r2`, with the small C library it and its programs link against (see its [README](tcc/README.md)). | unstable |
| `them` | A simple 16-bit CPU emulator focused on MS-DOS; runs full screen, or in a Memento window from its file manager (see its [README](them/README.md)). | unstable |
| `tnt` | TELNET server and remote shell on TCP/23, over SLIP or Ethernet, with `bsh`'s commands plus `get` and `net` (see its [README](tnt/README.md)). | unstable |
| `wgd` | Standalone WireGuard daemon: one IPv4 peer, AllowedIPs subnets, shell ping/traceroute and access to r2 TCP services, alongside `eth`; Jitterentropy fallback for CPUs without usable random instructions (see its [README](wgd/README.md)). | experimental |

## crt0

~~There is no C runtime version 0 (crt0) available yet. That's why the linker script is required to link compiled programs into ELF executables and to skip crt0. *This is a technical debt*~~

C runtime version 0 (crt0) is now available to modify in `libcr2/_crt0.asm`. Programs built by tcc on `r2` start from the C library's `tcc/libc/crt1.S` instead, whose `exit()` flushes stdio.

## libcr2 (cr2)

`libcr2` is a custom C library dedicated for use with the `r2` kernel. Mainly, it is built on the kernel syscalls implementation to provide basic functions like `sys_print`, etc.

To produce a statically linkable library/archive (`libcr2.a`), run:

```shell
make libcr2
```

### With a C library

libcr2 also goes with the C library in `tcc/libc` --- the one programs built by tcc on `r2` use. Its
headers notice that library by `<sys/r2libc.h>` on the include path (`libcr2/types.h`) and then leave the
C names to it: the integer types, `printf`, `strlen`, `memcpy`, and five calls of libcr2's own, which go
by other names there:

| Name as an app here sees it | Name next to the C library |
| --------------------------- | -------------------------- |
| `exit(pid, code)` | `r2_exit(pid, code)` |
| `chdir(path)` | `r2_chdir(path)` |
| `read(sock, buf, max)` | `tcp_read(sock, buf, max)` |
| `write(sock, buf, len)` | `tcp_write(sock, buf, len)` |
| `close(sock)` | `tcp_close(sock)` |

The names on the right exist in every build; the apps here, built with `-nostdinc`, have both.
