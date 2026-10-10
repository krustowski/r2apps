# telegram

The Telegram client Memento's **Telegram** desktop icon opens.  It used to be
a window compiled into Memento (`cpp/memento-hello/windows/telegram_window.cpp`);
it is now `telegram.elf`, a Go program in a process of its own, and Memento
only hosts it: the window passes keys, clicks and its size in and shows the
frames telegram.elf draws, the way the Web and Jug windows host `r2web.elf`
and `jug.elf`.  Everything else --- the Bot API, HTTPS, pictures, GIFs --- is
here.

It is a bot: ask @BotFather for one (`/newbot`), give telegram.elf the token,
and anyone who writes to the bot shows up as a chat on the left.  What you type
is sent back as the bot.  Telegram's own apps use MTProto, with its own key
exchange and a login by phone number; the Bot API is plain HTTPS and JSON.

## Build and install

```sh
make            # telegram.elf, in the tinygo-r2 image (cd ../tinygo-r2 && make image, once)
make test       # the client, its drawing and the decoders, on the host
make install    # TELEGRAM.ELF onto ../../fat.img
```

`make` builds the C half on the host first --- `media/native` with stb_image
and h264bsd, the way Memento's web engine builds them, and BearSSL in
`../r2tls` --- then links it in with TinyGo.  The ELF is about 650 KiB.

r2_main's `make build_iso` copies `telegram.elf` into `iso/bin`, so it is in
`/mnt/tar/bin` (and `/mnt/iso/bin`) where Memento starts it from.  For a quick
test without a new ISO, put `TELEGRAM.ELF` on the floppy and `cd /mnt/fat`
before `fg memento`: the kernel looks in the working directory first.

## The token

Read from `/mnt/tar/opt/memento/telegram.txt`, a line shipped on the boot
medium (`iso/opt/memento/telegram.txt` in r2_main), else from
`/mnt/fat/TELEGRAM.CFG`.  With neither, the window asks for it, and a token
typed in is kept in TELEGRAM.CFG.  Ctrl+T asks again.

For tests, a line `api=http://host:port` in either file sends every request
there instead of `https://api.telegram.org` --- plain HTTP to a mock Bot API,
reachable from QEMU at the user network's host address (`10.3.4.1` with
`net=10.3.4.0/24,host=10.3.4.1`).

## Keys

| Key | |
| --- | --- |
| Enter | send; Shift+Enter is a line break (for a ` ```code block``` `) |
| Tab, Shift+Tab | next, previous chat (a click on one too) |
| Up/Down, PgUp/PgDn, wheel | scroll |
| Ctrl+Space, Alt+Space, right click | the menu on a message: 1-9 a reaction, 0 none, R reply, C copy the text, G copy the GIF; PgUp/PgDn move it to an older/newer message |
| Ctrl+C | copy what is typed, or with nothing typed the chat's newest message |
| Ctrl+V | paste; a PrintScreen is attached as a screenshot, a copied GIF as that GIF, and Enter sends it with what is typed as its caption (Backspace on an empty line takes it off) |
| Ctrl+S, Settings button | open/close settings; click Notifications or press Space/N/Enter to toggle; Esc closes settings |
| Esc | not reply after all; then close |

`inline` and ` ```block``` ` in what is typed go out as code entities, the way
Telegram's own apps make them, and code in messages coming in is drawn in a
box.  The font has no emoji, so reactions are shown by a few letters (`<3`,
`+1`, `fire`).

## How it works

```
Memento                        telegram.elf
HostedWindow ── block on the ──  hosted.Client      input in, frames out
(windows/hosted_window.cpp)       user heap          (go/libgor2/memento/hosted)
                                     │
                                  App                the client: chats, messages,
                                     │                 menu, painting (app.go, paint.go)
                       jobs ─────────┼───────── results
                                     │
                                  worker             one request at a time over
                                                     r2net and r2tls (net_r2.go)
```

**The window.**  The block is `r2web::HostBlock` (cpp/r2web/host.h) with its
own magic (`host.h`): an input queue, two frame buffers of palette indices,
heartbeats both ways, and mailboxes for the clipboard and the window's
attention.  telegram.elf draws at Memento's 2 pixels a unit with Memento's own
6x12 Terminus font (`font.go`, generated from Memento's sources by
`make gen`), in the 16 EGA colours, which mean the same on the VGA and on the
graphics kernel's 256-colour framebuffer.

Incoming messages (including photos and GIFs) and added reactions also show
six-second bubbles above the taskbar clock, even while Telegram is focused
or minimised. Each bubble names the chat and shows a short preview; the
newest sits nearest the clock and older bubbles stack upward. Memento keeps
up to twelve visible alerts, each with its own expiry, and retains the newest
on overflow. Outgoing messages, our own reactions, reaction removals and
unchanged reaction totals do not alert.
Incoming messages also mark the window title and taskbar button for attention
until focused. Reactions show their clock bubble without marking the window.

