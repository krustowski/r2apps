# QuickJS on r2

QuickJS 2026-06-04 by Fabrice Bellard and Charlie Gordon, vendored from the
release tarball <https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz>
(SHA-256 `b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a`)
under its [MIT license](LICENSE). Only the engine is here: `quickjs.c`,
`libregexp.c`, `libunicode.c`, `cutils.c`, `dtoa.c` and their headers; not
the `qjs`/`qjsc` programs, `quickjs-libc` or the tests.

One local change, guarded by `CONFIG_R2`: `CONFIG_ATOMICS` stays off (r2 has
one thread per process and no pthreads), so there is no `Atomics` object.

Everything else the engine needs from the system comes from
[`../../libjsr2/port`](../../libjsr2/port): freestanding headers, formatting
and clocks under `jsr2_` names, and musl's libm
([`../musl-libm`](../musl-libm/README-r2.md)). The r2 build defines
`NDEBUG`; host builds keep QuickJS's assertions, which catch leaked values
when a runtime is freed. Compiled with `-Os`, the engine is about 515 KB of
code.
