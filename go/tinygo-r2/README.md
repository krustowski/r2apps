# The `r2` TinyGo target

TinyGo reads its standard library and target definitions from `TINYGOROOT` at
compile time rather than baking them into the binary, so a new target is five
files dropped into the stock image.  There is no LLVM rebuild and `make image`
takes seconds.

```shell
make image     # docker build -t tinygo-r2:0.38.0 .
```

| File | Installed as | What it is |
| ---- | ------------ | ---------- |
| `r2.json` | `targets/r2.json` | The target definition. |
| `r2.ld` | `targets/r2.ld` | The memory map. |
| `r2.S` | `targets/r2.S` | `_start`, the syscall trampoline, the argv stash. |
| `runtime_r2.go` | `src/runtime/runtime_r2.go` | The runtime's board-support layer. |
| `interrupt_r2.go` | `src/runtime/interrupt/interrupt_r2.go` | No-op interrupt shims. |

## Why a target and not just a linker script

An earlier attempt built for the host `linux/amd64` target and passed a custom
linker script.  That does produce a binary that loads at `0x600000`, but it is
still the *Linux* runtime: it drags in musl, and its heap, its console and its
exit path are Linux syscalls, which `r2` does not implement.  The binary links
and then executes the `syscall` instruction on a kernel that has never heard
of it.

Building as a genuine baremetal target replaces all of that.  The heap comes
from the linker script, the console is syscall `0x10`, and the resulting
binary contains no `syscall` instruction at all --- only `int 0x7f`.

## What the runtime layer has to provide

TinyGo asks a baremetal target for a fixed, small set of functions; `r2`
answers them with syscalls:

| TinyGo wants | `r2` gives it |
| ------------ | ------------- |
| `putchar` | syscall `0x10`, buffered a line at a time |
| `getchar`, `buffered` | nothing --- keystrokes arrive through the kernel's pipes, not a console device |
| `ticks`, `ticksToNanoseconds`, `nanosecondsToTicks` | syscall `0x04`; a `timeUnit` is one millisecond |
| `sleepTicks` | syscall `0x05` |
| `exit`, `abort` | syscall `0x00` |
| `main` | called by `_start` after it switches stacks |
| `preinit` | nothing: the kernel's ELF loader zeroes `.bss` for us |
| `interrupt.Disable`/`Restore`/`In` | no-ops --- see below |

`interrupt.Disable` has nothing to disable.  Programs run in ring 3, where
`cli` and `sti` would fault, and they install no handlers of their own.  The
kernel does preempt the process on the PIT, but that is a full context switch
that restores every general-purpose register, not a callback into Go code, so
a critical section here has nothing to exclude.  The cooperative scheduler
switches goroutines only at explicit yield points.

## Bumping the TinyGo version

Change the tag in `Dockerfile` and in `IMAGE` in `../Makefile.tmpl`, then
rebuild.  The things most likely to need attention are the set of functions
the runtime expects (compare against a `runtime_*.go` for another baremetal
target, `runtime_arm7tdmi.go` is the smallest) and the build tag on
`runtime/interrupt/interrupt_none.go`, which is what decides whether this
target has to supply its own.
