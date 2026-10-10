# C++ library (libc++r2) and apps

| Project name | Purpose | State |
| ------------ | ------- | ----- |
| `libc++r2` | C++23 runtime and standard library for `r2`: containers, strings, formatted output, `expected`, coroutines, filesystem, graphics, input, and the kernel ABI. | usable |
| `example-print` | The minimal C++ program: a hand-written syscall wrapper and nothing else. | stable |
| `memento-hello` | The Memento GUI framework on `r2`: a desktop with a file manager, web browser, Telegram client ([`go/telegram`](../go/telegram/README.md), hosted), video player and DOS programs (through `c/them`) in windows. | unstable |
| `r2web` | A separate browser process hosted in Memento, with tiny ES5 JavaScript support (MuJS). See its [README](r2web/README.md). | unstable |
| `jug` | The program manager: a CDN catalog of programs, SHA-256-verified ELF updates to the RAM disk, and restarts of running instances with their arguments; a console and a hosted Memento window. See its [README](jug/README.md). | unstable |
| `mpegplay` | MPEG-1 video with MP2 sound through HD Audio, from files or HLS streams, in Memento's Video window or full screen on the graphics kernel. See its [README](mpegplay/README.md). | unstable |
| `memento-hello/web` | The shared engine of Memento's Web window: TCP/IP, TLS 1.2 (BearSSL), HTTP/1.1, HTML layout, pictures. See its [README](memento-hello/web/README.md). | unstable |
| `third_party/mujs` | MuJS 1.3.5 (ISC), vendored for r2web's JavaScript with freestanding platform hooks and execution limits. | ported |
| `third_party/bearssl` | BearSSL, vendored unmodified for the Web window's TLS. | upstream |
| `third_party/stb` | stb_image 2.30 and stb_sprintf 1.10, vendored unmodified for pictures and JavaScript number formatting. | upstream |
| `third_party/h264bsd` | h264bsd (Baseline H.264), for the MP4 animations Telegram shows (built into `go/telegram`). | upstream |

The Turbo C++ IDE that Memento's Editor window hosts (`tcpp.elf`) is built from its own repository; it compiles with [`c/tcc`](../c/tcc/README.md).

## libc++r2

`libc++r2` is to C++ what `c/libcr2` is to C: the whole userland surface of the
kernel, and the pieces of the language runtime that have to exist before C++
code can run at all --- global constructors, `operator new`, the Itanium ABI
hooks, `memcpy` and friends.

It is self-contained: built with `-nostdinc -nostdinc++`, it uses no host
header and links against no host library.  C++23 by default --- including the
parts of the language that need library support to work at all, such as `<=>`,
coroutines and structured bindings --- and C++17 on request for code that is
not ready to move (`make STD=c++17`).

```shell
cd libc++r2
make            # libc++r2.a, libc++r2compat.a, _crt0.o
make check      # host-side tests
make examples   # examples/hello, examples/gfxdemo, examples/snake
```

An application needs a two-line Makefile:

```make
NAME := hello
include ../../Makefile.tmpl
```

See [libc++r2/README.md](libc++r2/README.md) for the memory map, the syscall
convention, and how to link `c/libcr2` or a host `libstdc++` alongside it.
