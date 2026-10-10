# libjsr2 — JavaScript for r2 programs

QuickJS ([`../third_party/quickjs`](../third_party/quickjs/README-r2.md)),
an event loop, and the Web APIs that need no document, as one static
library for freestanding r2 programs. r2web builds its DOM on it; `js.elf`
runs scripts with nothing else.

```sh
make            # libjsr2.a for r2, and obj/host/jsr2c (the bytecode compiler)
make host       # obj/host/libjsr2.a, the same library for host tests
make test       # the host tests (tests/*.test.js)
make cli        # js.elf
make test-r2    # most of the tests again, in js.elf on r2 under QEMU
make install    # JS.ELF onto the floppy image
```

## Using it

```cpp
#include "jsr2.h"

jsr2::setPlatform({pageAlloc, pageFree, monotonicMs, epochMs, entropy, log});
jsr2::Engine e;
e.start();                       // runtime, context, the Web APIs
e.setTransport(&myNetwork);      // for fetch, XMLHttpRequest, EventSource
e.eval(src, len, "page.js");     // src NUL-terminated at len
for (;;) { e.tick(); if (e.wantsFrame()) e.frame(now); /* idle */ }
```

`Platform` is the machine: big blocks for the heap, a millisecond clock, the
date, entropy and a log line. Nothing in the library makes a syscall or
blocks, so the same code runs on r2 and on the host.

Every callback into the script is a *task* (`eval`, `call`, a timer, a
network event): it gets `Limits::taskMs` (1 s) and is then stopped with an
uncatchable error, and its microtasks run before it ends. Unhandled promise
rejections are reported after that checkpoint, as browsers do. `error()` is
the last uncaught error; the platform's log hears every one.

`tick()` runs what is due: the network's news first, then timers (oldest
first, at most 64 a tick, so zero-delay loops cannot starve the program).
`frame()` runs requestAnimationFrame callbacks; the embedder decides the
rate. `busy()` says whether anything is still to come.

## Memory

Each engine has a `Heap` with a limit (`Limits::heapBytes`, 8 MiB by
default). QuickJS keeps blocks of up to 512 bytes in 4 KiB arenas of its own;
what comes to the heap is those arenas and bigger blocks, cut by size class
from 256 KiB chunks of the platform's memory, or taken whole above 64 KiB.
`stop()` returns every chunk at once, whatever the script left behind.

## Network

`Transport` is three calls: `open` (method, absolute URL, header lines,
body, whether to stream), `next` (HEADERS, DATA, END or FAIL, polled every
tick) and `close`. fetch, XMLHttpRequest and EventSource are built on it in
the prelude; r2web implements it on `web::Loader`. EventSource follows the
spec's parser (CR/LF/CRLF, `event`, `data`, `id`, `retry`, comments) and
reconnects with `Last-Event-ID`.

## The prelude

[`src/prelude.js`](src/prelude.js) is the API surface, compiled to bytecode
at build time by `jsr2c` (a host build of the same QuickJS sources: the
bytecode format and atom table do not depend on the C library, so it loads
unchanged on r2) and read in place, without a copy. It defines console,
timers, queueMicrotask, requestAnimationFrame/requestIdleCallback,
performance, DOMException, Event and its kinds, EventTarget (capture,
bubbling through a parent hook embedders give, once, passive, signal),
AbortController/AbortSignal, URL and URLSearchParams (WHATWG parsing,
relative resolution, IPv4 forms), TextEncoder/TextDecoder (UTF-8 with
streaming, windows-1252, UTF-16LE), atob/btoa, crypto.getRandomValues and
randomUUID, structuredClone, Blob, File, FormData, Headers, Request,
Response, fetch (with data: and blob: URLs), XMLHttpRequest (asynchronous),
EventSource and navigator. Embedders build on `globalThis.__jsr2lib`.

## On r2

[`port/`](port/) is the C library QuickJS sees: libc++r2's string and
memory functions, `stb_sprintf` and the clocks behind `jsr2_` names
([`src/libc.c`](src/libc.c)), and musl's libm under `jsr2m_` names
([`../third_party/musl-libm`](../third_party/musl-libm/README-r2.md)), so
nothing collides with libc++r2's own `sin` or `snprintf`. r2's clock is UTC:
time zone offset 0. The library is about 645 KB of code and data; r2web's
image is 1.35 MiB with it and its 256 KiB stack.

`js.elf` (`cli/`): `js [-o FILE] [-t TASK_MS] script.js ... [-- args]`. It
runs the scripts, then the loop until nothing is pending, `r2.exit()` or a
minute; console output goes to the console and, with `-o`, into FILE
(appended as it grows, so a floppy can be read while it runs). Scripts also
have `r2.args`, `r2.readFile`, `r2.writeFile`, `r2.lastError`, `r2.log` and
`r2.heap`.

`make test-r2` (`tests/run-qemu.sh`) boots the text kernel with js.elf and
the test bundle on a scratch floppy and reads the result back; 31 checks
(language, Math through musl, dates, timers, events, URL, encoding, limits)
pass there as on the host. fetch and EventSource are checked on the host
against a scripted network, and on r2 through r2web and GARN.
