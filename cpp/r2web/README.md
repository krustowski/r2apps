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

## JavaScript

[MuJS](https://mujs.com/) 1.3.5 is vendored under its ISC license in
[`../third_party/mujs`](../third_party/mujs/README-r2.md). It supplies ES5
functions, arrays, objects, closures, JSON, regexps, Date and Math. The r2
port provides real number formatting and x86-64 setjmp/longjmp, without a
host libc. [`port/`](port/) contains the freestanding compatibility code.

The browser runs the first 16 classic inline/external `<script>` elements
in document order, after parsing HTML and before loading linked stylesheets
and pictures. Local pages can load local `.js` files; remote pages use the
existing HTTP/TLS loader. Non-JavaScript types, including modules and JSON
data scripts, are skipped. CSS/image updates do not execute scripts again.

Available browser bindings:

- `window` (the global object), `document.title`, `document.body`.
- `document.write(...)`, which appends markup at the end of the body.
- `document.getElementById(id)`, returning a small element object or `null`.
- Element `innerHTML`, `textContent`/`innerText`, text-control `value`,
  `className`, `href` and `src`; values can be read and assigned.
- Inline `onclick` on links and buttons, with `this` bound to their element
  when they have an ID. Supported handlers handle the activation themselves;
  automatic link navigation/form submission is suppressed.
- `location.href` and `location.assign(url)` request navigation.
- `console.log(...)` and `alert(...)` show text in the status line.

This is a small browser API, not a complete DOM. ES modules, modern JS syntax,
timers, event listeners/onload, fetch/XHR, cookies, storage, dynamic script
insertion and the broader DOM are not implemented. MuJS does not turn this
text-layout browser into an engine for current web applications. `:js off`
disables scripts and reloads; `:js on` enables them again. The demo tests
arithmetic, generated content, a form value and a button handler.

Each VM has a **2 MiB allocation limit**, including allocation headers.
Each script/handler gets at most **one million interpreter/regexp steps or
250 ms**, whichever happens first. A timeout discards that VM and releases
all its allocations, including abandoned parser temporaries. Syntax/runtime
errors are reported and the next script can run. DOM output retains the
existing 768 KiB page cap. The browser's arena grows on the kernel user heap;
there is no scripting arena in the desktop process. The build checks both
ELFs against the kernel's private 2 MiB image limit.

## Hosting

[`host.h`](host.h) defines a pointer-free versioned ABI: a bounded command
queue, heartbeat and shutdown words, clipboard/new-window mailboxes, and
two trailing pixel buffers sized for the desktop. Frame slots have atomic
reader/writer claims so the producer cannot overwrite a frame being copied.
The active rectangle is independent of Memento's bitmap allocation stride,
which can grow beyond the client area during a resize. Closing waits for the
child, then terminates it if needed, before reclaiming the shared block.

Each hosted browser uses eight TCP ports starting at `48000 + slot*32` and
a DNS port at base+10. Memento's Telegram stack keeps its original 47000
range; port bindings route replies to the right process. Browser windows
have independent page/script state and run alongside Telegram, Chat and IRC.
The shared engine stays in [`../memento-hello/web`](../memento-hello/web/README.md)
because Telegram uses its loader, TLS, picture and PNG code too.
