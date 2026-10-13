# Go apps hosted in Memento

For a small app that draws its own Memento window, start with
[`go/hello-world`](../../hello-world/README.md). Its Go UI supplies event and
paint hooks to `hosted.Run`; the reusable wrappers handle the window loop.

`github.com/krustowski/rou2exOS-apps/go/libgor2/memento` is the reusable window
bridge used by `go/spotify` and `cpp/memento-hello/windows/spotify_window.cpp`.
It supplies launch validation, commands, double-buffered snapshots, heartbeats,
shutdown acknowledgement, runtime text and single-byte font text conversion.
[`host.hpp`](host.hpp) supplies the matching freestanding C++17 host API.

The C++ window allocates the shared block, starts the Go ELF with
`app.elf --host 0x<address>`, forwards user actions as commands and paints the
latest snapshot. The Go process runs the app model and workers. A future app
defines its command numbers and snapshot, then writes a C++ painter using
Memento's existing `PlatformWindow`/`PlatformBitmap` API. The transport also
works with a fixed-size pixel array in the snapshot if Go should render it.

## Define an app protocol

Give each app its own magic and version. Define the same snapshot in Go and
C++, using 32-bit integers and fixed-size byte arrays. Include the C++ helper
and use `r2memento::Block<YourSnapshot>` instead of copying Spotify's block.
The includer supplies `uint32_t`, `uintptr_t` and `size_t`, as libc++r2 does.

Snapshots must have no pointers, strings, slices, maps or interfaces. Their
size must be a positive multiple of four, with alignment at most four. Assert
the size and offsets on both sides; layout and byte order are the r2 ABI.

[`examples/counter/main.go`](examples/counter/main.go) and its
[`host.hpp`](examples/counter/host.hpp) are a complete Go child and matching
protocol. The example accepts an increment command and publishes a count and
label. Build it with the installed TinyGo r2 image:

```sh
make -C go/libgor2/memento/examples/counter build
```

Install `wincount.elf` where Memento can spawn it, then add a window wrapper
alongside `SpotifyWindow` with the event hooks below.

## The Go child

The counter uses this loop:

```go
host, err := memento.Attach[Snapshot](r2.Args(), magic, version, r2.Ticks())
if err != nil {
    fmt.Println(err)
    return
}
r2.SetConsoleSink(host.RuntimeOutput)
for host.Poll(r2.Ticks()) == memento.Running {
    if host.DrainCommands(handleCommand) != memento.Running {
        break
    }
    host.Publish(&snapshot)
    time.Sleep(20 * time.Millisecond)
}
// Stop/join any workers and persist state before acknowledging shutdown.
host.Close()
```

`Attach` checks `--host`, alignment, that the entire block lies in the shared
user heap, magic and version. The heap is the 4 MiB from `0xC00000` and, once
they are full, the extension the kernel adds below 1 GiB (from `0xA000000`, or
past the tar archive); on r2 the kernel is asked (syscall `0x43`, through
`libgor2.SharedHeapContains`) before the block is read. It accepts extra app
arguments after the address. `Connect` takes an already mapped block and is
useful for ordinary Go tests.

`Poll` checks `Quit`, a ten-second host heartbeat timeout, and queue bounds.
The first stop reason is retained and written into the block. It does not
acknowledge shutdown: workers and persistence still need time to finish.

`DrainCommands` processes the queue pending at entry and acknowledges each
slot before calling the handler. Commands arriving during a handler remain
for the next drain. The 32-entry ring works across 32-bit counter wraparound.
Invalid counters cause `ExitBadQueue` before reading any slots.

`Publish` copies a complete snapshot. If the host still leases the back
buffer, it returns false; retry the latest state next time. The client never
gives app code pointers into shared snapshots.

Keep `Poll`, `DrainCommands` and `Publish` on one goroutine. Workers use Go
channels to exchange app state. `Close` is the final shared-memory operation:
after it, the host may free the allocation, and further client calls do not
touch it. Call it explicitly after cleanup; deferring it across a panic would
acknowledge release before the runtime finishes reporting the crash.

`RuntimeOutput` preserves the first 127 diagnostic bytes and sets `ExitRuntime`
without allocating or yielding, so it works when the Go heap is exhausted.
`SetConsoleSink` also receives ordinary console output; use snapshots for
normal UI status and reserve the sink for diagnostic output. A custom sink
can call `RuntimeOutput` and additionally write an app-specific log, as Spotify
does. `memento.Text` writes zero-terminated ASCII for Memento's small font and
replaces unsupported Unicode/control characters with `?`.

