# Hello, world in a Memento window

`gohello.elf` is a small Go process hosted by Memento. The desktop supplies its
movable, resizable window and taskbar entry. Go draws the contents with
Memento's Terminus font, palette and beveled button. Click **Say hello** or
press Enter to copy a greeting and show a clock notification; Esc closes it.

The complete UI is in [app.go](app.go). It imports
[`libgor2/memento/hosted`](../libgor2/memento/README.md) and supplies two hooks:
`OnEvent` for input and `OnPaint` for drawing. `Run` handles attachment,
heartbeats, resizing, frame retries, panic reporting and shutdown. Drawing
uses the host's shared pixel buffers, so even a maximized window does not
need a second canvas in the Go heap.

From the repository root:

```sh
make -C go/hello-world build
make -C cpp/memento-hello build
make -C go/hello-world test
make -C cpp/memento-hello hostedapptest
```

The Go build uses the existing `tinygo-r2:0.38.0` Docker image and Python 3
to append window metadata to the ELF. If needed,
create it first with `make -C go/hello-world image`.

Copy `go/hello-world/gohello.elf` to the boot medium. For a FAT floppy,
`make -C go/hello-world install FLOPPY=/path/to/fat.img` writes `GOHELLO.ELF`.
In Memento's **Files** window, select that ELF and choose **Run in new window**
from its right-click menu (also Alt+Space or Shift+F10), or press **Ctrl+Enter**.
The executable is launched by its full path, independently of the desktop's
working directory. No app-specific desktop icon or executable setting is needed.

To make another app, copy this directory, choose an 8.3 ELF name and a unique
magic in `app.go`, and change the hooks. Keep `MEMENTO_APP := 1` in the Makefile.
The host entry point writes a manifest from the same `Options` used by the app;
the build appends it to the ELF. Files reads the title, size, magic and features
from that metadata and uses its generic hosted-window adapter. No C++ changes
are needed. Pass `true` as the last argument of `WriteManifest` to enable `Notify`.

All sizes and drawing coordinates in Go are pixels. The ELF must implement
the hosted-window protocol; ordinary console/full-screen programs cannot be
put in this window by adding metadata alone. Files reports missing or unsupported
window metadata instead of starting those programs. See
[the protocol notes](../../cpp/memento-hello/HOSTED_APPS.md) for other languages.
