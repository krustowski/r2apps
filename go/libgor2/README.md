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
| `types.go` | The structures the kernel reads and writes, and the assertions that keep them honest. |
| `system.go` | Exit, sysinfo, RTC, ticks, sleep, tasks, `Args`. |
| `console.go` | Print, clear, flush. |
| `fs.go` | Files, directories, mounts, `chdir`, fsck. |
| `video.go` | Framebuffer, VGA modes, blitting, the kernel font. |
| `audio.go` | Speaker and MIDI. |
| `net.go` | Ports, serial, packets, driver registration. |
| `input.go` | Keyboard and mouse pipes. |
| `mem.go` | The kernel's shared heap. |

## Calling convention

The syscall number goes in **RAX**, the first argument in RDI, the second in
RSI; the result comes back in RAX.  The kernel's dispatcher --- `syscall_inner(arg1,
arg2, syscall_no)` --- takes exactly two arguments, so there is no third one,
whatever `c/libcr2`'s four-argument `syscall()` prototype suggests.

Every syscall that takes a pointer checks it against `0x600000..0xA00000`.  All
Go memory satisfies this: globals, heap and stack all live inside the process's
private frame, and goroutine stacks come from that heap.

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
