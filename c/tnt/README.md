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
