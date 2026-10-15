# Minesweeper in a Memento window

`mines.elf` is the Minesweeper of Memento's desktop: the Mines icon opens a
hosted window and starts it there, as Telegram and Snake are started. Memento
supplies the frame, the taskbar entry and the input; this Go program draws the
contents with Memento's font and colours, through
[`libgor2/memento/hosted`](../libgor2/memento/README.md). It used to be built
into Memento (`windows/mines_window.cpp`); it is the same game, moved out.

The three boards everyone knows: 9x9 with 10 mines, 16x16 with 40 and 30x16
with 99. The first cell opened is never a mine, nor is any around it, so a
game starts with an opening rather than a guess.

| Input | Does |
| --- | --- |
| left click, Space or Enter | open a cell |
| right click, F | flag it (again: take the flag off) |
| click or Space on an open number | when its flags are all there, open the rest of its neighbours |
| arrows | move the keyboard cursor |
| N, or the face | a new game |
| 1 2 3 | beginner, intermediate, expert |
| Esc | close |

Best times, one per level, are kept in `/mnt/fat/MINES.HSC` ("12 85 0", the
file the built-in window used). The level last chosen is remembered on the
RAM disk in `/mnt/tmp/MINES.CFG` until reboot. A maximised window shows the
whole board larger, in whole pixels, with text twice the size from one and a
half times the layout.

## Files

| File | What |
| --- | --- |
| [`game.go`](game.go) | the rules: levels, laying the mines, flood fill, chords, flags, the clock |
| [`app.go`](app.go) | the window: layout and drawing, mouse and keys, the two files, sounds |
| [`main.go`](main.go) | `hosted.Run` on r2, with the r2 file and speaker syscalls |
| [`main_host.go`](main_host.go) | on the build host, the window metadata for the ELF |
| [`game_test.go`](game_test.go) | rules, records, levels and input, on stock Go |

## Build and test

```sh
make -C go/mines build      # mines.elf, with Memento's window metadata
make -C go/mines test       # go test -race, on the host
make -C go/mines install    # MINES.ELF onto ../../fat.img
```

The build uses the `tinygo-r2:0.38.0` Docker image (`make -C go/mines image`
once if it is missing) and Python 3 to append the descriptor ("MNS1",
492x318 pixels, "Minesweeper"). Files' **Run in new window** on `MINES.ELF`
opens it from that metadata as well.

r2_main's ISO rule copies `mines.elf` to `/mnt/tar/bin`. Memento finds it by its
`mines_exe` setting (Settings > Programs > Games executables), `mines.elf` by
default: a bare name follows the kernel's search order (Jug updates, the
working directory, `/mnt/tar/bin`, `/mnt/iso/bin`); an absolute path selects
that file. `cpp/memento-hello/obj/hosted_app_test mines.elf 0x31534e4d 492 318 0
Minesweeper` checks the descriptor as Memento reads it.
