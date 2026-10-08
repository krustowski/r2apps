# streamd

Streams r2's displayed VESA framebuffer as 640x480 MJPEG at a target of 30 FPS,
including Memento windows, without Memento's mouse cursor. Open
`http://10.3.4.2:8080/stream` in a browser or an OBS Browser Source. The server
handles one stream client at a time.

Build with `make build`; this also rebuilds libcr2 when its sources change.
libcr2 uses `-O2`, and streamd uses `-O3` with LTO. Library objects are isolated
in `c/build/libcr2`, so unrelated application objects cannot enter the archive.
Run the host TCP regression tests with `make test`. They cover both JPEG
sampling modes, delayed ACKs, cross-frame pipelining, loss/window recovery,
MSS negotiation, packet bounds and sequence-number wraparound. They simulate
the receiver and clock; they do not boot r2 or run theM.

The sender keeps each HTTP/JPEG buffer until cumulatively acknowledged.
It pipelines up to 32768 bytes within the peer's advertised receive window,
retransmits the oldest missing bytes after 100 ms (backing off to 2 seconds),
and probes closed windows. This prevents a lost TCP segment or ACK from
leaving it waiting indefinitely when other applications increase load.

After 15 seconds without ACK progress, streamd resets the client and resumes
accepting connections. Reconnect or refresh the stream source if needed;
streamd itself does not need restarting. Timeout diagnostics, including the
unacknowledged byte count and peer window, go to `/mnt/tmp/STREAMD.LOG`.
The log is replaced whenever streamd starts.

To validate on r2, start a stream, launch theM with a DOS game in Memento,
and check that new frames continue. If it freezes again, inspect STREAMD.LOG
and check whether refreshing the stream reconnects successfully. The host
tests verify TCP recovery; whether theM triggers packet loss on r2 still
requires this runtime check.

## Floating-point state during multitasking

The kernel must save and restore x87/MMX/SSE state on every timer context
switch. Without it, theM's SSE instructions overwrite the JPEG encoder's
registers, which can corrupt encoding or stop frame generation. The kernel
fix is in `r2_main/src/abi/timer_interrupt.asm` and
`r2_main/src/task/scheduler.rs`: an aligned FXSAVE area travels with each
saved kernel stack, and new processes start with reset floating-point state.
Rebuild and boot the graphics kernel as well as the applications; replacing
streamd.elf alone cannot fix this kernel bug.

`make fpu-test` exercises the sibling kernel's actual timer assembly in host
mode, verifying 10,000 restores of x87, MXCSR and XMM0-XMM15. It fails on the
original timer stub. `tests/fpu/r2_context_test.c` is the r2 test program;
launch two instances (`bg fputest one` and `bg fputest two`) to check state
isolation across 300 sleeps per process. Results go to QEMU debugcon (0xe9).
The patched graphics kernel passes both instances; the original kernel
fails the initial-state check because the new process inherits live state.

## Frame rate and latency

The current defaults target 30 FPS at 640x480, JPEG quality 2. Each multipart
frame remains contiguous (header, JPEG, trailing CRLF). Two JPEG buffers
allow the next frame to be encoded and transmitted before the preceding
frame's final ACK arrives. The sender retains both buffers until cumulatively
acknowledged. Each JPEG buffer starts at 64 KiB, grows on demand up to 1 MiB,
and releases excess capacity after ACK retirement. This avoids reserving two
megabytes of JPEG storage on a small shared heap. The sender can replace only
a completely unsent tail with a newer picture. When both buffers have
transmitted bytes, capture skips a deadline
instead of building a queue of old frames. The JPEG encoder pumps networking
every 16 output rows, with bounded receive/send batches and no sleeps in the
row hook.

TCP segments use up to 1300 payload bytes, further limited by the MSS in the
peer's SYN (536 bytes if absent). The kernel TCP request buffer limits payloads
to 1370 bytes. The transmit pipeline holds up to 32 KiB within the receiver's
window. TinyJPEG's entropy writer appends bytes directly instead of calling
the freestanding memcpy for each byte. Solid-colour blocks encode only their
DC coefficient, skipping the DCT and AC scan; other blocks retain the fast
AA&N DCT.
Frame deadlines use 33/34 ms intervals and skip missed deadlines;
frames are captured on demand, so slow encoding/networking cannot create a
queue of old pictures.

30 FPS requires enough CPU and network bandwidth. Capture/encoding plus TCP
service should leave headroom within 33.3 ms, and sustained payload throughput
must exceed `average JPEG bytes * 30`. `send_ms` includes queue residence and
ACK latency, which now overlap encoding; do not add it to `jpeg_ms` to infer
the frame period. Retain the kernel floating-point context fix beside theM.

