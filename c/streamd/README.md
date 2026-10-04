# streamd

Streams r2's displayed VESA framebuffer as 640x480 MJPEG at a target of 30 FPS,
including Memento windows. Open `http://10.3.4.2:8080/stream` in a browser
or an OBS Browser Source. The server handles one stream client at a time.

Build with `make build`. Run the host TCP regression tests with `make test`.
The tests simulate the receiver and clock; they do not boot r2 or run theM.

The sender keeps each HTTP/JPEG buffer until cumulatively acknowledged.
It pipelines up to 14400 bytes within the peer's advertised receive window,
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
frame is sent as one contiguous write (header, JPEG, trailing CRLF), avoiding
two extra acknowledgement waits for tiny writes. TCP data segments use PSH,
and the bounded transmit pipeline holds up to 14.4 KiB within the receiver's
window. Frame deadlines use 33/34 ms intervals and skip missed deadlines;
frames are captured on demand, so slow encoding/networking cannot create a
queue of old pictures.

30 FPS is a target, not a measured guarantee. Capture, JPEG encoding and
sending must fit within roughly 33 ms on average; inspect the first-frame
capture_ms, jpeg_ms and send_ms in STREAMD.LOG to identify a bottleneck.
JPEG quality 1 reduces bandwidth if quality 2 cannot keep up. Retain the
kernel floating-point context fix when running beside theM.

## Complete pictures during movement

Memento now brackets all dirty row bands as one presentation. At its end,
the kernel samples the full composed indexed surface into a 640x480 RGB24
snapshot in shared RAM. Two snapshot banks let streamd read the previous
complete picture while the next one is drawn. A bank leased by a capturer
cannot be overwritten; leases and presentation ownership are released if a
process exits or crashes. The buffers are allocated lazily and retain about
1.8 MiB for the kernel's lifetime.

This prevents capture of horizontal splits between old/new row bands and
avoids reading slow video memory for the normal 640x480 stream. Capture at
other sizes, or without a cooperating presenter, uses a generation check on
the framebuffer. An overlapping presentation returns FB_CAPTURE_BUSY;
streamd discards that RGB buffer and retries without transmitting a JPEG
from it. Old kernels are detected by Memento's capability probe and keep
their existing blit behaviour.

streamd keeps two RGB buffers and compares them exactly. If the picture is
unchanged it reuses the existing JPEG, saving encoder CPU without relying
on a potentially colliding hash. Every five seconds a single STREAMD.LOG
line reports actual transmitted FPS, average capture/jpeg/send times and
JPEG size, cached JPEG count, TCP retries and busy captures. Cached counts
are repeated complete pictures, not new desktop presentations.

The tearing fix requires the rebuilt **kernel and Memento**, plus streamd.
Regenerate the boot image and reboot; swapping streamd alone is insufficient.
`make capture-test` runs the kernel's snapshot-lease and generation tests.
`tests/capture/r2_capture_test.c` is the QEMU regression program: it presents
top and bottom halves 30 ms apart and checks that accepted captures never
mix their colours. The prior kernel fails; the patched graphics kernel
passes, including a check against capture starvation.
