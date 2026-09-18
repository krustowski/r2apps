# C++ library (libc++r2) and apps

| Project name | Purpose | State |
| ------------ | ------- | ----- |
| `libc++r2` | C++23 runtime and standard library for `r2`: containers, strings, formatted output, `expected`, coroutines, filesystem, graphics, input, and the kernel ABI. | usable |
| `example-print` | The minimal C++ program: a hand-written syscall wrapper and nothing else. | stable |
| `memento-hello` | The Memento GUI framework on `r2`, linked against the host libstdc++. | unstable |

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
make examples   # examples/hello, examples/gfxdemo
```

An application needs a two-line Makefile:

```make
NAME := hello
include ../../Makefile.tmpl
```

See [libc++r2/README.md](libc++r2/README.md) for the memory map, the syscall
convention, and how to link `c/libcr2` or a host `libstdc++` alongside it.
