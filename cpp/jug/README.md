# Jug

Jug is r2's C++ program manager, built with libc++r2. It compares local ELF
programs with a remote SHA-256 list, downloads verified updates to the RAM disk,
and can restart every running instance with its original arguments.

The **Jug** desktop icon opens a Memento window hosted by **JUG.ELF** in its own
process. Memento supplies input, displays indexed frames and owns the shared
memory; closing the window shuts down the child before releasing that memory.
Jug shares Memento's HTTP/TLS engine but has its own network ports.

## Build and run

```sh
make                         # jug.elf and jug.bin
make check                   # host tests using the real libc++r2
make -C ../memento-hello build
```

The kernel's `make build_iso` builds Memento and Jug, places Jug in `iso/bin`,
and copies `configs/jug.cfg` to `iso/opt/jug/jug.cfg`. GRUB's USB archive exposes
that configuration as `/mnt/tar/opt/jug/jug.cfg`.

On the kernel console, or in the hosted shell through its `fg`/`bg` commands:

```text
fg jug update
fg jug list
fg jug install tnt
fg jug upgrade
fg jug restart tnt
fg jug remove tnt
fg jug sum /mnt/tar/bin/tnt.elf
```

`update` fetches the list and checks local files. `upgrade` downloads updates to
installed programs; `install` also downloads programs that are new to this
machine. Downloads never stop running instances automatically. `restart` uses
the task table and saved command lines (syscall `0x41`) to stop and start all
instances. If their arguments cannot be read, they remain running. Jug protects
itself and, when hosted, Memento from restart.

In the window, **Update/U** refreshes the list, **Get/G** downloads the
selected program, **Get all/A** downloads available updates, **Restart/R** then
**Y** restarts its instances, **Remove/Del** removes its downloaded copy, and
**Ctrl+C** copies its full SHA-256. The list shows the publication timestamp,
program name, size, checksum prefix, local origin, update status and running
PIDs. Escape cancels a transfer; Escape when idle closes the window.

**Tab** cycles from the program list through the six bottom buttons and back;
**Shift+Tab** cycles backward. The focused button is highlighted, and
**Enter/Space** activates it. **Left/Right** move between buttons. List navigation
keys (**Up/Down**, **Page Up/Down**, **Home/End**) return focus to the list;
**Enter** there downloads the selected program. Clicking a row or button also
sets keyboard focus. Restart still requires **Y** to confirm.

## Configuration

The default repository is `https://cdn.vxn.dev/jug`, with `sums.txt` below it.
Jug reads the first configuration present in this order:

1. `/mnt/fat/JUG.CFG`
2. `/mnt/tar/opt/jug/jug.cfg`
3. `/mnt/iso/opt/jug/jug.cfg`

```ini
repo = https://cdn.vxn.dev/jug
list = sums.txt
insecure = 0
```

`list` can instead be `list.txt`, a relative path, or an absolute HTTP(S) URL.
Program paths are resolved against the list's directory. Console options
`--repo URL`, `--config FILE` and `-k` override the repository, configuration
file and TLS certificate checking. HTTPS verifies certificates using the same
`opt/memento/cacerts.bin` bundle as Web; the ISO includes it. Networking requires
the `eth` userland driver to be running.

## Publishing a repository

Place ELF programs under the directory served as `/jug`. Generate its list:

```sh
make sums DIR=/path/to/published/jug
# Or:
python3 mksums.py /path/to/published/jug --output /path/to/published/jug/sums.txt
```

The generator includes nested `.elf` files, rejects unsupported or duplicate
program names, and replaces an existing list only after generation succeeds.
Serve the list and its binaries from the same directory:

```text
# updated 2026-10-08 12:34:56 UTC
# SHA-256  bytes  path (relative to this list)
<64 hex digits>  78968  tnt.elf
<64 hex digits>  45312  bin/sh.elf
```

Jug also accepts ordinary `sha256sum` output without byte sizes. It uses the
HTTP `Last-Modified` header when the list has no `# updated` timestamp. Cached
lists are associated with their source URL so changing repositories cannot use
another repository's cached catalog.

## Storage and executable lookup

Programs are stored as `/mnt/tmp/jug/TNT.ELF`, with an eight-character base name
at most. The kernel searches this directory **before the working directory**,
then falls back to `/mnt/tar/bin` and `/mnt/iso/bin`. This applies to `fg`, `bg`
and the ELF spawn syscall. Removing a download restores the shipped version.
The RAM disk and its registry are reset on reboot.

`JUG.REG` tracks each program's path, byte size and SHA-256; `SUMS.TXT` caches
the last fetched catalog. Immutable shipped binaries can reuse cached digests;
writable downloads are rehashed when inspected. Before installation Jug checks
the size, full SHA-256 and x86-64 ELF headers and segments. It writes a `.NEW`
file, reads it back, and publishes it using subdirectory renames, retaining the
previous executable for rollback. Replacement needs space for both copies.
Downloads are limited to the HTTP engine's 4 MiB body limit.

## Tests

`make check` covers standard SHA-256 vectors, catalogs, damaged ELF files,
registry round trips, config precedence, same-size file changes, source-specific
caches, failed writes and readback, replacement rollback, and multi-instance
restarts. The manifest generator is tested against real filesystem fixtures.

`make -C tests target` builds `tests/jugtest.elf` and `tests/shipped.elf` for a
kernel smoke-test ISO. Put them in its archive as `bin/jugtest.elf` and
`bin/jprobe.elf`, include the default `opt/jug/jug.cfg`, and boot the text kernel
with a floppy `INIT.RC` containing `fg jugtest` (or type that command in its
console). Success prints `JUG TARGET PASS`.
This checks real RAM-disk writes/renames, download precedence even from the
shipped binary's directory, restarts preserving two instances' arguments, and
fallback to the shipped binary after removal.
