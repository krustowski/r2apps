# RNTP network disk

RNTP exports a FAT12/16 RAM disk from one R2 instance and mounts it at
`/mnt/net` on multiple R2 clients or through a Linux FUSE mount over TCP. This application contains both the server and
client modes; the kernel supplies the block-request bridge through syscall
`0x47`.

## Build

From this directory:

```sh
make build
make test
make install R2_MAIN=/path/to/r2_main
```

`build` produces `rntp.elf`, using `../libcr2.a` and the common C linker script.
`install` stages it in the kernel repository's `iso/bin`; `R2_MAIN` defaults to
the sibling `r2_main` checkout. Kernel-side `make build_rntp` and
`make build_iso` also build and stage this application. Rebuild the boot archive
and ISO after installation.

## Run

After ETH has configured the machines' addresses:

```text
# Server: create a 16 MiB FAT16 export
bg rntp serve 16

# Client: connect to the server and mount /mnt/net
bg rntp connect 192.0.2.1
```

`serve 2` formats FAT12; larger sizes use FAT16. Sizes are 2–64 MiB, subject to
available memory. The optional last argument selects a TCP port; the default
is 7780. Up to seven updated clients share one filesystem, with locks around
FAT operations and revision-based cache invalidation. An extra undo image
lets disconnects abort unfinished writes. Legacy clients keep an exclusive
lease and cannot coexist with shared sessions. The RAM image survives
client reconnects and is lost when the server exits or reboots. Connections
trust the selected network; authentication and encryption are not implemented.

Mount information reads a kernel cache initialized before mounting. The client
refreshes free space every two seconds with STAT requests, so shell mount
listings and Memento's Files window do not wait for remote FAT scans.

`protocol.c` owns the wire codec and server request handler. `bridge.h` defines
the kernel ABI. `tests/protocol.c` tests FAT12/16 statistics, request framing,
bounds and generation checks. Kernel-side bridge/filesystem tests and the
two/three-guest QEMU regressions remain in `r2_main/tests/netdisk`.

See `r2_main/docs/networking/rntp.md` for the full wire protocol, bridge ABI and
connection lifecycle.

The updated peers negotiate write batching: up to 32 sectors per request,
with repeated FAT edits and zero/data writes coalesced within a filesystem
operation. The kernel flushes the operation before its syscall returns, so
success still requires a remote acknowledgment. TCP uses a 32 KiB receive
window and several segments in flight, with retransmission at the original
sequence numbers. Both peers need the new app for batching; legacy clients
retain single-sector access.

`make test` covers batch bounds, all-or-nothing validation, free-space
reporting, larger TCP receive windows and socket reuse. The build reserves
a 512 KiB stack and checks that the entire image fits R2's 2 MiB process
mapping. It also refreshes the SDK archive before linking.


## Linux mount

Install `fuse3-devel` (Fedora) or `libfuse3-dev` (Debian/Ubuntu), a C compiler,
`pkg-config` and `fusermount3`. The Linux target does not require libcr2 or
an R2 kernel build:

```sh
make linux
mkdir -p ~/rntp
./rntp-fuse 192.0.2.1 ~/rntp -f
# In another shell:
cp picture.bmp ~/rntp/PICTURE.BMP
fusermount3 -u ~/rntp
```

Use `-p PORT` for a different port; omit `-f` to daemonize. The server needs
the shared RNTP extension. The mount reads and writes the existing FAT12/16
filesystem, including directory creation, rename/replacement, delete,
truncate, sparse zero-fill and atomic append per FUSE write request. Names
are case-insensitive 8.3; long names, symlinks, chmod/chown, distributed file
locks, mmap and POSIX lifetime of remotely renamed/deleted open files are
unsupported. Modification times use UTC DOS timestamps (two-second precision).
FAT permissions are fixed to the mounting user's files/directories. Data and
attributes bypass the Linux kernel caches; transport caches refresh on each
foreign revision. `fsync` confirms RAM writes, not durable persistence.

`make test-linux` runs two concurrent TCP clients against the production
session handler and tests rollback on ENOSPC and disconnect. `make test-fuse`
mounts two temporary FUSE filesystems for FAT12 and FAT16 and checks shared
writes, concurrent append, rename/delete and directory growth. It requires
`/dev/fuse` and the host's normal FUSE mount permission. `fsck.fat -n` verifies
the resulting images when dosfstools is installed. The kernel repository's
`tests/netdisk/qemu_linux.py` tests actual Linux mounts against the R2 server;
`qemu_pair.py --shared` tests two R2 clients against one R2 server.


R2 downloads negotiate READ_BATCH and fetch up to 32 sectors per exchange.
The updated kernel accepts all sectors through bridge operation 12 so a
16 KiB image-loader read avoids 32 daemon wakeups. Its read buffer is separate
from the FAT cache and is cleared between filesystem operations and on writes
or failure. Bridge operation 13 detects support; old kernels use daemon
read-ahead with existing sector completions. Old servers retain sector reads.
Update the client kernel and both RNTP apps for the full optimization. The
fmimage executable and job ABI do not need to change.
