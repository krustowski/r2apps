# libgor2

The Go binding for the `r2` kernel ABI.  It is to Go what `c/libcr2` is to C:
every syscall the kernel offers, and the types they read and write.

```go
import "github.com/krustowski/rou2exOS-apps/go/libgor2"

var info libgor2.SysInfo
if err := libgor2.ReadSysInfo(&info); err != nil {
	fmt.Printf("sysinfo: %v\n", err)
}
```

| File | Covers |
| ---- | ------ |
| `syscall.go` | The `int 0x7f` entry point, syscall numbers, error codes. |
| `abi.go` | TinyGo assembly and runtime hooks, including `Syscall3`. |
| `types.go` | The structures the kernel reads and writes, and the assertions that keep them honest. |
| `system.go` | Exit, sysinfo/user, RTC, ticks, sleep, tasks, command lines, desktop relaunch, power, `Args`. |
| `console.go` | Print, clear, flush. |
| `fs.go` | Files, directories, mounts, `chdir`, fsck. |
| `video.go` | Framebuffer, VGA modes, RGB/indexed blitting, presentation transactions, captures, the kernel font. |
| `audio.go` | Speaker, MIDI and nonblocking HD Audio PCM output. |
| `net.go` | Ports, serial, packets, driver registration. |
| `input.go` | Keyboard and mouse pipes. |
| `mem.go` | The kernel's shared heap, `KBytes`, memory info. |
| `stack.go` | Goroutine stack depth and reuse counters, the stack cache limit. |
| [`memento/`](memento/README.md) | Hosted-window commands, snapshots, heartbeats, shutdown and runtime reporting, with a C++ host helper. |

## Memento windows

Use [`libgor2/memento`](memento/README.md) for a Go app that runs inside a
`cpp/memento-hello` window. Memento owns the window and paints snapshots; the
Go process owns the app state and consumes commands. The package extracts the
shared transport used by Spotify, preserving its version 4 ABI, and includes
a small counter app as a starting point. It can be tested with stock Go;
the kernel syscall bindings in this directory still require TinyGo.

## Calling convention

The syscall number goes in **RAX**, the first argument in RDI, the second in
RSI, and an optional third in RCX; the result comes back in RAX. `Syscall`
clears RCX, while `Syscall3(number, arg1, arg2, arg3)` supplies it. The
trampoline also places the number in RDX for older kernels, matching libcr2.
Rebuild the `tinygo-r2` image after updating these bindings.

Every syscall that takes a pointer checks that the whole buffer it uses lies
inside one user region: the image and user stacks (`0x400000..0xA00000`) or the
kernel's shared user heap (`0xC00000..0x1000000` and any extension). Globals,
the system stack and the default Go arena live in the private frame. The
optional `r2largeheap` arena and blocks from `KMalloc` also satisfy the checks.
`KBytes` turns a `KMalloc` block into a slice any call here will take:

```go
addr := libgor2.KMalloc(1 << 20) // 1 MiB the collector never sees
defer libgor2.KFree(addr)

n, err := libgor2.ReadFileAt("/mnt/fat/BIG.DAT", libgor2.KBytes(addr, 1<<20), 0)
```

The check is against the region, not the slice: the kernel is still not told
how long most buffers are, so a slice shorter than what a syscall writes is
overrun into whatever follows it.

## Indexed frames and capture

`BlitIndexed(pixels, palette, width, height, firstRow, rows, clear)` draws
selected rows of an 8-bit frame. Supply the complete frame and 256 RGB palette
triples; the kernel centers the image at the largest integer scale that fits.
`IndexedAvailable` probes framebuffer support.

When `IndexedPresentAvailable` returns true, call `BeginIndexedPresent`, draw
the row bands, then call `EndIndexedPresent` with the complete composed frame.
Call `CancelIndexedPresent` if drawing fails. This protocol lets updated kernels
serve stable 640x480 captures from a completed snapshot in RAM.

