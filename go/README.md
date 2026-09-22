# Go library (libgor2) and apps

| Project name | Purpose | State |
| ------------ | ------- | ----- |
| `libgor2` | The Go binding for the `r2` kernel ABI: every syscall, plus the types they read and write. | usable |
| `tinygo-r2` | The TinyGo target that makes Go run on `r2` at all --- runtime hooks, entry point, memory map. | usable |
| `example-print` | The minimal Go program: says who it is, what it was given, and what time the machine thinks it is. | stable |
| `icmpresp` | ICMP Echo responder over SLIP. A port of `c/icmpresp`, and the proof that a Go program can be a real `r2` service. | stable |

Go on `r2` is TinyGo, not the `gc` toolchain.  What you get is the whole Go
*language* --- slices, maps, strings, interfaces, closures, `defer`, `panic`,
**goroutines and channels** --- plus a garbage collector and a useful part of
the standard library, in a binary small enough to live in the 2 MiB frame the
kernel gives a process.  What you do not get is anything that needs threads,
`mmap` or signals, which is what the upstream runtime is built on.

## Quick start

The only thing that has to be installed on the host is Docker.

```shell
cd tinygo-r2 && make image     # once; builds the TinyGo image that knows about r2
cd ../example-print && make    # produces hello.elf
```

An application needs a two-line Makefile:

```make
NAME := hello

include ../Makefile.tmpl
```

There is no `linker.ld` to write and no `target.json` to maintain: both belong
to the target, in `tinygo-r2/`, and every application shares them.

Copy the `.elf` onto the floppy image and run it from the shell like any other
`r2` program:

```shell
mcopy -i fat.img example-print/hello.elf ::BIN/HELLO.ELF
# in the r2 shell
run HELLO
```

## How it works

TinyGo reads its standard library and its target definitions from `TINYGOROOT`
at compile time rather than baking them into the binary, so teaching it a new
target means dropping five files into an existing image --- no LLVM rebuild,
and `make image` finishes in seconds.  `tinygo-r2/` holds them:

| File | What it is |
| ---- | ---------- |
| `r2.json` | The target: baremetal amd64, cooperative scheduler, conservative GC. |
| `r2.ld` | The memory map (below). |
| `r2.S` | `_start`, the `int 0x7f` trampoline, and the argv stash. |
| `runtime_r2.go` | `putchar`, `ticks`, `sleepTicks`, `exit` --- what the runtime cannot do without. |
| `interrupt_r2.go` | The no-op interrupt shims the scheduler needs in ring 3. |

### Memory

Every process gets one private 2 MiB page at `0x600000`, and it has to hold the
program, its stack and its heap:

```
0x600000  .text / .rodata / .data / .bss
          _heap_start ... _heap_end      the collector's arena, ~1.5 MiB
0x7A0000  stack, 128 KiB, growing down from _stack_top
0x7C0000  left alone --- see below
0x800000  end of the frame
```

The 256 KiB below the top of the frame is deliberately unused.  The kernel
picks a process's initial stack from a fixed per-slot table, and the last two
slots are at `0x7F0000` and `0x7D0000` --- inside this frame.  A program
unlucky enough to land in slot 8 or 9 would otherwise have its argv frame
written straight through its own heap.

There is no guard page: the frame is a single 2 MiB huge page, so the finest
protection the hardware offers is the whole page.  A stack overflow runs
quietly into the top of the heap.

### What the runtime does with the ABI

| Runtime needs | Syscall |
| ------------- | ------- |
| `putchar` | `0x10`, buffered a line at a time |
| `ticks`, `nanotime`, `time.Now` | `0x04` (milliseconds since boot) |
| `sleepTicks` | `0x05` |
| `exit`, `abort` | `0x00` |
| heap | none --- the linker script provides it |

Console output is buffered and flushed at each newline and on exit, because the
kernel traces every syscall to the serial port: a syscall per character costs
far more than the printing does.  `libgor2.Flush()` forces a partial line out.

## What works

Verified on the real kernel under QEMU, not just compiled:

- `fmt`, including `%v`, `%q` and floating point (`fmt.Printf`, `fmt.Sprintf`)
- goroutines, channels, `select`, maps, slices, string concatenation
- the garbage collector (conservative, non-moving, ~1.5 MiB arena)
- `time.Now`, `time.Sleep`, `time.Since`
- `strconv`, `strings`, `bytes`, `sort`, `errors`, `math`, `unicode/utf8`, `sync`
- the whole of `libgor2`: sysinfo, RTC, ticks, mounts, task list, directory
  listing, file reads, serial, packets

A `println` hello world is about 10 KiB; the same program with `fmt` is about
110 KiB, which is what most of these examples cost.

## What does not

- **Threads.** `-scheduler=tasks` is cooperative and single-threaded.  A
  goroutine yields at channel operations, `time.Sleep` and `runtime.Gosched()`,
  and nowhere else --- a tight loop starves every other goroutine in the
  process.  (The kernel still preempts the process as a whole on the PIT.)
- **`os` and `net`.**  There is no file descriptor and no socket on `r2`; use
  `libgor2` instead.
- **`go` statements in a service that must not block.**  `libgor2.SleepMS`
  parks the whole process in the kernel, so every goroutine stops with it.
  `time.Sleep` sleeps one goroutine.

## Gotchas

### Taking the address of a local costs an allocation

This is the one that will bite, and it cost a day to find:

```go
func SerialRead() (byte, bool) {
	var v uint32                                        // heap-allocated!
	if Syscall(ScSerialPort, 0x02, ptr(unsafe.Pointer(&v))) != 0 {
```

The ABI wants an address as an integer.  TinyGo's escape analysis sees the
address of `v` disappear into a `uintptr` it cannot follow, gives up, and moves
`v` to the heap --- on *every call*.  In a polling loop that is an allocation
per iteration and a collection every few thousand, and the program spends its
whole life in the collector: the service above produced no output at all until
the local became a package-level cell.

So: in anything that runs in a loop, hoist the variable out, or make it a
package-level cell as `libgor2` does.  It is worth disassembling a hot loop
(`objdump -d`) and looking for `call runtime.alloc` before blaming the kernel.

### Floating point across a context switch

The kernel's timer interrupt saves the fifteen general-purpose registers and
nothing else --- no `fxsave`, so the SSE and x87 state is not preserved across
a context switch.  Two processes using floating point at the same time will
corrupt each other.  This is not specific to Go (a `gcc -O2` C program uses SSE
too), but Go leans on it harder.  The fix belongs in the kernel's
`timer_interrupt.asm`.

### One address space

Processes share a single address space above their own 2 MiB frame, and the
kernel's userland heap is 4 MiB shared by all ten process slots.  A Go program
holds its own heap inside its own frame and never touches the shared one, which
is why `libgor2.KMalloc` returns a `uintptr` rather than a pointer: memory from
there is invisible to the collector and must be freed by hand.

## Things found in the ABI along the way

Worth knowing whichever language you write in:

- The syscall number goes in **RAX**, arguments in RDI and RSI.  The kernel's
  dispatcher takes two arguments, so the third argument in `c/libcr2`'s
  `syscall()` prototype never reaches it.
- `c/libcr2`'s `serial_write()` passes the byte *by value* where the kernel
  expects a *pointer* to it, so it writes whatever lives at that address --- on
  the occasions the range check lets it through at all.
- The kernel's own comment on syscall `0x2F` says a `TaskInfo` is 20 bytes; the
  code writes 28.  `0x30`/`0x31` document `arg1` as a port number, but the code
  dereferences it as a pointer.  The code is right in both cases.