## The C++ window

With the counter's header included, initialize the shared block and launch:

```cpp
auto *block = static_cast<CounterBlock *>(r2::heap::kernel_allocate(sizeof(CounterBlock)));
// Handle allocation failure before using block.
memset(block, 0, sizeof(*block));
CounterHost host(block);
host.initialize(COUNTER_MAGIC, COUNTER_VERSION);
char args[64];
if (r2memento::launchArguments(args, sizeof(args), "wincount.elf", block)) {
    auto pid = r2::spawn("wincount.elf", args);
    // Retain pid for task monitoring; release the allocation if spawning fails.
}
```

Keep a local `CounterSnapshot` and a `uint32_t shownFrame = 0`. In the window's
`OnImmediateModeIdleLoop` handler, beat and copy before requesting repaint:

```cpp
host.keepAlive();
if (host.read(snapshot, shownFrame)) wnd->Repaint();
```

In `OnKeyEvent`/`OnMouseClick`, call `host.send(CounterIncrement)` for an
increment action. A false return means the ring is full or the child has
exited; choose whether to retry that action. Paint the **local** snapshot in
`OnPaint`. `Host::read` takes and releases the snapshot lease internally.
Register `keepAlive()` with desktop/background paths that block normal window
idle events, such as Memento's fullscreen-app wait loop.

On close, call `host.requestClose()`, keep beating during the bounded cleanup
wait, then release the allocation only when `host.exited()` or the task table
confirms the child has stopped. A failed task-table read must be treated as
still alive. If a live child exceeds the wait, retain the allocation and stop
heartbeats so it eventually times out; never free memory it may still use.
Task monitoring, allocation lifetime, window creation and painting remain the
window wrapper's responsibility. Spotify's wrapper shows this integration.

The helper uses sequentially consistent 32-bit atomics to match Go's
`sync/atomic`, including the lease/recheck handshake. Each direction has one
writer: the host owns commands, heartbeat, reader lease and quit; the child
owns snapshots, frame, heartbeat, command tail, exit reason and acknowledgement.
Read `runtimeText` after acknowledgement or confirmed task death.

## A window the program draws: package hosted

### Window hooks and drawing

`hosted.Run` (on the `r2` target) takes `Options` and `Hooks`. A minimal app is:

```go
//go:build r2

package main

import (
    "fmt"
    memento "github.com/krustowski/rou2exOS-apps/go/libgor2/memento/hosted"
)

func main() {
    err := memento.Run(memento.Options{
        Magic: 0x31484f47, Title: "Hello / Go", Width: 480, Height: 240,
    }, memento.Hooks{
        OnPaint: func(w *memento.Window, c *memento.Canvas) {
            width, height := w.Size()
            c.Text(memento.Rect{X: 16, Y: 16, W: width - 32, H: height - 32},
                "Hello, world!", memento.Black)
        },
    })
    if err != nil { fmt.Println(err) }
}
```

The matching host must supply that magic. [Hello-world](../../hello-world/README.md)
embeds build-time window metadata and opens through **Files > Run in new window**
(Ctrl+Enter), using the generic hosted-window adapter. `Run` consumes
the process's `--host` arguments, installs the runtime diagnostic sink, polls
the host, drains bounded batches of input and retries busy frame buffers.

`OnOpen` initializes state. `OnEvent` receives `Command` values; `cmd.Key()`
decodes keys and `cmd.LeftPressed(rect)` tests button presses. Resize is
applied before the event hook and invalid sizes are ignored. `OnPaint`
receives a freshly cleared light-grey canvas only when its frame is leased.
Input requests a repaint automatically. `OnIdle` can consume worker results
and call `Repaint`; it receives boot time in milliseconds. `OnClose` receives
the stop reason and runs before shared memory is released. All hooks run on
one goroutine; join workers before returning from `OnClose`.

`Window.Size`, `SetTitle`, `Repaint`, `Close` and `Fail` control the contents
and lifecycle. `Colours` and `PortBase` expose the host's palette and reserved
network ports. `Copy`, `Open`, `Attention`, `Notifications` and `Notify` wrap the
existing desktop services. `Copy`/`Open` return false while their mailbox is
busy; `Notify` returns false if notifications are unavailable or its queue is
full. Apps needing guaranteed delivery should retry from `OnIdle`.

