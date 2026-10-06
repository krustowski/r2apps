# web — the shared browser and Telegram engine

A small web browser for rou2exOS: HTTP/1.1, TLS 1.2 by
[BearSSL](https://bearssl.org/), and HTML with a small CSS subset, pictures and forms,
drawn in the kernel's own fixed-width font. Memento's
[`Web window`](../windows/browser_window.cpp) hosts the separate
[`r2web.elf`](../../r2web/README.md) process. Its browser UI is now
[`../../r2web/browser.cpp`](../../r2web/browser.cpp); its shared engine is here.

```
r2web/browser.cpp    toolbar, page, status line, keys, history
      │
   loader            resolve → connect → TLS → request → response → redirects
   │   │   │
   │   │   └── http      request line, response parser (chunked, lengths, 100)
   │   └────── tls.c     BearSSL's non-blocking engine; roots from cacerts.bin
   └────────── NetIf     net_r2.cpp on r2, sockets in tests/host_fetch.cpp
doc                  HTML → items → lines and runs, in character cells; forms
css                  the CSS subset: selectors, cascade, @media
url                  what the user typed, and links resolved (RFC 3986)
image                PNG/JPEG/GIF/BMP → scaled, dithered palette indices;
                     every frame of an animated GIF (Telegram)
   └── stb_image.c   the decoder (stb_image 2.30, in ../../third_party/stb)
mp4                  an MP4 of H.264 → an animation, a step at a time (Telegram)
   └── h264.c        the decoder (h264bsd, Baseline only, ../../third_party/h264bsd)
png                  palette indices → PNG (screenshots)
web_r2.cpp           memory, clock, entropy and the date, on r2
```

The Telegram window (`../windows/telegram_window.cpp`) uses the same loader,
TLS and picture code for the Bot API.

Nothing blocks.  The browser process turns the loader from its own loop (Telegram uses Memento's idle loop), the
loader turns the network stack, and the TLS engine is BearSSL's low-level one,
which never touches a socket: bytes go in and come out through its buffers.

## What it shows

Headings (h1 and h2 at twice the size), paragraphs, lists (bullets, circles,
numbers), block quotes, definition lists, `<pre>`, tables read row by row,
links (underlined, walkable with Tab), bold (the font has none: the glyphs are
drawn twice, one pixel apart), colours, horizontal rules, pictures, and
forms.  `<script>` is skipped by this renderer; r2web runs classic scripts separately
through MuJS and a small DOM adapter before laying out their output. `<svg>`
and friends are skipped. See [r2web's JavaScript API](../../r2web/README.md#javascript).

Text is decoded from UTF-8, windows-1252/ISO-8859-1, windows-1250 and
ISO-8859-2, then mapped to CP437, which is what the font is for letters and
the Latin-1 symbols.  What CP437 does not have loses its accent (č → c,
ř → r); the font's box-drawing and Greek rows are not CP437's, so those are
not used.  `tools/gencharmap.py` generates the tables in `charmap.inc`.

Not done: a complete browser DOM/modern JavaScript, WebP and SVG pictures, cookies, compression (the
request asks for `identity`), IPv6, TLS 1.3.

## Pictures

An `<img>` (its `data-src` when a lazy loader put the real address there;
`data:` addresses are left out) is an item in the document with its alt text
after it.  Until the window has the picture, the alt text is what shows ---
`[alt]`, or `[img]` inside a link --- and once it has, `setImageSize` lays the
page out again with the picture on a line of its own, as many rows tall as
it needs, scaled down to the page's width, aligned with its block.  A
picture inside a link is that link: Tab reaches it and a click follows it.
A link with nothing in it (the "card" pattern: an empty `<a>` stretched
over an article by CSS) takes the card's heading, or failing that its
`aria-label` or `title`.

The window fetches pictures one at a time after the page and its style
sheets, the first 16 of a page, and keeps them by address, so the page read
again (its sheets came, `:css` or `:img` switched) still has them.
`image.cpp` decodes with stb_image --- freestanding, without SIMD or
floating point (`stb_image.c` says how it is built) --- makes the picture fit
(at most the page's width and 480 pixels, never enlarged, each pixel the
average of those it stands for), lays transparency over the page's
background, and puts it into the screen's colours with a 4x4 ordered dither:
the 16 EGA colours, or the 6x6x6 cube on the 256-colour framebuffer.  One
byte a pixel, drawn straight into the window's bitmap.  A picture of 2x2
pixels or less (spacers, counters) is dropped.

An address that answers with `image/*` shows that picture alone, and
`file:` addresses (or a bare `/mnt/...` path) open pictures, `.htm` pages and
text from the disks; a page's relative pictures and links are then files
beside it.

## Uploads

`Loader::start` takes a content type for the body of a POST, so an upload can
be `multipart/form-data`; the request and the body are in the big pool.
`png.cpp` writes the screenshots Telegram sends: an indexed PNG (4 bits a
pixel with 16 colours, 8 with 256), rows filtered Up and deflated with the
fixed codes and one-byte runs --- no match search, which a screen of flat
windows does not need (an 800x600 desktop is about 12 KiB).

Pages are cut at 768 KiB.  The body, the document's text and its layout live
on the kernel's user heap (`big_alloc` in `web_r2.cpp`), apart from Memento's
arena; build with `EXTRA=-DWEB_BIG_ARENA` to keep them in the arena instead.

## CSS

`css.cpp` keeps the part of CSS that a grid of fixed-width cells in sixteen
colours can act on:

- `display` (none, block, inline — a menu of `li { display: inline }` flows
  on one line), `visibility: hidden`
- `color`, `background-color` / `background`, quantised to the EGA palette;
  a colour too close to what is behind it gives way to black or white, and
  the page itself stays white
- `font-weight`, `font-style`, `font`, `text-decoration`, `text-transform:
  uppercase`, `text-align`, `white-space: pre`, `list-style: none`
- `margin` and `padding` on the left as indent cells, top and bottom margins
  as blank lines (rounded; zero removes the tag's own)

Selectors: type, `*`, `#id`, `.class`, `[attr]`, `[attr=value]`, `:root`,
`:link`, `:not()` of one simple selector, descendant and child combinators.
A selector with anything else (sibling combinators, `:hover`, `:nth-child`,
pseudo-elements) is dropped whole.  `@media` is evaluated for a screen 600
pixels wide; `@supports` and `@layer` are read, `@import`, `@font-face` and
`@keyframes` are not.  `var()` is not resolved, so a declaration that uses it
is ignored.

The tags' own looks are a built-in style sheet (`kUaSheet` in `doc.cpp`), so
a page can override them; structure — headings, lists, tables, paragraph
spacing — stays with the tag handlers.  Styles come from `<style>`,
`style=""` and the first three `<link rel=stylesheet>` sheets, which are
fetched after the page is on the screen and applied by reading it again (not
when a form on it has been filled in meanwhile).  `:css off` in the address
bar shows pages with the built-in sheet only; `:css on` brings the page's back.

Dark mode (`:dark on|off`, Ctrl+D, or the D/L button in the toolbar) is only
a matter of drawing, in the window: each of the 16 EGA colours a page ends up
with is swapped for its opposite in brightness (black and white, the two
greys, each dark colour and its bright one), so a page keeps its own colours
and the usual contrast check still applies afterwards.  The page is not read
again.

## Forms

Text fields (any type that takes text), password fields, text areas,
checkboxes, radio buttons, `<select>`, submit, reset and image buttons,
`<button>`, and hidden fields.  A form is sent as
`application/x-www-form-urlencoded`: in the address for GET, in the body
for POST (the loader follows 301, 302 and 303 after a POST with a GET, and
repeats it on 307 and 308).  Enter in a text field presses the form's first
button, as browsers do.  A checkbox or radio button without a name is left
out: it can send nothing, and on today's pages it is a CSS trick for opening
menus.  Buttons of type `button` and controls outside any form need
JavaScript; r2web handles supported inline `onclick` handlers through its
DOM adapter and reports unsupported buttons in the status line.

Each control sits in the page's text as a run of placeholder cells and in the
link table as an entry of its own (`Document::linkControl`), so Tab, Enter,
Space and the mouse reach it like a link; the window draws the cells from the
control's state.

## TLS

- TLS 1.2 only, ECDHE (P-256, P-384, X25519) with AES-GCM or
  ChaCha20-Poly1305 first, then AES-CBC and static RSA for older servers.
- The certificate chain must lead to one of the roots in
  `/mnt/tar/opt/memento/cacerts.bin`, read from the USB stick on the first
  handshake: 36 roots (Let's Encrypt, DigiCert, Google, Amazon,
  Sectigo/USERTrust, GlobalSign, Microsoft, GoDaddy/Starfield, Entrust,
  SSL.com, Certum) in 13 KiB.  `tools/mkcacerts.sh [bundle.pem] [usb-dir]`
  regenerates `cacerts.bin` (and copies it into `usb-dir/memento`).
  `tools/mkcacerts.c` describes the format.  Without the file every certificate is unknown, and the error
  page says which file could not be read.
- An unknown root gets an error page with a "load it anyway" link; that retry
  forgives the root and nothing else.  Wrong names and expired certificates
  have no way past.
- The date comes from the RTC, read as UTC (QEMU's default).  A clock before
  2024 counts as unset and every certificate is refused, saying why.
- BearSSL is built with `-mgeneral-regs-only` and without its SSE/AES-NI
  code: this kernel does not promise to keep vector registers across a
  context switch.  `bearssl.mk` lists what is left out.

**Entropy is the weak point.**  The engine is seeded with RDRAND where the CPU
has it --- QEMU's default CPU does not; `-cpu host` with KVM usually does ---
plus 512 bytes of timestamp-counter jitter, the RTC and the tick count,
condensed by BearSSL's HMAC-DRBG.  That keeps a passive observer out; it is
not something to trust against an attacker who can model the machine's timing.

## Network

`net_r2.cpp` is its own ARP/IPv4/ICMP/UDP/DNS/TCP client, shaped after
`go/r2net` (whose README explains the kernel's behaviour it works around):

- With no global Ethernet driver registered, the browser registers and
  answers ARP and ping for the machine.  With one registered (eth.elf, garn),
  it binds its TCP ports instead, assumes the tap's MAC `52:54:00:12:34:57`
  for the gateway until a frame teaches it the real one, and --- since UDP
  replies go to the driver --- falls back to DNS over TCP.
- Address `10.3.4.2/24` unless the kernel says otherwise, gateway `.1`,
  DNS `1.1.1.1` then `8.8.8.8`.  `:dns <ip>` and `:gw <ip>` in the address
  bar change them; `about:net` shows them.
- The guest needs a route out: NAT on the host for `10.3.4.0/24`.  With QEMU's
  user networking the same layout works without root:
  `-netdev user,id=n,net=10.3.4.0/24,host=10.3.4.1 -device rtl8139,netdev=n,mac=52:54:00:12:34:56`
- It shares the process's one frame queue with c/libcr2's stack in the Chat
  and IRC windows.  Within Memento, neither reads the queue itself: `../netmux.cpp` does, and
  gives each stack the frames for its ports (the browser claims 47000-47015;
  ARP goes to both, ping to one).  A frame for the stack that is not asking
  just now waits for it there, so Telegram and an IRC session run side by side. r2web uses a
  separate process and distinct bound ports for each browser.
- TCP: one segment in flight when sending; three duplicate ACKs at once on
  a gap; retransmission with backoff.
- Receiving: the window offered is sixteen full segments (`WEB_RX_SEGMENTS`,
  1460 bytes each) into a 32 KiB ring buffer per connection, reopened as soon
  as reading frees a segment.  That rests on the kernel queueing every frame
  in a buffer of its own and leaving what it cannot queue in the NIC's ring
  (see `net/netdrv.rs`); kernels before that kept one frame in one buffer,
  overwritten by the next, and the window had to be two segments of 1024.
  Measured in QEMU with user networking, a 700 KiB page over plain HTTP:
  before, 2 minutes 20 seconds, most of it the sender resending what was
  lost; now 0.07 s for the response, about 2 s until it is on the screen.
  `about:net` counts gaps and repeated segments, which should both stay 0.

## Memory

Memento and r2web each have a private 2 MiB image (see libc++r2's README); its heap arena is
768 KiB of it, and grows onto the kernel's user heap when that is full.  The shared
engine adds about 200 KiB of text --- BearSSL 68, stb_image 32, the engine
(with CSS, forms and pictures) the rest --- built with `-Os`.
Check what is left below the 0x800000 line with
`nm -n memento-hello.elf | grep ' _end$'` after adding anything.

The page's buffers --- the body, its text and items, the layout --- and the
decoded pictures are on the kernel's user heap, 4 MiB for all processes
together until the kernel grows it (by an eighth of the RAM, once, when it is
full), which is why the arena is not: a 700 KiB page needs about 2.5 MiB of it, and every buffer that
grows while others do leaves holes.  So the body gets its Content-Length in
one block, the text and items are reserved from the source's size before
parsing, and all of them give back their growing room when they are done.
Memento and r2web need a kernel whose syscalls accept user-heap pointers
(`USER_REGIONS` in `src/abi/syscall.rs`).

## Tests

The engine builds on the host too (`WEB_HOST`), with the same BearSSL
configuration and roots:

```shell
make -C web/tests check                     # URLs, HTTP, HTML layout, CSS, forms,
                                            # pictures (tests/img/) and PNG round trips
make -C web/tests && web/tests/host_fetch https://news.ycombinator.com/ 76
```

After a load, the status line says where the time went (`dns`, `connect`,
`tls`, `response`, and when the first byte of the body arrived).

`host_fetch` runs the loader against real servers through ordinary sockets ---
the page, then its style sheets --- and prints the laid-out page; it reads the
roots from `cacerts.bin` (or `$WEB_CACERTS`), and `WEB_NOCSS=1` leaves the
page's CSS out.  What only runs on r2 --- `net_r2.cpp`, `web_r2.cpp`
and the window --- was tested in QEMU with the networking above.
