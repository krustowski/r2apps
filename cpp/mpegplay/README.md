# mpegplay

An MPEG-1 video player for r2, shown in memento's **Video** window --- or,
on the graphics kernel, across the whole screen in true colour (F) --- with
sound through the HD Audio driver, from files or from HLS streams.
Decoding is [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT, by Dominic
Szablewski); its MP2 audio decoder is taken back to integers in `mp2.hpp`.

## Making a film for it

For the whole screen on the graphics kernel (HD):

```sh
ffmpeg -i input.mp4 -vf scale=1280:720:force_original_aspect_ratio=decrease:force_divisible_by=2 -r 25 \
       -c:v mpeg1video -b:v 3000k -maxrate 4000k -bufsize 4000k -g 50 -mbd rd -trellis 1 \
       -c:a mp2 -b:a 192k -ar 44100 -f mpeg clip.mpg
```

For the text kernel (16 colours):

```sh
ffmpeg -i input.mp4 -vf scale=320:240 -r 25 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 128k -f mpeg clip.mpg
```

- **1280x720 for the whole screen.** Between half the screen and the
  screen, a film is shown at its own size and enlarged by the kernel, the
  cheapest way there is (see *The whole screen*): 720p costs about 7 ms a
  picture to decode and convert on a recent laptop, 1080p about 16 ms (an
  I-picture two or three times that), which a slow machine will not manage
  at 25 fps.
  In the window, and on the text kernel, the picture is at most 960x600
  and 320x240: larger films are scaled down, but every pixel is still
  decoded.
- **MPEG-1 only.** `-c:v mpeg1video`; MPEG-2 is recognised and refused.
- Either container works: a program stream (`-f mpeg`, `.mpg`) or a bare
  video stream (`-f mpeg1video`, `.m1v`). Sound comes from a program
  stream's first MP2 track (`-c:a mp2`: MPEG-1 Layer II, mono or stereo,
  32, 44.1 or 48 kHz); `-an` makes a silent film.
- `-q:v` sets the quality: 2 is best, 31 is worst. Anything below 6 looks the same
  after dithering to 16 colours. For HD a bit rate (`-b:v`) keeps the file
  size in hand: the tar archive is in memory.

Put the files in `r2_main/iso/video/` before `make build_iso`. They end up
in `/mnt/tar/video` and the Video window lists them. The tar archive is
loaded into memory at boot, so a large collection costs RAM.

## Streams (HLS)

The Video window also plays HLS: an `.m3u8` playlist in the directory, a
`.url` file whose first line is a playlist's address, or an address typed
after pressing **U** (Ctrl+V pastes one copied from the browser). The window
fetches the playlist --- a master playlist's variant with the highest
`BANDWIDTH` up to 6 Mbit/s on the graphics kernel (the lowest on the text
kernel, which shows 320x240); a live playlist again every target duration, from three
segments before its end; a VOD one from the start to `#EXT-X-ENDLIST` ---
and each MPEG-TS segment, over HTTP or HTTPS with the browser's loader, or
from the disk when the playlist is a file. The bytes go to the player
through a 1 MiB ring on the user heap (`streamRing` in the block), and the
player demuxes the transport stream (PAT, PMT, PES) itself.

Only MPEG-1 video with MP2 audio plays, which is not what streaming sites
send (H.264 and AAC, refused with a message); make the stream yourself:

```sh
ffmpeg -re -i input.mp4 -vf scale=640:360:force_original_aspect_ratio=decrease:force_divisible_by=2 \
       -c:v mpeg1video -b:v 1200k -maxrate 1600k -bufsize 1600k -mbd rd -trellis 1 -cmp 2 -subcmp 2 -g 48 \
       -c:a mp2 -b:a 192k -ar 44100 -ac 2 \
       -f hls -hls_time 2 -hls_list_size 6 -hls_flags delete_segments live.m3u8
```

With 256 colours (the graphics kernel) the player converts each picture at
the size the window shows it, so a stream larger than 320x240 is sharper
when the window is maximised, and sharper again on the whole screen (F):
640 wide, at the film's own shape and frame rate, is decoded at full rate on
real hardware. `-maxrate` plus `-bufsize` bounds a 2 s segment near
600 KiB. Drop to 480 wide if the status line counts pictures *late*.