The default 4:4:4 mode preserves full colour resolution for desktop text. For
games/video, use 4:2:0, which averages each 2x2 chroma region and halves the
number of DCT blocks:

```sh
make build JPEG_SUBSAMPLING=420
# Reduce bandwidth further, at a visual quality cost:
make build JPEG_SUBSAMPLING=420 JPEG_QUALITY=1
# Restore the desktop profile:
make build JPEG_SUBSAMPLING=444 JPEG_QUALITY=2
```

Quality 2 divides the base quantization tables by ten and can produce much
larger JPEGs than quality 1. 4:2:0 may soften coloured text. Run `make jpeg-test`
(requires Python Pillow) to independently decode solid-colour, odd-dimension,
quadrant, noisy and RGBA fixtures in both modes. `make bench` runs the encoder
benchmark on the host; it does not include kernel capture, multitasking or TCP.

## Complete pictures during movement

Memento now brackets all dirty row bands as one presentation. It restores
the pixels beneath the mouse cursor in the composed surface before ending
the presentation. The cursor stays visible on the local display, while the
kernel samples the full cursor-free indexed surface into a 640x480 RGB24
snapshot in shared RAM. Two snapshot banks let streamd read the previous
complete picture while the next one is drawn. A bank leased by a capturer
cannot be overwritten; leases and presentation ownership are released if a
process exits or crashes. The buffers are allocated lazily and retain about
1.8 MiB for the kernel's lifetime.

Cursor exclusion requires rebuilding Memento with the updated r2 backend in
the sibling `Memento` checkout and installing the resulting `MEMENTO.ELF`.
The existing snapshot-capable graphics kernel and streamd capture API suffice;
replacing streamd alone does not remove a cursor already baked into a snapshot.
Framebuffer fallback captures still include any cursor drawn into video memory.

This prevents capture of horizontal splits between old/new row bands and
avoids reading slow video memory for the normal 640x480 stream. Capture at
other sizes, or without a cooperating presenter, uses a generation check on
the framebuffer. An overlapping presentation returns FB_CAPTURE_BUSY;
streamd discards that RGB buffer and retries without transmitting a JPEG
from it. Old kernels are detected by Memento's capability probe and keep
their existing blit behaviour.

The kernel exposes a monotonically increasing snapshot ID and presentation
timestamp under the same lease as the pixels. A repeated ID returns
FB_CAPTURE_UNCHANGED without copying RGB data. streamd reuses its JPEG without
scanning RGB in that case. New snapshots still use an exact pixel comparison
before encoding; old kernels ignore the optional metadata extension and
continue to use that comparison. The extension opts in with bit 63 of the
capture syscall's dimensions argument, so legacy callers with a live RCX
register remain compatible.

Every five seconds STREAMD.LOG reports:

- `fps`: fully acknowledged multipart frames, including repeated pictures.
- `source_fps`: distinct delivered snapshot IDs; `unknown` on older kernels
  or framebuffer fallback. It can be lower than the presenter's frame rate.
- `encoded_fps` and `cached`: newly encoded versus repeated delivered JPEGs.
- Average `capture_ms`, `jpeg_ms`, `send_ms` and JPEG `bytes`.
- `work_max_ms`: maximum capture/encoding time among delivered frames.
- `age_max_ms`: maximum snapshot-to-ACK age. It grows on an unchanged desktop
  because the presentation timestamp remains unchanged.
- `replaced`, `skipped`, `retries` and `capture_busy`.

The metadata optimization requires rebuilding and booting the graphics kernel
and streamd. `make r2-capture-test` builds `build/r2_capture_test.elf`; run it
as `bg captest m` after installing it as `captest.elf`. It checks identity,
timestamps, unchanged capture without an RGB copy, and legacy syscall ABI
compatibility through QEMU debugcon. The writer/reader modes below still test
complete-picture capture during split-band presentations.
`bg captest f` runs a faster alternating-colour presenter for MJPEG throughput
checks and prints STREAMD.LOG to debugcon when it finishes.

The tearing fix requires the rebuilt **kernel and Memento**, plus streamd.
Regenerate the boot image and reboot; swapping streamd alone is insufficient.
`make capture-test` runs the kernel's snapshot-lease and generation tests.
`tests/capture/r2_capture_test.c` is the QEMU regression program: it presents
top and bottom halves 30 ms apart and checks that accepted captures never
mix their colours. The prior kernel fails; the patched graphics kernel
passes, including a check against capture starvation.
