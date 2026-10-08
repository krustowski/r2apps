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

Until someone gives Memento a login or a password, the system is `root` with no password and a
connection gets the shell straight away, as it always did.  Memento's first login since boot that
gives credentials leaves a salted hash of its login and password in `/mnt/tmp/SESSION.CFG`, and from
then on a **new** connection is asked for that pair first:

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
CR NUL (a telnet client sending a character at a time) or LF.  Every option a telnet client
offers or asks for is refused (`WONT`/`DONT`), but for the server's own ECHO: a client left to
believe it has SUPPRESS-GO-AHEAD goes to a character at a time once the echo is switched, and then
shows Enter as `^M` with the prompt on the same line.  Refused, it stays in line mode.
Three wrong tries close the connection.  A SESSION.CFG that is there but cannot be read lets
nobody in; one that holds the default credentials (empty, or `root` with no password, which an
older Memento wrote for a login left empty) is no session, and asks nothing.  `auth.c` has its own
SHA-256 for this and computes the hash the way Memento does.

The prompt is `user@host:cwd> `, from the kernel's sysinfo: the user is the one Memento's login set,
`root` before that.

## Directories: `ls`, `cd`

A session starts in `/mnt/fat` when there is a floppy to read, and in `/` otherwise (booted from
USB: the kernel keeps `/mnt/fat` in its mount table with no disk in the drive).  Each session has
its own working directory.  `/` and `/mnt` are listed from the mount table, each mount point with
its filesystem (`fat/  <DIR>  fat12`); everything below a mount point is the kernel's listing.
Paths may use `.` and `..` (`cd ..` from `/mnt/fat` is `/mnt`), and `cd` alone goes back to
where the session started.

## Making directories: `mkdir`

```
mkdir <path>
```

Makes a directory in the current one, or wherever the path says (`mkdir /mnt/tmp/WORK`,
`mkdir NEWDIR/INNER`), on the floppy (`/mnt/fat`) or the RAM disk (`/mnt/tmp`): which mount a path
is on comes from the kernel's mount table, so `/`, `/mnt`, the CD and the tar archive are refused.
The name is up to eight letters, digits or `!#$%&'()-@^_`{}~`, without a dot (the kernel lists a
directory `A.B` as `A`).  Syscall 0x27 does not say whether the directory came to be, so `mkdir`
looks first (an existing name is refused) and afterwards (and says so if it is not there).

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