**Notification settings.** Notifications are on by default. Open **Settings**
at the bottom left (or press **Ctrl+S**) and uncheck **Notifications** to stop
clock bubbles and window attention. Changes save immediately as
`notifications=on` or `notifications=off` in `/mnt/tmp/TELEGRAM.CFG`; each new
Telegram window reads that preference before receiving updates. Muting drops
the client's pending bubbles. Alerts already displayed finish their six-second
expiry. The RAM disk keeps the setting for this OS session; token and API
configuration continue to use their existing files.

The host opts in to a notification queue in the Telegram window's unused
`initialUrl` space: eight 96-byte slots, with head/tail words and a capability
magic (`cpp/r2web/host.h`, `hosted.Client.Notify`). The Version 1 shared block
and pixel offsets stay the same. A burst waits in a bounded 32-entry client
backlog while the host queue fills; older Memento builds continue to use
window attention. `make -C ../../cpp/memento-hello notifytest` checks stack
expiry and the host queue; `make test` checks incoming events and retries.

**The network.**  One request at a time: `getMe` once, then
`getUpdates?timeout=20`, a long poll the server holds until something
arrives.  What is typed or picked (`sendMessage`, `sendAnimation`,
`sendPhoto`, `setMessageReaction`) stops a poll in progress; its updates come
again, since the offset only moves past what has been read.  One connection
is kept from one request to the next (HTTP/1.1 keep-alive), so the TLS
handshake --- seconds on a slow machine, and the step most likely to fail ---
happens once rather than with every request; a poll stopped half way takes its
connection with it, and a kept connection the server has closed is replaced
(a GET goes again; a POST only when nothing at all came back).  The worker owns
the r2net stack (r2net is driven from one goroutine) and the client's goroutine
runs while it waits.  It takes four TCP ports from the 32 Memento sets aside for
the window's slot (`48000 + slot*32`), so it runs alongside the Web and Spotify
windows, and opens its stack again after a connection that could not be made:
a stack opened before `eth` had published the address and DNS server would
otherwise never learn them.

**Pictures.**  A photo is fetched at the size nearest 320 pixels wide and
dithered into the screen's colours (`media`, which is Memento's
`web/image.cpp` and `web/mp4.cpp` in Go, held to the same unit tests).  A GIF
sent as a file moves, every frame of it.  Most "GIFs" are MP4s of H.264, as
Telegram turns them into: Telegram's still comes first, then the MP4, decoded
by h264bsd a few milliseconds at a time to move in place of the still
(Baseline profile only, which is what GIF sites serve).  At most three GIFs
move at once.  Pixels live on the kernel's user heap, outside Go's collector.

**Screenshots.**  PrintScreen puts the screen on Memento's clipboard.  The
Telegram window asks for pictures, so a Ctrl+V in it writes a PNG of the
clipboard to `/mnt/tmp/CLIP.PNG` and tells telegram.elf where and how long it
is (`r2web::PasteImage`), which sends it with `sendPhoto`.

Uploads use TCP progress to renew their idle timeout, honor the server's
segment-size limit and cap outgoing payloads at 1024 bytes for smaller internet
paths. The response gets a fresh timeout after the complete multipart body is
sent. Upload failures in the diagnostic log include bytes written/total bytes.

## When something goes wrong

A request that fails says why on the status line and is tried again after
five seconds.  `/mnt/tmp/TELEGRAM.LOG` (Memento's Files window, F3) keeps the
last requests --- what each took, whether it needed a new connection, how it
ended --- with the Go heap at the time, and a crash's last words; requests are
logged by method only, never by path, which holds the token.

Should telegram.elf end anyway, its window says why (a panic's message, or
where it crashed) and Enter starts it again in the same window.

## Files

| File | |
| ---- | --- |
| `main.go` | r2: attach to the window, the loop, the worker. |
| `diag_r2.go` | The diagnostic log and the crash capture. |
| `app.go` | The client: token, chats, messages, menu, keys, which request is next. |
| `paint.go`, `canvas.go` | Drawing the window. |
| `json.go`, `text.go` | The Bot API's JSON read straight into code page 437, with code marks; layout. |
| `api.go`, `net.go`, `net_r2.go` | What goes out, HTTP over a kept connection, the network worker. |
| `media/` | Pictures, GIFs and MP4s; `native/media.c` is the C bridge to stb_image and h264bsd. |
| `font.go`, `charmap.go`, `tools/gen.go` | Memento's font and Unicode map, generated. |
| `host.h` | The magic Memento's window uses. |