`CaptureFramebuffer` fills a screen-sized `[]uint32` with `0x00RRGGBB` pixels
and requires a 32bpp framebuffer. `CaptureFramebufferRGB24Scaled` fills a
`[]byte` with tightly packed RGB triples at dimensions from 1 to 65535.
Both validate destination capacity. If either returns `EBusy`, discard the
captured contents and retry later.

`CaptureFramebufferRGB24ScaledIfNew` also accepts `*FBCaptureInfo`. Set
`FrameID` to zero to force a copy, or retain the last accepted ID to skip an
unchanged 640x480 indexed snapshot. `EUnchanged` leaves the pixels untouched;
metadata identifies the leased frame and its timestamp in milliseconds.
`Flags & FBCaptureInfoSnapshot` identifies a stable RAM snapshot. Old kernels
perform an ordinary capture and leave the output flags/timestamp zero.

## Process and system controls

`CommandLine(pid)` reads up to 128 bytes of the original launch command,
without assuming a terminating NUL. `RequestDesktopRelaunch(pid)` asks a
registered Memento running under the graphics-session supervisor to close
cooperatively and relaunch. Only that Memento can register or poll with
`RegisterDesktopRelaunch` and `DesktopRelaunchPending`; other apps receive
`ENotImplemented`. `Reboot` and `PowerOff` do not return on success.

`SetUser` changes the system user to a single printable ASCII word.
`WriteNetConfig` publishes network configuration from the global driver.
Filesystem helpers support the floppy, writable FAT RAM disk, read-only
ISO/TAR volumes and mount directories such as `/` and `/mnt`.

## Structure layouts

The kernel writes these structures directly into our memory, so each one has to
match the packed C layout byte for byte.  Go has no `packed`: it aligns every
field to its own width, and three of the ABI structures put a wide field at an
odd offset.  For those --- `RTC.Year`, `VfsDirEntry.Size`, `TaskInfo.RIP` ---
the field is held as bytes behind an accessor.

The bottom of `types.go` asserts the size of every one of them at compile time.
Get a layout wrong and the package stops compiling, rather than quietly handing
the kernel a pointer to the wrong shape:

```go
_ = uint(unsafe.Sizeof(RTC{}) - 7)
_ = uint(7 - unsafe.Sizeof(RTC{}))
```

## Scratch cells

`net.go` keeps package-level cells for the syscalls whose argument is a pointer
to a single scalar, rather than taking the address of a local.  This is not a
micro-optimisation: see "Taking the address of a local costs an allocation" in
[../README.md](../README.md).  Nothing in this package is re-entrant, which is
safe because the scheduler is cooperative and a syscall is not a yield point.

## Errors

A syscall that fails returns an `Errno`, which implements `error`, so the
familiar shape works:

```go
if err := libgor2.Chdir("/mnt/fat"); err != nil {
	// libgor2.EFileNotFound, libgor2.EInvalidInput, ...
}
```

Calls that answer with a count or a handle rather than a status --- `Ticks`,
`ListTasks`, `Run`, `Receive` --- return that value directly.

## Verification

Run `make test` here for stock-Go ABI and networking regression tests. The
`r2abimock` tag replaces assembly/runtime hooks only on the host; an `r2`
build always selects the real hooks. Unexpected mock syscalls panic.
Native application builds use the rebuilt TinyGo image.

For an isolated VM, build `./libgor2/tests/native` as `ABI.ELF` with the r2
target and place it on a FAT floppy alongside this `INIT.RC`:

```text
cd /mnt/fat
fg ABI --smoke
```

Boot the current kernel with an E1000 or RTL8139 device and QEMU debug output
enabled (`-debugcon file:abi.log -global isa-debugcon.iobase=0xe9`). The test
reports `GO ABI CHECK PASS` after checking argv, sysinfo/user changes, process
command lines, desktop/power error returns, VFS mount directories, RAM-disk
I/O, local pings through a separate driver process, registration cleanup and
port binding. A 32bpp graphics boot also verifies capture metadata and unchanged
snapshot detection through the real three-argument trampoline.
With `-device isa-debug-exit,iobase=0xf4,iosize=0x04`, a passing test stops
QEMU with exit code 33. Other display formats report the capture check as skipped.
