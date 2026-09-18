# libc++r2

A C++ runtime and standard library for the [`r2` kernel](https://github.com/krustowski/rou2exOS),
in the same spirit as `c/libcr2`: no host libc, no libstdc++, nothing but the
kernel ABI underneath.

```cpp
#include <r2.hpp>

int main(int argc, char **argv) {
    r2::vector<r2::string_view> args;
    for (int i = 0; i < argc; i++)
        (void)args.push_back(r2::arg(i));

    r2::sort(args.begin(), args.end());
    r2::println("hello from C++ on r2, ", args.size(), " arguments");
    return 0;
}
```

```
make                 # libc++r2.a, libc++r2compat.a, _crt0.o
make check           # host-side tests: allocator, containers, formatting
make examples        # examples/hello and examples/gfxdemo
```

An application needs a two-line Makefile:

```make
NAME := hello
include ../../Makefile.tmpl
```

`make build` produces `hello.elf` and `hello.bin`; `make install` copies
`HELLO.ELF` onto `fat.img`.  Run it from the r2 shell with `fg HELLO` --- note
that `fg` takes the name without the extension and rejects anything longer than
eight characters.

## What is in it

| Header | What it gives you |
| ------ | ----------------- |
| `r2/types.hpp` | fixed-width types, spelled to match `libcr2/types.h` so both headers can be included together |
| `r2/new.hpp`, `r2/memory.hpp` | `operator new`/`delete`, `unique_ptr`, `construct_at`, uninitialised algorithms |
| `r2/vector.hpp`, `r2/string.hpp`, `r2/string_view.hpp`, `r2/array.hpp`, `r2/span.hpp`, `r2/optional.hpp`, `r2/function.hpp` | the containers |
| `r2/algorithm.hpp`, `r2/utility.hpp`, `r2/type_traits.hpp` | `sort` (introsort), `find`, `lower_bound`, `move`, `forward`, `pair`, the traits the containers need |
| `r2/io.hpp` | `print`, `println`, `printf("{}")`, `format`, `concat` --- type-safe, no varargs |
| `r2/heap.hpp` | the arena allocator, and its statistics |
| `r2/syscall.hpp` | the raw ABI: syscall numbers, the kernel's structures, `raw_syscall` |
| `r2/fs.hpp` | files and directories |
| `r2/gfx.hpp` | `Canvas`, `Font`, the VESA framebuffer, VGA mode 13h |
| `r2/input.hpp` | keyboard and mouse |
| `r2/time.hpp` | ticks, sleep, the RTC, `Stopwatch`, `FrameTimer` |
| `r2/process.hpp` | arguments, `exit`, `panic`, sysinfo, the task table, `spawn` |
| `r2/net.hpp` | addresses, byte order, frames, port binding |
| `r2/audio.hpp` | the PC speaker |
| `r2/math.hpp` | `sqrt`, `sin`, `cos`, `floor`, `fmod`, ... since there is no libm |

It is C++17, built `-fno-exceptions -fno-rtti -nostdinc -nostdinc++`.  There is
no `std::` namespace beyond the three things the language itself requires:
`std::initializer_list`, `std::nothrow` and `std::align_val_t`.

### No exceptions, so failure is in the return value

Every operation that has to allocate says whether it worked:

```cpp
r2::vector<int> v;
if (!v.reserve(1000)) { r2::println("out of arena"); return 1; }
(void)v.push_back(1);                 // [[nodiscard]]; ignore it deliberately

auto file = r2::fs::read_text("/README.TXT");
if (!file) { r2::println("no such file"); return 1; }
```

`operator new` returns `nullptr` rather than throwing, and the containers check
it.  `r2::string` is the one exception to the pattern: `append` and `+=` keep
the string unchanged on failure and set a flag, so that building a message with
several appends stays readable --- ask `failed()` if it matters.

## Memory map

This is the part worth understanding before writing anything large.

The kernel gives each process a **private 2 MiB frame at `0x600_000`**, and
refuses to load any ELF segment that ends past `0xA00_000`.  Between
`0x800_000` and `0xA00_000` the memory is shared and identity-mapped, and other
processes' stacks live there --- so a program whose `.bss` reaches that far is
scribbling on its neighbours.  Everything libc++r2 sets up by default stays
inside the private frame:

```
0x600_000  +---------------------------+
           |  .text  .rodata  .data    |   the program
           +---------------------------+
           |  stack     512 KiB        |   _crt0.asm, R2_STACK_BYTES
           +---------------------------+
           |  heap arena 512 KiB       |   heap.cpp, R2CXX_ARENA_BYTES
0x800_000  +---------------------------+   <-- stay below this line
           |  other processes' stacks  |
0xA00_000  +---------------------------+   <-- the kernel rejects segments past here
```

Both sizes are build-time settings:

```
make ARENA_BYTES=262144 STACK_BYTES=262144
```

and a single application can override the arena without rebuilding the library:

```cpp
R2_HEAP_ARENA(1024 * 1024)      // at file scope, in exactly one .cpp
```

### Why the heap is an arena and not the kernel's

The kernel has a 4 MiB heap of its own at `0xC00_000` (syscall `0x0a`), and
libc++r2 does not use it for `operator new`.  Every syscall that takes a
pointer range-checks it against `0x600_000-0xA00_000` and returns
`InvalidInput` for anything else, so a string built on the kernel heap can be
allocated but never printed, written to a file, or blitted to the screen.  The
arena lives in `.bss`, inside the image, which is inside the range the kernel
accepts.

The kernel heap is still reachable through `r2::heap::kernel_allocate()` for
large scratch buffers that never cross the ABI boundary.

The arena allocator is a first-fit free list with boundary tags: blocks are
coalesced with both neighbours on free, so a loop that allocates and frees
stays flat rather than marching off the end of a bump pointer.
`r2::heap::stats()` reports what is in use, what is free, and the largest block
that can still be handed out.

## The syscall number goes in two registers

`r2::raw_syscall` loads the syscall number into **both `RAX` and `RDX`**:

```cpp
asm volatile("int $0x7f"
             : "=a"(ret)
             : "a"(number), "d"(number), "D"(arg1), "S"(arg2), "c"(arg3)
             : "r11", "memory");
```

`c/libcr2` passes it in `RDX` only.  The kernel's interrupt entry
(`src/abi/syscall.rs`) does `mov rdx, rax` before calling `syscall_inner`, so
what it actually reads is `RAX` --- which `libcr2` never sets.  A C program
built against `libcr2` therefore makes its first syscall with whatever happened
to be left in `RAX`.

That is not theoretical.  A small C probe built against `libcr2` was traced on
this kernel: its first `print()` arrived as syscall 0, which is `exit`, and the
process vanished before printing anything.  Adding `"a"(number)` to `libcr2`'s
inline assembly made the same probe reach `print` normally.  Setting both
registers costs one instruction and works whichever register the kernel
happens to read.

## Startup

`_crt0.asm` reads the argv frame the kernel leaves on the stack, switches to
the private stack in `.bss`, and calls `__r2_start`, which:

1. brings up the heap (a global constructor may allocate),
2. runs `.init_array` --- the linker script defines the bracketing symbols by
   hand, which a custom script must or no global constructor ever runs,
3. calls `main`,
4. flushes the console, runs `at_exit` handlers, then the static destructors
   and `.fini_array`, and makes the exit syscall.

`main` may be declared either way; `int main()` and `int main(int, char **)`
both link.

## Using libcr2 from C++

`c/libcr2/net.c` already implements ARP, ICMP, DHCP and TCP on top of the
kernel ABI, and this library deliberately does not reimplement them.  To use
it, add the archive to the link:

```make
NAME       := myserver
EXTRA_LIBS := $(abspath ../../../c/libcr2.a)
include ../../Makefile.tmpl
```

Two things to know when both libraries are linked:

- Put `libc++r2.a` first.  Both define `memcpy` and friends; `libcr2`'s takes a
  `uint16_t` length and silently truncates copies over 65535 bytes.
- `libcr2` also defines `malloc`, `free` and `realloc` against the kernel heap.
  libc++r2 keeps its C allocator out of `libc++r2.a` for exactly this reason,
  so there is no clash --- but remember that memory from `libcr2`'s `malloc`
  cannot be passed back to a syscall.
- If a duplicate symbol does turn up, `-Wl,--allow-multiple-definition` settles
  it in favour of whichever archive came first.

## Using a host libstdc++ (the memento-hello case)

A program that links the host's `libstdc++.a` needs a pile of glibc symbols
that never run: the verbose terminate handler wants `fprintf`, the demangler
wants `sprintf`, `system_error` wants `strerror_r`.  `libc++r2compat.a` has
them, together with `malloc`/`free` onto the arena and the `_Unwind_*` stubs:

```
g++ ... $(OBJS) libc++r2.a -lstdc++ libc++r2compat.a -lgcc ...
```

These are the stubs from `cpp/memento-hello/r2_stubs.cpp`, collected in one
place so each new C++ program does not have to rediscover them.

## Graphics

Two paths, and `examples/gfxdemo` uses whichever the kernel actually has:

- **VESA framebuffer.**  `Display::open()` describes it; `present()` sends a
  `Canvas`.  A full-screen 32-bit buffer at 1024x768 is 3 MiB and will not fit
  in a 2 MiB process, so draw into a small canvas --- 320x200 is 256 KiB --- and
  let the kernel scale it up.  That is what the second form of syscall `0x17`
  is for.
- **VGA mode 13h.**  `Vga13::open()` switches the hardware and maps VGA RAM
  into the process; one byte per pixel, no blit syscall per frame.  Text mode
  is restored by the destructor.

Text comes from the kernel's own PSF font via `Font::kernel()` --- 8 pixels
wide, one byte per row, most significant bit leftmost.  If the kernel has no
font, `Canvas::draw_text` draws boxes rather than nothing, so a layout problem
is still visible.

## Tests

`make check` runs the host tests: a thousand-odd assertions over the allocator
(including a churn loop that checks every block comes back), the containers,
the string, `sort`, and the number formatting.  They run natively, where a
failure is a line number rather than a triple fault.

`tests/target/` is the other half --- startup, the console, the filesystem
round trip, the clock and the heap under load --- and it can only run on r2.
It writes its result to `CXXTEST.TXT` on the floppy so a run can be checked
from outside the virtual machine.

## Known rough edges

- **`sleep()` is best-effort.**  The kernel marks the process blocked and the
  scheduler wakes it on a PIT tick, but the scheduler takes its lock with
  `try_lock`, so a sleep requested at a contended moment can return early.  Do
  not use it as a clock; use `ticks()` for that, and `FrameTimer` for pacing.
- **A program launched from `INIT.RC` at boot sometimes never gets scheduled
  again** after its first few syscalls --- roughly one run in three, in a
  headless QEMU harness.  It is not specific to C++: a C program doing the same
  work through `libcr2` hangs the same way at the same point.  Launched from
  the shell instead (`fg SELFTEST`), the same binary ran to completion every
  time it was tried.
- **`fs::write` writes one 512-byte sector.**  Syscall `0x21` reads exactly 512
  bytes from the pointer it is given and produces a file of that size, so
  `write()` stages through a zeroed buffer and refuses anything longer.  There
  is no append.
- **`fs::size_of()` reads the directory**, because there is no stat syscall.
  Give it an absolute path.
- **`sin`, `cos` and `atan` are approximations** --- around 1e-9 for the first
  two, 1e-5 for `atan`.  Fine for turning a cube; not for numerical work.
- **No `shared_ptr`, no `map`, no iostreams.**  With one thread, a fixed arena
  and a 2 MiB address space, reference counting and node-based containers cost
  more than they are worth here.

## Layout

```
libc++r2/
├── include/r2.hpp          one include for everything
├── include/r2/*.hpp        the headers above
├── src/_crt0.asm           startup, and the private stack
├── src/*.cpp               the runtime
├── src/compat/             the host-libstdc++ stubs
├── linker.ld               0x600_000, with .init_array bracketed
├── Makefile                the library
├── Makefile.tmpl           include this from an application
├── examples/hello          console, containers, filesystem, system info
├── examples/gfxdemo        canvas, font, mouse, both display paths
├── tests/host              runs natively
└── tests/target            runs on r2, writes CXXTEST.TXT
```