`Canvas.Clear`, `FillRect`, `Text` and `Button` draw in client pixels and clip
to the visible area. Text is code page 437, one byte per glyph, using the
same 6x12 Terminus table as Memento. The named palette indices work with both
the 16-colour VGA and 256-colour framebuffer backends. To regenerate the
font from the sibling Memento checkout:

```sh
cd go/libgor2/memento/hosted
go run tools/gen_font.go ../../../../../Memento/src/ui/platform/impl/r2/R2_TerminusFont.h
```

Canvas references are valid only inside `OnPaint`; do not retain them, use
them from workers or nest drawing calls. The low-level `Client.Paint` also
provides this direct drawing path for apps that keep their own event loop.
It does not allocate a pixel buffer or copy a completed frame. The host's
slot lease protects drawing from concurrent presentation. `Publish` remains
available for apps with their own pixel buffers.

### Launch from Files

Set `MEMENTO_APP := 1` before including `go/Makefile.tmpl`. In a host-only
`main_host.go`, handle the build's `--memento-manifest` argument by calling
`hosted.WriteManifest(os.Stdout, options, pictures, notifications)`. Use the
same `Options` as the r2 entry point; hello-world's host main is a complete
example. The build runs this inside the Go container and uses Python 3 to
append a 128-byte descriptor outside the ELF's load segments.

Files reads that descriptor to get the protocol magic, title, initial pixel
size and clipboard/notification features, then launches the selected absolute
path with `--host`. The ELF can be renamed or moved without a desktop registry
entry or a C++ window subclass. Paths must fit the kernel's 63-byte limit and
have no spaces. Titles are 1..79 printable ASCII bytes and dimensions are
1..2048 pixels. The app must already implement the version 1 hosted protocol;
metadata does not turn a console or full-screen app into a windowed app.
See [HOSTED_APPS.md](../../../cpp/memento-hello/HOSTED_APPS.md) for the binary
format and how other languages can add a descriptor.

### Low-level hosted transport

When a program would rather draw its whole window than have Memento paint a
snapshot, [`hosted`](hosted/hosted.go) is the child side of Memento's
`HostedWindow` (`cpp/memento-hello/windows/hosted_window.cpp`), the window
`r2web.elf`, `jug.elf` and `telegram.elf` are shown in.  The block is
`r2web::HostBlock` (`cpp/r2web/host.h`) under the app's own magic: the window's
keys, clicks, wheel and size come in as commands, and the program publishes
frames of palette indices (16 or 256 colours, as `Colours` says) into two
buffers after the block.  Nothing app-specific is compiled into Memento; its
window is a few lines:

```cpp
class TelegramWindow : public HostedWindow {
public:
    static const int W = 300, H = 170;
    TelegramWindow() : HostedWindow("telegram.elf", "Telegram", tghost::Magic, 0, nullptr, true) {}
};
```

The Go loop:

```go
host, err := hosted.Attach(r2.Args(), magic, r2.Ticks())
var cmd hosted.Command
for host.Poll(r2.Ticks()) == hosted.Running {
    for host.Next(&cmd) {
        // cmd.Op: OpKey (cmd.Key()), OpMouseMove, OpMouseButton, OpWheel, OpResize
    }
    if dirty && !host.Publish(pixels, w, h, "Title") {
        // Memento is still reading the other buffer: try again next turn
    }
    time.Sleep(10 * time.Millisecond)
}
host.Close() // the last access: Memento may free the block
```

`Copy` puts text on Memento's clipboard, `Attention` turns the window's title
and taskbar button red until it is looked at, `Open` asks for a Web window,
and `Fail` leaves a message the window shows after the program has ended.
`RuntimeOutput`, given to `libgor2.SetConsoleSink`, makes a panic's own words
that message, without allocating.  An ended program's window offers Enter to
start it again in place.
`Notifications` reports whether the host opted into clock bubbles; `Notify`
queues a bounded message without changing the version 1 block layout.
`PortBase` is the first of the 32 TCP ports Memento sets aside for the
window's slot.  A window built with `pictures` set hands the program a PNG of
the clipboard's picture on a Ctrl+V (`PasteImage`).

## Checks

```sh
make -C go/libgor2/memento test
make -C go/telegram test
make -C go/spotify test
make -C cpp/memento-hello check
```

The package tests run on stock Go, including the race detector. The C++ test
checks host behavior and both generic and Spotify ABI sizes/offsets. Spotify
continues to use its existing 4040-byte version 4 block and app-specific UI.
