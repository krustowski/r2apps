# tcc

[TinyCC](https://repo.or.cz/tinycc.git) ported to `r2`: the compiler runs on `r2` and builds `r2`
programs there. This directory holds what the port adds to upstream tcc, which is fetched at a pinned
commit and left unmodified:

+ `libc/` --- a small C library on top of libcr2's syscalls: stdio, the printf family (floating point
  included), strings, `strto*`, `qsort`, time, `setjmp`, and file descriptors over the kernel's named-file
  calls. tcc is linked against it, and so is every program tcc builds on `r2`.
+ `r2/` --- tcc's configuration for `r2` and a wrapper around its `main` that adds the options every `r2`
  program needs: `-static -Wl,-Ttext=0x600000 -D__r2__=1`.
+ `tests/` --- `tcct` (does a compiler's output load and run?), `libct` (does the C library behave like
  glibc?) and `r2ct` (do the C library and libcr2 go together?), with the `INIT.RC` scripts that run them.
+ `run-qemu.sh` --- boots `r2` with files on a scratch floppy, waits for the result files a test writes,
  and prints them.

## Building and shipping

```shell
make build    # clones tinycc, builds a host tcc, then build/sysroot
make test     # tcct, libct and tcc on r2, each in QEMU
```

`make build` produces `build/sysroot`, laid out as the boot archive is:

```
bin/tcc.elf
include/       tcc's own headers and this libc's; libcr2's in include/r2/
lib/           crt1.o crti.o crtn.o libc.a libcr2.a libtcc1.a
```

`build_iso` in `r2_main`'s Makefile copies it into `iso/bin`, `iso/lib` and `iso/include`, so on `r2` it
is `/mnt/tar/bin/tcc.elf`, `/mnt/tar/lib` and `/mnt/tar/include` (and the same under `/mnt/iso`). tcc
looks in `/mnt/tar` (`r2/config.h`); `-B <dir>` points it at another root laid out the same way.

The tests:

| Target | What runs on `r2` |
| ------ | ----------------- |
| `test-tcct` | `tcct`, built by the host tcc against libcr2 alone |
| `test-libct` | `libct`, built by gcc against this libc |
| `test-tcc` | `tcc -o r2ct.elf r2ct.c -lcr2`, `tcc -o libct.elf libct.c`, and both programs |
| `test-image` | the same, on `r2_main/r2.iso` as it is (graphics kernel, tcc as shipped) |
| `libct-host` | (on Linux) `libct` against glibc, the reference for its expected values |

`run-qemu.sh` reads `R2_MAIN` (the kernel checkout, `../../../r2_main` by default), `R2_ISO` (an image
to boot as it is), `R2_TAR_ADD` (a directory to add to the boot archive), `R2_WORK` and `R2_TIMEOUT`.

## Using it on r2

Programs are compiled where they can be written, as only FAT volumes are writable:

```
cd /mnt/fat
fg tcc -o hello.elf hello.c
fg hello
```

The kernel passes at most 8 arguments, so options that every build needs belong in `r2/main.c`.

libcr2 comes with it: its headers as `<r2/syscall.h>`, `<r2/net.h>` and so on, its TCP/IP stack as
`-lcr2` (`tcc -o srv.elf srv.c -lcr2`). Its syscall wrappers are in `libc.a` already. Next to the C
library, five of its calls go by other names, as theirs belong to C: `r2_exit(pid, code)`,
`r2_chdir(path)`, and for sockets `tcp_read`, `tcp_write` and `tcp_close`. libcr2's headers notice the C
library by `<sys/r2libc.h>` (see `libcr2/types.h`); the apps built with `-nostdinc` see libcr2 as before.

The r2 port of the tcpp IDE uses this compiler too: **F9** saves and builds the current `.C` file
beside its source as `.ELF`, and **Ctrl+F9** builds and runs it. The Build panel shows diagnostics.
The compiler runs in a separate task, using `tcc.elf --ide 0xADDRESS` and the versioned shared-heap
request in `r2/ide.h`. That entry point uses TinyCC's library API, the same r2 link options as the
command line, and `libcr2`; source and output paths do not consume the kernel's eight argument slots.
Both the IDE and compiler must be updated in the boot image. TinyCC compiles C, not C++.

## The C library

It is the part of C99 tcc needs, and what a program reaches for next. On `r2`:

+ A file is a name and an offset: the kernel keeps no open files, and each read or write names the file
  (syscalls 0x39 and 0x3a). Names are at most 63 bytes. A new file appears on its first write; `w`
  deletes and recreates. A file's size is found by probing (there is no `stat`).
+ `stdout` is line-buffered and `stderr` unbuffered, both to the console; `exit()` flushes every stream,
  so programs must start from this `crt1.o`, not libcr2's `_crt0.asm`. `stdin` is always empty.
+ `malloc` is the kernel's user heap, which hands out zeroed memory.
+ `printf` floats are exact to 18 significant digits; further digits print as 0. `strtod` scales in
  long double, so a constant can, rarely, be off by its last bit.
+ There is no `scanf`, environment (`getenv` gives NULL), locale, time zone (the RTC is taken as UTC),
  `system` or `exec`.

## Notes

+ The host tcc is built without bounds checking and backtraces: their runtime does not build out of
  tree, and `r2` has no use for them.
+ libcr2's own `memcpy` takes a `uint16_t` length: copies of 64 KiB or more come out wrong. Programs on
  the C library get its `memcpy` instead.
+ `tcc -run` is compiled in but untested.
+ On the graphics kernel, `sleep_ms` returns after one tick whatever it is asked for (`r2ct` says so);
  the text kernel sleeps as long as it should.
