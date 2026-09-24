# web — the browser engine behind Memento's Web window

A small web browser for rou2exOS: HTTP/1.1, TLS 1.2 by
[BearSSL](https://bearssl.org/), and HTML without scripts or style sheets,
drawn in the kernel's own fixed-width font.  The window is
[`../windows/browser_window.cpp`](../windows/browser_window.cpp); everything
it stands on is here.

```
browser_window.cpp   toolbar, page, status line, keys, history
      │
   loader            resolve → connect → TLS → request → response → redirects
   │   │   │
   │   │   └── http      request line, response parser (chunked, lengths, 100)
   │   └────── tls.c     BearSSL's non-blocking engine; roots from cacerts.bin
   └────────── NetIf     net_r2.cpp on r2, sockets in tests/host_fetch.cpp
doc                  HTML → items → lines and runs, in character cells; forms
css                  the CSS subset: selectors, cascade, @media
url                  what the user typed, and links resolved (RFC 3986)
web_r2.cpp           memory, clock, entropy and the date, on r2
```

Nothing blocks.  The window turns the loader from Memento's idle loop, the
loader turns the network stack, and the TLS engine is BearSSL's low-level one,
which never touches a socket: bytes go in and come out through its buffers.

## What it shows

Headings (h1 and h2 at twice the size), paragraphs, lists (bullets, circles,
numbers), block quotes, definition lists, `<pre>`, tables read row by row,
links (underlined, walkable with Tab), bold (the font has none: the glyphs are
drawn twice, one pixel apart), colours, horizontal rules, image `alt` text,
and forms.  `<script>`, `<svg>` and friends are skipped.

Text is decoded from UTF-8, windows-1252/ISO-8859-1, windows-1250 and
ISO-8859-2, then mapped to CP437, which is what the font is for letters and
the Latin-1 symbols.  What CP437 does not have loses its accent (č → c,
ř → r); the font's box-drawing and Greek rows are not CP437's, so those are
not used.  `tools/gencharmap.py` generates the tables in `charmap.inc`.

Not done: JavaScript, images, cookies, compression (the request asks for
`identity`), IPv6, TLS 1.3.

Pages are cut at 768 KiB.  The body, the document's text and its layout live
on the kernel heap (`big_alloc` in `web_r2.cpp`), which none of them ever
passes to a syscall; build with `EXTRA=-DWEB_BIG_ARENA` to keep them in the
arena instead.

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
JavaScript; the status line says so.

Each control sits in the page's text as a run of placeholder cells and in the
link table as an entry of its own (`Document::linkControl`), so Tab, Enter,
Space and the mouse reach it like a link; the window draws the cells from the
control's state.

## TLS

- TLS 1.2 only, ECDHE (P-256, P-384, X25519) with AES-GCM or
  ChaCha20-Poly1305 first, then AES-CBC and static RSA for older servers.
- The certificate chain must lead to one of the roots in
  `/mnt/iso/opt/memento/cacerts.bin`, read from the CD on the first
  handshake: 36 roots (Let's Encrypt, DigiCert, Google, Amazon,
  Sectigo/USERTrust, GlobalSign, Microsoft, GoDaddy/Starfield, Entrust,
  SSL.com, Certum) in 13 KiB.  `tools/mkcacerts.sh [bundle.pem] [iso-dir]`
  regenerates `cacerts.bin` (and copies it into `iso-dir/opt/memento`);
  r2_main's `build_iso` copies it onto the CD.  `tools/mkcacerts.c` describes
  the format.  Without the file every certificate is unknown, and the error
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
- One stack per process, and it shares the process's frame queue with
  c/libcr2's stack in the Chat and IRC windows: use one or the other at a time.
- TCP: one segment in flight when sending; three duplicate ACKs at once on
  a gap; retransmission with backoff.
- Receiving is shaped by the kernel.  It moves one frame per millisecond tick
  into a single buffer (`poll_and_deliver` in the kernel's `net/netdrv.rs`),
  and the next frame overwrites it whether or not it was read.  Memento's
  loop is regularly busy for longer than a tick --- a repaint, a TLS record
  to decrypt --- and every frame of a burst but the last is lost in such a
  stretch.  So the window offered is two segments (`WEB_RX_SEGMENTS`), the
  window reopens as soon as reading frees a segment (a sender that avoids
  silly windows otherwise waits out its persist timer: seconds per KiB), and
  the window repaints the status line twice a second at most while loading.
  Measured in QEMU with packets released in 20 ms batches, a 768 KiB page
  took 16 s with two segments, 20 s with one and 53 s with four; `about:net`
  counts the frames that went missing.
- The real fix is in the kernel: a frame buffer per queued message, and
  frames left in the NIC's ring while the receiving process's queue is full.
  With that, larger windows would be safe and much faster.

## Memory

The whole Memento process is 2 MiB (see libc++r2's README).  The browser adds
about 165 KiB of text --- BearSSL 68, the engine (with CSS and forms) the rest
--- built with `-Os`, which leaves about 21 KiB below the 0x800000 line.  Check with
`nm -n memento-hello.elf | grep ' _end$'` after adding anything.

## Tests

The engine builds on the host too (`WEB_HOST`), with the same BearSSL
configuration and roots:

```shell
make -C web/tests check                     # URLs, HTTP, HTML layout, CSS, forms
make -C web/tests && web/tests/host_fetch https://news.ycombinator.com/ 76
```

After a load, the status line says where the time went (`dns`, `connect`,
`tls`, `response`, and when the first byte of the body arrived).

`host_fetch` runs the loader against real servers through ordinary sockets ---
the page, then its style sheets --- and prints the laid-out page; it reads the
roots from `cacerts.bin` (or `$WEB_CACERTS`), and `WEB_NOCSS=1` leaves the
page's CSS out.  What only runs on r2 --- `net_r2.cpp`, `web_r2.cpp`
and the window --- was tested in QEMU with the networking above.
