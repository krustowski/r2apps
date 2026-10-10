# r2web — Memento's hosted web browser

`r2web.elf` is a separate C++ process. Memento's **Web** icon starts it with
`--host <shared-block-address>`, as Spotify and the DOS emulator are hosted.
The browser owns its toolbar, navigation/history, HTML/CSS layout, forms,
pictures, TLS and networking. Memento owns the outer window and delivers
keyboard/mouse input and clipboard text, then composites indexed frames.
The child never opens a graphics mode or reads Memento's input pipes.

```sh
make -C ../memento-hello build  # builds both memento-hello.elf and r2web.elf
make build                    # browser only
make test                     # host-side JS, DOM, limits and shared-frame tests
```

Install both ELFs in `/mnt/tar/bin` or `/mnt/iso/bin` (`memento-hello.elf` is
packaged as `memento.elf`). `r2_main`'s ISO rule includes both and places the
demo at `/mnt/tar/opt/r2web/demo.html`. The desktop's `make install` copies
both to the FAT image as `MEMENTO.ELF` and `R2WEB.ELF`. TLS roots retain their
existing path `/mnt/tar/opt/memento/cacerts.bin`.

The kernel must preserve x87/SSE state across task switches (`FXSAVE`/`FXRSTOR`
in its timer handler). QuickJS and the C++ browser use that state; older
kernels can corrupt application data when another app runs, causing stalled
loads or incorrect port numbers. Build and boot a current `r2_main` image
(`make build` there); copying a newer `r2web.elf` alone does not update the
kernel.

## JavaScript

