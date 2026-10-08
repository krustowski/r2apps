# tnt

A TELNET server and remote shell.

| Argmuent | Pair value (if any) | Meaning | Required |
|----------|---------------------|---------|----------|
| `debug`   | *none*        | Starts in verbose mode. | No |
| `--net` | `eth`/`slip`         | Sets the networking driver (serial SLIP or Ethernet) | No, default is SLIP. |

## Example

```sh
bg TNT eth
```

Use `eth` (or `--net eth`) for telnet over Ethernet; without it TNT uses
serial SLIP. Memento's network port list shows kernel TCP bindings, including
outbound client ports, rather than only listening servers.

The kernel must support releasing port bindings via syscall `0x37`, arg2 = 1,
for this version's file-transfer cleanup. Update the kernel together with TNT
and the Go network applications. A full binding table now reports an error
instead of evicting port 23; Go connections release their bindings on close.
## Logging in

Until someone logs in to Memento, a connection gets the shell straight away, as it always did.
Memento's first login since boot leaves a salted hash of its login and password in
`/mnt/tmp/SESSION.CFG`, and from then on a **new** connection is asked for that pair first:

```
Memento is logged in: its login and password, please.

login: bob
password:
bob@rourex:/mnt/fat>
```

Connections that were already in the shell stay there.  The password is not shown to a telnet
client: `tnt` sends TELNET `WILL ECHO` before asking for it, so the client stops echoing, and
`WONT ECHO` after.  Only to a client that has sent TELNET commands itself, which `telnet` does as it
connects to port 23 (to another port only with `open host -port`); `nc` would print those bytes,
so it gets none, and its terminal shows the password as it is typed.  A line may end in CR LF,
CR NUL (a telnet client sending a character at a time, as it does while the server echoes) or LF.
Three wrong tries close the connection.  A SESSION.CFG that is there but cannot be read lets
nobody in.  `auth.c` has its own SHA-256 for this and computes the hash the way Memento does.

The prompt is `user@host:cwd> `, from the kernel's sysinfo: the user is the one Memento's login set,
`root` before that.

## Showing files: `read`

```
read <path>
```

Reads the whole file into the user heap first (it grows past its first 4 MiB when it has to; up to
8 MiB here, `get` is the way for more), then sends it down the session the way `get` sends a
download: following the client's ACKs and sending again whatever went missing, since a file is
far more than one burst of segments and libcr2's TCP does not retransmit.  `\n` becomes `\r\n`,
and for a telnet client a 0xff byte is doubled.  A key pressed while it is being sent stops it once
what is already out has arrived, and says how far it got.  It used to read the file with
`read_file()` into a 4 KiB buffer, which a bigger file (256 kB) overran, and tnt crashed.

## Downloading files: `get`

```
get <path> [port]
```

Sends one file from any mounted filesystem (`/mnt/fat`, `/mnt/iso`, `/mnt/tar`, relative paths
resolve against the current directory) to another computer over a separate data connection on
port 8023 (or `[port]`).  `tnt` prints the commands to run and waits 60 s for the connection;
pressing Enter in the telnet session cancels.

```sh
curl -o FILE.BIN http://<r2-ip>:8023/        # or wget, or a browser (HTTP, with Content-Length)
nc <r2-ip> 8023 < /dev/null > FILE.BIN        # raw bytes; ncat also: nc --recv-only ...
```

`nc` with its stdin left open (a terminal) keeps waiting after the file is complete: that is
ncat's behaviour, hence `< /dev/null` or `--recv-only` (OpenBSD nc: `-d`).

When done, `tnt` reports the byte count and the CRC-32 of what the receiver acknowledged, to
compare with e.g. `python3 -c 'import zlib,sys; print("%08x" % zlib.crc32(open(sys.argv[1],"rb").read()))' FILE.BIN`
(or `rhash --crc32`).

libcr2's TCP never retransmits, so the file is not simply written into a socket: `get` follows
the receiver's ACKs itself and sends again from the first unacknowledged byte after a timeout or
three duplicate ACKs (go-back-N, re-reading the file with `read_file_at`), with a small
congestion window so a NIC that drops bursts is not flooded.  See `xfer.c`.  The telnet session
does not answer while a transfer runs.
