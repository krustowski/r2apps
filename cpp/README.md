# C++ library (libc++r2) and apps

| Project name | Purpose | State |
| ------------ | ------- | ----- |
| `libc++r2` | C++23 runtime and standard library for `r2`: containers, strings, formatted output, `expected`, coroutines, filesystem, graphics, input, and the kernel ABI. | usable |
| `example-print` | The minimal C++ program: a hand-written syscall wrapper and nothing else. | stable |
| `memento-hello` | The Memento GUI framework on `r2`: a desktop with a file manager, web browser, Telegram client, video player and DOS programs (through `c/them`) in windows. | unstable |
| `memento-hello/web` | The engine of Memento's Web window: TCP/IP, TLS 1.2 (BearSSL), HTTP/1.1, HTML layout, pictures. See its [README](memento-hello/web/README.md). | unstable |
| `third_party/bearssl` | BearSSL, vendored unmodified for the Web window's TLS. | upstream |
| `third_party/stb` | stb_image 2.30 (public domain), vendored unmodified for the pictures in the Web and Telegram windows. | upstream |

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