Pages run on [QuickJS](https://bellard.org/quickjs/) through
[`../libjsr2`](../libjsr2/README.md): ES2023 and later (classes and private
fields, async/await, modules without imports, BigInt, regexps with named
groups), an event loop with timers, microtasks and animation frames, and the
Web APIs that need no document --- fetch, XMLHttpRequest, EventSource,
URL, TextEncoder/TextDecoder, atob/btoa, crypto random values, Blob,
FormData, AbortController, structuredClone, console.

The document is [`js/dom.js`](js/dom.js), compiled to bytecode at build
time: a DOM tree (nodes, elements and the HTML element classes, attributes,
classList, dataset, inline style, innerHTML/outerHTML, selectors for
querySelector/matches/closest, events with capture and bubbling, inline
`on...` handlers, forms and their controls, templates, shadow roots, custom
elements, MutationObserver), and the window around it (location and history
with pushState, localStorage/sessionStorage and cookies kept per origin for
the browser's session, matchMedia, getComputedStyle, IntersectionObserver
and ResizeObserver that report everything visible). [`htmlparse.cpp`](htmlparse.cpp)
reads HTML into that tree the way browsers do (implied html/head/body, end
tags paragraphs and list items imply, tables, raw text, SVG).

The tree is the page once its scripts start. The browser keeps its native document
([`web/doc.cpp`](../memento-hello/web/doc.cpp)) and lays out what the
DOM serialises after every change (at most every 150 ms), with each link and
control marked `onclick="r2:N"`; what the user does to it comes back as DOM
events on node N: clicks (a listener makes any element clickable), typing
(an `input` event per key), checkboxes, lists, Enter and form submission,
which goes through the page's `submit` event and its DOM's values. Focus and
the field being typed into stay on their nodes across re-layouts.
HTML snapshots are copied to native buffers in small batches, including for
geometry queries, so rendering does not keep full HTML copies in the script
heap. Snapshots are limited to 3 MiB; a rejected snapshot keeps the last shown page.
[`script.cpp`](script.cpp) is the bridge: the engine, the natives `dom.js`
calls, and the page's network --- three connections of its own next to the
browser's, streamed, so an EventSource lasts as long as the page.

Scripts run in document order (classic first, then deferred and module
scripts), external ones fetched by the loader, `file:` ones read from the
disk; scripts a page adds later run when they are connected. `document.write`
while the page loads lands after the running script. Pages without scripts
or inline handlers start no engine at all. `:js off` disables scripts and
reloads; [about:console](about:console) shows what scripts logged.

Limits: **12 MiB** of script heap per page, **one second** per task (a
script, a callback and its microtasks; then it is stopped with an
uncatchable error and the page goes on), 160 KiB of native stack. Not there:
`import` in modules, synchronous XMLHttpRequest, WebSocket, canvas drawing,
CSS positioning, grid, custom fonts and full table layout. The build checks
the ELF against the kernel's private 2 MiB image limit (about 1.35 MiB with the stack).

`make test` runs the host checks (parser, DOM, events, loop, limits, pixel
boxes/geometry, shared frames); libjsr2 has its own, on the host and in QEMU. GARN's served folder
(`r2_main/iso/opt/garn`) has `jstest.htm` (self-checking, with things to
click and type into) and `sse.htm` (an event stream from GARN's `/events`)
for `http://localhost/` on r2 itself.

## Pixel layout

Pixel layout is the default. Type `:layout text` in the address bar to switch
back to the earlier cell layout, or `:layout px` to return to pixel boxes.
The switch applies to the current page and subsequent navigation in that
browser window, without reloading the page or restarting its scripts.

[`web/pixels.cpp`](../memento-hello/web/pixels.cpp) lays out the retained
native element tree. It supports block and inline flow, inline blocks,
width/height and min/max dimensions, content-box/border-box sizing, margins
(including horizontal auto margins), padding, solid borders and backgrounds.
Lengths include pixels, percentage widths and heights with a definite
containing height, and the parser's existing em/rem/pt conversions. CSS RGB colours survive the cascade and are quantised once to the active
framebuffer palette (256 colours on supported displays, EGA on VGA). An
authored dark canvas keeps its colours when browser dark mode is enabled.
Solid rounded borders support a single pixel radius. Buttons use padded
boxes in pixel mode; their bracket delimiters remain in text mode. Text wraps at words and splits long words using
the measured system font; CSS font sizes select the normal or enlarged system
font. Summary values can use the enlarged font while panel headings remain
compact.

Flex containers support rows and columns, growth/shrinkage, flex basis,
gaps, row wrapping, justify-content, align-items and align-self. Text directly
inside a flex container becomes an anonymous flex item. Images, form editing,
links, keyboard focus, find and scrolling use the resulting pixel positions. Fractional pixels are carried across flex children so dense chart rows fill their width.
Decorative `aria-hidden` elements remain visible. GARN's
[`/ui` history charts](../../c/garn/README.md#system-webui) use these boxes
for NIC throughput, heap usage and scheduler counts.

In pixel mode, `getBoundingClientRect`, offset dimensions/positions,
client dimensions/border offsets and scroll extents come from the same layout
engine. A query after a DOM/style change flushes a native snapshot immediately,
including fetched style sheets and loaded image sizes; bounding rectangles
account for page scrolling. `innerWidth`, `innerHeight`, `scrollY` and
`pageYOffset` report the page viewport. Text mode retains approximate sizes.

This is a first box-layout implementation: adjacent margins are not collapsed;
inline rectangles are unions of their fragments; border styles draw as solid;
column wrapping, reversed flex directions, positioned elements, overflow clips
and independent element scrolling are not implemented. CSS selectors and
media rules retain the shared parser's existing limitations. The demo shows
flex boxes and a size readout that updates after changing a box's width.

## Hosting

[`host.h`](host.h) defines a pointer-free versioned ABI: a bounded command
queue, heartbeat and shutdown words, clipboard/new-window mailboxes, and
two trailing pixel buffers sized for the desktop. Frame slots have atomic
reader/writer claims so the producer cannot overwrite a frame being copied.
The active rectangle is independent of Memento's bitmap allocation stride,
which can grow beyond the client area during a resize. Closing waits for the
child, then terminates it if needed, before reclaiming the shared block.

Each hosted browser uses eight TCP ports starting at `48000 + slot*32` and
a DNS port at base+10; telegram.elf, hosted the same way, takes four from its
own slot's base. Memento's own stack (the Video window) keeps the 47000
range; port bindings route replies to the right process. Browser windows
have independent page/script state and run alongside Telegram, Chat and IRC.
The shared engine stays in [`../memento-hello/web`](../memento-hello/web/README.md)
because Memento and jug use its loader, TLS and PNG code too.
