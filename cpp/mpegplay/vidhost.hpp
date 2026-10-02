//
//  vidhost.hpp --- the block of memory a window and mpegplay share.
//
//  mpegplay does not draw.  Whoever starts it --- memento's Video window ---
//  allocates one of these on r2's user heap (0xC00_000 up, mapped the same way
//  in every process), and passes its address:
//
//      mpegplay.elf --host 0xC12340 /mnt/tar/video/clip.mpg
//
//  The player decodes into it, the window copies the pictures onto the screen
//  and sends the keys back as a few flags.  The same shape as the Editor's
//  block for Turbo C++ (memento-hello/windows/editor_window.cpp), and this
//  header is included from both sides so that the layout cannot drift.
//
//  Pictures are palette indices, one byte a pixel, in the colours the r2
//  backend of Memento shows everything in: what the window gets is what it
//  can put on the screen as it is.  That is the 16 EGA colours on the VGA,
//  and 256 when Memento runs on the graphics kernel's framebuffer --- the same
//  16, then a 6x6x6 cube at 16..231 and greys at 232..255 (R2_Palette).  The
//  host says which in `colours` before it starts the player.
//
//  Two picture buffers, so that the one on the screen is never the one being
//  written.  The player writes the other one and then publishes it by setting
//  `front`.  The window reads like this, and the player never writes a buffer
//  the window has named in `reading`:
//
//      f = front;  reading = f;  if (front != f) try again;
//      copy pixels[f];  reading = NONE;
//
//  (If the player flipped between the first two steps it may already be
//  writing into f; it can only have done that after flipping, which the check
//  catches.  Once reading = f is in place with front still f, the player sees
//  it before it gets to f again.)
//
//  Both sides beat: a player that hears nothing from its host for ten seconds
//  leaves, and a host looks at the task table to see whether its player died.
//
#pragma once

#include <r2/types.hpp>

struct VidHostBlock
{
    static const uint32_t Magic = 0x44495650; // "PVID"
    static const uint32_t Version = 7;

    //  The largest picture: a quarter of the 640x400 screen, and what an MPEG-1
    //  file for this should be made at.  Bigger ones are scaled down to fit.
    static const int MaxW = 320;
    static const int MaxH = 240;
    //  The largest picture the window may ask for (wantW, wantH): with 256
    //  colours the player scales the film to the size it is shown at and
    //  dithers it there, rather than the window enlarging a dithered 320x240
    //  and the dots with it.  A 1920x1080 screen is a 960x540 desktop.
    static const int OutMaxW = 960;
    static const int OutMaxH = 600;
    static const uint8_t NONE = 0xFF;

    enum State : uint8_t
    {
        LOADING = 0,
        PLAYING,
        PAUSED,
        ENDED,
        FAILED, // `message` says why
    };

    uint32_t magic;
    uint32_t version;

    // ── Written by the player ────────────────────────────────────────────────
    volatile uint32_t playerBeat;
    volatile uint32_t frame;   // bumped each time a new picture is published
    volatile uint8_t front;    // the buffer holding it
    volatile uint8_t state;    // State
    volatile uint8_t exited;   // the player has let go of the block
    uint8_t pad0;
    volatile uint16_t width;   // the film at up to MaxW x MaxH: its shape, and the size used when the window asks none
    volatile uint16_t height;
    volatile uint32_t rateMilli;    // the stream's frames per 1000 s (25 fps: 25000)
    volatile uint32_t positionMs;   // of the picture published last
    volatile uint32_t durationMs;   // 0 when the file does not say
    volatile uint32_t dropped;      // pictures decoded but not shown, to keep time
    char message[64];

    // ── Written by the host ──────────────────────────────────────────────────
    volatile uint32_t hostBeat;
    volatile uint8_t reading; // the buffer being copied, or NONE
    volatile uint8_t paused;
    volatile uint8_t quit;
    volatile uint8_t volume;   // the sound's, in percent: 0..200, 100 as decoded
    volatile uint32_t restart; // bumped to play from the start again
    uint16_t colours;          // 16 or 256, set before the player starts
    uint16_t pad2;

    // ── Sound (written by the player) ───────────────────────────────────────
    enum Sound : uint8_t
    {
        SOUND_NONE = 0,   // the film has no MP2 audio
        SOUND_ON = 1,     // playing it (HD Audio, syscall 0x3f)
        SOUND_NO_DEVICE = 2,
    };
    volatile uint8_t sound;
    uint8_t pad3[7];

    // ── A stream (HLS), when the player is started with --stream ─────────────
    //  The window fetches the playlist and its MPEG-TS segments and writes
    //  the bytes into a ring of its own on the user heap, at streamRing; the
    //  player reads them.  Counts only ever grow: the window writes at
    //  streamHead % streamSize and then moves streamHead on, the player reads
    //  at streamTail and moves that.  streamEnded: nothing more is coming.
    uint64_t streamRing;
    uint32_t streamSize;
    uint32_t pad4;
    volatile uint64_t streamHead;
    volatile uint64_t streamTail;
    volatile uint8_t streamEnded;
    uint8_t pad5[7];

    // ── For the window's diagnostics line (I), written by the player ─────────
    //  Maxima since the window last read them; it sets them back to 0.
    volatile uint32_t waitMaxMs;   // longest wait for stream bytes that were not there
    volatile uint32_t decodeMaxMs; // longest decode of one picture, waits left out
    volatile uint32_t lateMaxMs;   // furthest behind a skipped picture was
    volatile uint32_t waits;       // waits for stream bytes over 50 ms, since the start

    // ── The picture's size ───────────────────────────────────────────────────
    //  Written by the window: the size it shows the picture at, in pixels,
    //  its shape already the film's (width : height); 0 for the player's own
    //  (width, height above).  Read by the player for every picture.
    volatile uint16_t wantW;
    volatile uint16_t wantH;
    //  Written by the player: the size of the picture in each buffer.
    volatile uint16_t bufW[2];
    volatile uint16_t bufH[2];

    // ── The whole screen (F), on the graphics kernel only ────────────────────
    //  The window sets `fullscreen` once Memento has stopped sending frames
    //  (R2_HoldScreen); the player then draws each picture across the whole
    //  framebuffer itself, in true colour (syscall 0x17), instead of into
    //  pixels[], and says so in `fsShowing`.  The window clears
    //  `fullscreen` to leave, and gives Memento the screen back only once
    //  `fsShowing` is 0 again: the player draws only while it is 1, and
    //  clears it between pictures, so nothing of its lands after Memento's.
    volatile uint8_t fullscreen;  // written by the window
    volatile uint8_t fsShowing;   // written by the player
    volatile uint8_t fsScaled;    // written by the player: the picture is enlarged by the kernel
    volatile uint8_t fsRefused;   // written by the player: it cannot (no memory, not 32 bits a pixel)
    volatile uint16_t fsW, fsH;   // written by the player: the picture's size on the screen, for the window's line

    uint8_t pixels[2][OutMaxW * OutMaxH];
};