For HD, the same at 1280x720 with `-b:v 3000k -maxrate 4000k -bufsize 4000k`:
a 2 s segment is then about 1 MiB, well under the 4 MiB limit below.

`-g 48` puts one I-picture in each two-second segment (segments start
with one). An I-picture costs several times what a P-picture does to decode,
and a slow machine falls a little behind at each; ffmpeg's default of one
every twelve pictures makes that often.

and serve the directory over HTTP (`python3 -m http.server`). Segments are
cut at 4 MiB, the browser engine's limit (`HttpResponse::MAX_BODY`). A stream cannot be restarted with R.

## Using it

Open **Video** on the desktop, pick a file and press Enter. Then:

| Key | |
|---|---|
| Space | pause / play |
| Up / Down | volume, 10% a press, 0--200% (shown after `sound`; kept for the next film) |
| R | from the start (also after the end) |
| F | the whole screen, in true colour (graphics kernel); F or Esc back to the window |
| I | diagnostics: repaints, network, stream buffer, decoding (see below) |
| Esc, Backspace | stop, back to the list |

The picture is scaled to fill the window with its shape kept, so Alt+F
(maximise) shows it across the screen.

The status line shows the position and the length, and `sound` while the
film's sound plays (or that there is no sound device). For a bare `.m1v`
stream it shows the position only, because that kind of file doesn't record
its length. If the line shows *N late*, the machine is not keeping up: those pictures were
decoded but skipped to keep time. Under QEMU without KVM expect that at
320x240; with KVM (`-enable-kvm`) it plays at full rate.

## How it works

`mpegplay.elf` has no screen of its own. The window allocates a
`VidHostBlock` (`vidhost.hpp`) on r2's user heap, which every process maps
the same way, and starts the player with its address:

    mpegplay.elf --host 0xC12340 /mnt/tar/video/clip.mpg

(The whole screen is different: see *The whole screen*.) The player reads
the file in 128 KiB chunks (syscall 0x39), decodes each
picture, converts it from YCbCr straight to the screen's colours with a 4x4
ordered (8x8 Bayer) dither, colour interpolated like brightness: the 16 EGA colours (through a 4096-entry nearest-colour
table) on the text kernel, or the 6x6x6 colour cube of Memento's 256-colour
palette when Memento runs on the graphics kernel's framebuffer, which looks
far better (the window says which, in the block). Each picture is published
into one of two buffers in the block when it is due. The window copies the newest one
into its own bitmap. Memento's r2 bitmaps are one palette index per pixel,
so this is a row-by-row copy. Both sides beat. The player leaves if the
window goes quiet for ten seconds, and the window checks the task table in
case the player dies. It is the same arrangement as the Editor window and
Turbo C++.

## No floating point, anywhere

r2 does not save a task's SSE or x87 registers when it switches tasks, so a
program that uses them can have them changed under it by another program
that does too. So this one uses none:

- `pl_mpeg_r2.h` is generated from `third_party/pl_mpeg.h` by
  `tools/trim_pl_mpeg.py`, which keeps the buffer, the demuxer and the video
  decoder and turns their timestamps and frame rate into integers (it drops
  the MP2 decoder and the high-level player). `make header` runs it again
  after the upstream file has been updated; each edit checks that it found
  what it expected.
- `mp2.hpp` is pl_mpeg's MP2 decoder (after kjmp2, which was fixed-point to
  begin with) with its synthesis filterbank back in integers. The 32-point
  DCT with Q15 constants and 64-bit products, and the 512-tap window
  doubled, which makes it exact, are generated from pl_mpeg's float
  ones by `tools/gen_mp2.py` (`mp2_synth.inc`). The samples go into the
  filterbank 2^6 larger than pl_mpeg's, so the DCT's truncations stay below
  one bit of the output. `tests/mp2_compare.cpp` decodes the same streams
  with both on the host: 73--76 dB signal to noise against the float
  decoder, which is under one bit of 16.
- Everything is built with `-mgeneral-regs-only`, including the parts of
  libc++r2 that are linked in (the Makefile compiles them again). GCC
  otherwise copies structures through SSE registers.
