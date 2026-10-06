# MuJS on r2

MuJS 1.3.5, vendored from [ccxvii/mujs](https://github.com/ccxvii/mujs)
at commit `05cd646bad083ed45e9e1c9846ea671d461ced30` (the last source revision
before that repository moved). The [ISC license](COPYING) and upstream source
are included. The r2 build compiles `one.c`; it does not build the CLI.

Local changes, guarded by `WEB_MUJS_R2` / `WEB_MUJS_LIMITS`:

- Date uses r2's UTC RTC, rather than POSIX time calls.
- Math.random seeds from the RTC; it is not cryptographic randomness.
- Script file IO is disabled; the browser fetches scripts through its loader.
- The VM instruction loop and regexp matcher call `web_js_poll()` so scripts
  cannot hold the hosted browser indefinitely. The host caps each evaluation
  at one million steps or 250 ms and discards a timed-out VM, including parser
  temporaries. JavaScript catch blocks cannot intercept that timeout.
- `js_try` explicitly casts its opaque save area for C++ callers.

The freestanding headers, x86-64 setjmp/longjmp, scalar libm additions and
number formatter are in [`../../r2web/port`](../../r2web/port). There is no
host libc dependency. The renderer and MuJS use libc++r2's existing scalar
math; this port is intended for page scripting, not numerical research.

The interpreter's arrays, string size and recursion limits are reduced for
r2. Script allocations use a separate tracked kernel-heap pool, with a 2 MiB
limit; browser document and picture buffers use their existing pools. No
JavaScript engine or scripting heap is linked into Memento itself.