- `make check-fp` lists every function in the ELF that touches an SSE or
  x87 register. It prints nothing. (It starts at `_start`: the GNU build-id
  note before it is a hash, and can disassemble as anything.)

## The whole screen (F)

In the window a picture is Memento's: palette indices, dithered to the 256
colours, on a desktop at half the screen's resolution (a 1920x1080 screen is
a 960x540 desktop, doubled by the kernel). With F, on the graphics kernel,
the window *holds* Memento's screen (`R2_HoldScreen` in Memento's r2
backend: the loop runs and windows compose as ever, but nothing is sent) and
sets `fullscreen` in the block. The player then draws each picture on the
framebuffer itself, in 32-bit colour, through syscall `0x17`, and says so in
`fsShowing`. F or Esc again, or the Video window losing the focus (a click
elsewhere, Alt+Tab), clears `fullscreen`; the player stops drawing between
two pictures and clears `fsShowing`; only then does the window let Memento
have the screen back, which sends its next frame whole over a wiped screen.
The pictures meanwhile went nowhere else, so the player puts the last one in
the window's buffer on the way out (a paused film stays on it).

`0x17` takes a buffer the size of the screen as it is, or a smaller one and
enlarges it to the whole screen, nearest pixel. The buffer is always the
screen's shape, the film in the middle and black round it, and the picture is
made in one of three ways (`fs WxH` on the I line says which size, `up` when
the kernel enlarged it):

- **At the film's own size** when it is between half the screen and the
  screen (720p or 1080p on a 1080p screen). One colour sample for each 2x2
  block of brightness, as MPEG-1 codes it, and table lookups for the rest:
  7 ms for a 1080p picture, 3 ms for 720p, on the laptop it was measured on.
- **Scaled down to the screen**, smoothly, for a film larger than it; if
  more than one picture in ten comes out late, to half the screen instead.
- **At half the screen, smoothly**, for a film no larger than that
  (640x360 on 1080p), doubled by the kernel.

Scaling smoothly to the full screen costs about 70 instructions a pixel ---
27 ms a 1080p picture, more than decoding it --- which is why a film that
fits is sent at its own size. The buffer is the screen's size in 32 bits (8
MiB at 1920x1080), from the user heap, which grows past its first 4 MiB. It
needs a 32-bit framebuffer; otherwise, or without the memory, the status
line says so and the film stays in the window. On the text kernel F does
nothing (the status line says why).

## Diagnostics (I)

While a film plays, I shows a line over the bottom of the picture, from the
last two seconds: `rep` repaints a second and their average/longest ms;
`pass` the longest pass of Memento's loop (the network waits that long);
for a stream `buf` the KiB waiting for the player, `dl` how fast the last
segment came, `wait` the player's longest wait for stream bytes and how many
waits over 50 ms there were; `dec` the longest decode of one picture; `behind`
how far behind the furthest skipped picture was; `fs` the size of the last
whole-screen picture (see above). A stuttering film shows
which of them it waits on: long repaints and passes are the display, an empty
`buf` with long `wait`s the network, a long `dec` the processor.

## Sound and time

The player decodes MP2 frames until about 350 ms is queued in the kernel's
ring (syscall `0x3f`, `r2/audio.hpp`) and keeps it there. What has been
played --- what was written less what is still queued --- is the clock: a
picture is shown when that reaches its time, and one decoded more than
100 ms (at least three frames) too late is skipped: less than that cannot be
seen, and the pictures after an expensive I-picture catch up. The first time stamps of the audio and the video line the two up.
Pause stops the stream's DMA, so the clock stands still with the picture.
When the audio runs out, or there is no sound device or no audio, the
pictures keep time by the tick counter as before.

The proper fix is in the kernel: `fxsave`/`fxrstor` around the task switch
in `r2_main/src/abi/timer_interrupt.asm`. Until then, memento itself (which
uses `double` throughout) and other SSE-using programs remain exposed to
each other.

## Building

```sh
make build        # mpegplay.elf
make check-fp     # prints nothing when the ELF is free of SSE/x87
make header       # regenerate pl_mpeg_r2.h from third_party/pl_mpeg.h
```

`third_party/pl_mpeg.h` is upstream commit `c871f2b` (2025-12-30), unmodified.
