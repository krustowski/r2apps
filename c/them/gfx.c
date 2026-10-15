#include "gfx.h"
#include "io.h"
#include "mmu.h"
#include "printf.h"
#include "syscall.h"
#include "vga.h"
#include "winhost.h"

/*
 *  gfx.c
 *
 *  Puts the emulated screen on the r2 display.
 *
 *  Two paths, chosen once at the first graphics frame. When the bootloader
 *  handed the kernel a linear VESA framebuffer the frame is converted to
 *  32bpp at the size the guest is running and blitted scaled to the whole
 *  screen -- a 640x480 guest stays 640x480 until the framebuffer stretches
 *  it, which is what keeps a program's text readable. When it did not --
 *  the common case of a plain text-mode boot -- the emulator borrows the VGA
 *  hardware itself: mode 13h, the guest palette loaded into the DAC and the
 *  frame copied straight into video memory. That one is 320x200 and cannot
 *  be talked out of it, so a wider mode is still sampled down there.
 *
 *  A third, when the emulator runs in a Memento window (winhost.h): no
 *  hardware and no console at all, every picture -- text mode included -- goes
 *  into the window's block instead.
 *
 *  krusty@vxn.dev / Nov 24, 2025
 */

extern uint8_t debug;

#define FRAME_INTERVAL_MS 20
#define VGA_W 320
#define VGA_H 200

/*
 *  The most of the run full-screen presentation may take, as a percentage.
 *
 *  What a frame costs is not the emulator's to choose: the kernel's scaled
 *  blit writes the whole framebuffer whatever size the guest is, so on a
 *  1024x768 screen one frame is three quarters of a million pixels however
 *  little the guest changed. Asking for a frame every FRAME_INTERVAL_MS
 *  regardless is then a promise that cannot be kept -- a 40 ms frame every
 *  20 ms leaves the guest a tenth of the processor, which does not look like
 *  a slow display, it looks like a hung emulator.
 *
 *  So the interval is earned rather than fixed: after each frame the next one
 *  is held off for as long as that frame cost, times what is left of the
 *  budget. A cheap backend still runs at FRAME_INTERVAL_MS, an expensive one
 *  quietly drops its rate, and the guest keeps the rest either way. Window
 *  frames use the fixed interval: their conversion is ordinary memory work,
 *  and elapsed time may include another process running while we are preempted.
 */
#define FRAME_BUDGET_PCT 25

/* The host's 640x480 16-colour mode: 80 bytes a row in each of four planes. */
#define VGA12_PITCH 80
#define VGA12_H 480

#define BACKEND_UNSET 0
#define BACKEND_VESA 1
#define BACKEND_VGA 2
#define BACKEND_NONE 3
#define BACKEND_WINDOW 4

/* The kernel's console glyphs: 8 pixels wide, <font_rows> rows each. */
static uint8_t font[256 * 16];
static uint8_t font_rows = 0;

/*
 *  The frame as the kernel takes it: one 32bpp pixel each, at whatever size
 *  the guest is running, up to VGA_MAX_W x VGA_MAX_H.
 *
 *  It lives in the program image rather than the heap because r2 only
 *  accepts syscall pointers into the loaded image, and the image shares one
 *  2 MiB mapping with everything else the emulator has -- which is what the
 *  smaller private stack in this program's crt0 makes room for.
 */
static uint32_t rgb[VGA_MAX_W * VGA_MAX_H];

static uint8_t backend = BACKEND_UNSET;
/* The mode the host adapter is actually in, so it is only ever reprogrammed
 * when the guest needs something it is not already showing. */
static uint8_t host_mode = 0;
static uint8_t *vram = (uint8_t *)0;
static uint8_t pal_shadow[768];
static uint8_t pal_loaded = 0;
static uint64_t last_frame_ms = 0;
/* When the next frame may be presented, and what the last one cost.  Kept
 * across mode changes: the cost is a property of the backend, not the guest. */
static uint64_t next_frame_ms = 0;
static uint64_t frame_cost_ms = 0;
static uint8_t dirty = 1;
static uint8_t graphics_shown = 0;

/* What the last presented frame cost, and the rate it works out at.  For the
 * heartbeat, which is where a display eating the run becomes visible. */
uint64_t gfx_frame_cost(void) { return frame_cost_ms; }

void gfx_mark_dirty(void) { dirty = 1; }

/*
 *  Whether the guest is better served by the host's 640x480 planar mode.
 *
 *  Only a 16-colour guest can go out this way -- the planes line up and the
 *  colours are the sixteen the attribute controller names -- and only one too
 *  big for 320x200 has anything to gain from it.
 */
static uint8_t wants_host_planar(void) {
    uint16_t w = 0, h = 0;

    if (!vga_planar())
        return 0;

    vga_geometry((uint16_t *)0, &w, &h);

    return (uint8_t)(w > 320 || h > 200);
}

static void choose_backend(void) {
    FBInfo_T fb = {0};

    if (backend != BACKEND_UNSET)
        return;

    /* In a window the screen is Memento's: nothing here may touch it. */
    if (winhost) {
        backend = BACKEND_WINDOW;
        return;
    }

    /* A real linear framebuffer is at least 320 pixels wide and true colour;
     * a text-mode boot reports the 80x25 character buffer instead. */
    if (get_fb_info(&fb) == 0 && fb.bpp >= 24 && fb.width >= VGA_W && fb.height >= VGA_H) {
        backend = BACKEND_VESA;
        return;
    }

    /*
     *  Start in the mode the guest is already running, not always 13h.
     *
     *  Committing to 13h here and letting the first frame switch to 12h means
     *  the display changes resolution a moment after it comes up, and a host
     *  that sized its window for the first mode -- which is what a window on
     *  an emulator does -- is then eighty lines short of the second for the
     *  rest of the run. There is nothing to learn from the smaller mode that
     *  the guest has not already said.
     */
    vram = (uint8_t *)map_vram();
    if (vram) {
        uint8_t want = wants_host_planar() ? 0x12 : 0x13;

        if (set_video_mode(want) == 0) {
            backend = BACKEND_VGA;
            host_mode = want;
            return;
        }

        /* A host that will not give us that one may still give us 13h. */
        if (want != 0x13 && set_video_mode(0x13) == 0) {
            backend = BACKEND_VGA;
            host_mode = 0x13;
            return;
        }
    }

    backend = BACKEND_NONE;
}

void gfx_mode_changed(uint8_t mode) {
    dirty = 1;
    pal_loaded = 0;

    if (mode == 0x03 && graphics_shown) {
        graphics_shown = 0;

        if (backend == BACKEND_VGA) {
            set_video_mode(0x03);
            host_mode = 0x03;
        }
    }
}

/*
 *  Hand the display back on the way out.
 *
 *  Only the mode is restored; clearing the console would mean taking the
 *  kernel's screen lock, which a syscall cannot safely wait on.
 */
void gfx_shutdown(void) {
    if (!graphics_shown)
        return;

    graphics_shown = 0;

    if (backend == BACKEND_VGA) {
        set_video_mode(0x03);
        host_mode = 0x03;
    }
}

/* How many times the palette has been pushed to the hardware, and what it has
 * cost. Each reload is 1024 port syscalls, so a guest that fades or cycles its
 * palette pays for one on every frame. */
static uint32_t dac_loads = 0;
static uint64_t dac_ms = 0;

uint32_t gfx_dac_loads(void) { return dac_loads; }
uint64_t gfx_dac_ms(void) { return dac_ms; }

/* Push the guest palette into the hardware DAC, but only when it changed:
 * each entry costs four port syscalls. */
static void load_dac(const uint8_t *pal) {
    uint8_t same = pal_loaded;

    for (uint16_t i = 0; i < 768 && same; i++)
        if (pal_shadow[i] != pal[i])
            same = 0;

    if (same)
        return;

    for (uint16_t i = 0; i < 768; i++)
        pal_shadow[i] = pal[i];
    pal_loaded = 1;

    {
        uint64_t t0 = get_ticks();

        for (uint16_t i = 0; i < 256; i++) {
            write_port(0x3C8, (uint32_t)i);
            write_port(0x3C9, (uint32_t)(pal[i * 3 + 0] >> 2));
            write_port(0x3C9, (uint32_t)(pal[i * 3 + 1] >> 2));
            write_port(0x3C9, (uint32_t)(pal[i * 3 + 2] >> 2));
        }

        dac_ms += get_ticks() - t0;
        dac_loads++;
    }
}

/*
 *  Paint a palette-indexed picture into the VESA framebuffer.
 *
 *  Used for the emulator's own messages, which are drawn at 320x200 with the
 *  kernel font.  The kernel's mode 13h renderer would do this too, but it
 *  copies pixel for pixel into the corner of the screen, which on a 1024x768
 *  framebuffer leaves the text a quarter of the size it could be; the scaled
 *  blit stretches it over the whole display instead.
 */
static void present_vesa(const uint8_t *frame, const uint8_t *pal, uint16_t w, uint16_t h) {
    if ((uint32_t)w * h > (uint32_t)VGA_MAX_W * VGA_MAX_H)
        return;

    for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
        uint16_t idx = frame[i];

        rgb[i] = ((uint32_t)pal[idx * 3 + 0] << 16) | ((uint32_t)pal[idx * 3 + 1] << 8) | (uint32_t)pal[idx * 3 + 2];
    }

    blit_buffer_scaled(rgb, w, h);
}

/*
 *  Paint the guest frame at its own size.
 *
 *  The kernel's scaled blit takes any width and height and stretches it over
 *  the whole framebuffer, so nothing has to be given up on the way out: a
 *  640x480 mode arrives as 640x480 and its 8x8 text is still 8x8.  The buffer
 *  it is handed has to live in the program image, r2 rejecting syscall
 *  arguments that point into the heap.
 */
static void present_vesa_native(Memory_T *mem) {
    uint16_t w = 0, h = 0;

    vga_frame_rgb(mem, rgb, VGA_MAX_W, VGA_MAX_H, &w, &h);

    if (!w || !h)
        return;

    blit_buffer_scaled(rgb, w, h);
}

/*
 *  Put the adapter in <mode> if it is not already there.
 *
 *  A mode set loads the hardware's own palette, so whatever was pushed into
 *  the DAC before it is gone and has to go again.
 */
static void host_vga_enter(uint8_t mode) {
    if (host_mode == mode)
        return;

    if (set_video_mode(mode) != 0)
        return;

    host_mode = mode;
    pal_loaded = 0;

    if (mode == 0x12) {
        uint32_t status;
        /* The host receives sixteen resolved RGB colours in DAC slots 0-15.
         * BIOS mode 12h normally maps brown/bright pixels to slots 20/56-63;
         * make the host AC map identity so those pixels use our loaded DAC. */
        for (uint8_t i = 0; i < 16; i++) {
            (void)read_port(0x3DA, &status);
            write_port(0x3C0, i);
            write_port(0x3C0, i);
        }
        (void)read_port(0x3DA, &status);
        write_port(0x3C0, 0x20); /* enable display */
    }
}

/*
 *  Show a 16-colour guest in the host's own 640x480 mode, plane for plane.
 *
 *  Mode 13h is 320x200 and a 640x480 guest sampled into it loses every other
 *  column and more than half its rows, which is readable for a picture and
 *  useless for anything with writing on it -- a program's own 8x8 text comes
 *  out four pixels wide and three tall. Nothing has to be given up: the guest
 *  is already in four planes of exactly the shape mode 12h wants, so each one
 *  goes out whole through the sequencer's map mask and the picture arrives at
 *  the size it was drawn.
 */
static void present_vga_planar(void) {
    uint8_t pal16[48];

    host_vga_enter(0x12);
    if (host_mode != 0x12)
        return;

    /* Sixteen entries rather than the DAC's full 256: that is all a planar
     * mode shows, and each one costs four port syscalls. */
    vga_palette_16(pal16);

    {
        uint8_t same = pal_loaded;

        for (uint8_t i = 0; i < 48 && same; i++)
            if (pal_shadow[i] != pal16[i])
                same = 0;

        if (!same) {
            uint64_t t0 = get_ticks();

            for (uint8_t i = 0; i < 48; i++)
                pal_shadow[i] = pal16[i];
            pal_loaded = 1;

            for (uint8_t i = 0; i < 16; i++) {
                write_port(0x3C8, (uint32_t)i);
                write_port(0x3C9, (uint32_t)(pal16[i * 3 + 0] >> 2));
                write_port(0x3C9, (uint32_t)(pal16[i * 3 + 1] >> 2));
                write_port(0x3C9, (uint32_t)(pal16[i * 3 + 2] >> 2));
            }

            dac_ms += get_ticks() - t0;
            dac_loads++;
        }
    }

    for (uint8_t p = 0; p < 4; p++) {
        write_port(0x3C4, 0x02);                /* sequencer: map mask */
        write_port(0x3C5, (uint32_t)(1u << p)); /* this plane only */

        vga_plane_rows(p, vram, VGA12_PITCH, VGA12_H);
    }

    write_port(0x3C4, 0x02);
    write_port(0x3C5, 0x0F); /* leave all four writable, as a mode set does */
}

static void present_vga(const uint8_t *frame, const uint8_t *pal) {
    /*
     *  Put the hardware back into mode 13h if the guest has been through text
     *  mode since the last picture.
     *
     *  A guest that asks for text mode has the adapter given back to it, which
     *  is right -- but the adapter does not come back on its own when the next
     *  graphics mode arrives, and choose_backend() only ever runs once.  A
     *  program that looks at the modes on offer, returns to text to think about
     *  it and then starts -- which is what a game with a setup screen does --
     *  was painting into 0xA0000 from then on with the CRT still showing the
     *  text buffer: running, drawing, and completely invisible.
     */
    host_vga_enter(0x13);
    if (host_mode != 0x13)
        return;

    load_dac(pal);

    for (uint32_t i = 0; i < (uint32_t)VGA_W * VGA_H; i++)
        vram[i] = frame[i];
}

/*
 *  Text mode goes to the kernel console, since 80 columns do not fit the
 *  320x200 the graphics modes are presented at.  The whole screen is built
 *  first and handed over in one call: every console call means waiting on
 *  the kernel's screen lock, so the fewer of them, the better.
 */
static uint8_t text_screen[25 * 81 + 1];

/*
 *  Draw a line of the emulator's own text on the screen.
 *
 *  Neither the console nor the serial line can be used for this: both make
 *  a syscall wait on something -- a screen lock, a UART that may not be
 *  there -- that cannot be waited on safely with interrupts disabled, and a
 *  program that hangs there takes the machine with it.  Drawing into the
 *  frame costs nothing but a memcpy of the kernel's font.
 */
void gfx_message(const uint8_t *text) {
    uint8_t *frame = vga_frame_buffer();
    uint16_t w = vga_frame_width();
    uint16_t h = vga_frame_height();
    uint16_t col = 0, row = 0;
    uint64_t until;

    if (!font_rows) {
        int64_t rows = get_kernel_font(font, sizeof(font));

        font_rows = (rows > 0 && rows <= 16) ? (uint8_t)rows : 0;
    }

    if (!frame || !font_rows)
        return;

    choose_backend();
    if (backend == BACKEND_NONE)
        return;

    /* A blue field, so the message is unmistakably the emulator's and not
     * a guest that painted nothing. */
    for (uint32_t i = 0; i < (uint32_t)w * h; i++)
        frame[i] = 1;

    for (uint16_t i = 0; text[i]; i++) {
        uint8_t ch = text[i];
        const uint8_t *glyph = &font[(uint32_t)ch * font_rows];

        if (ch == '\n' || col + 8 > w) {
            col = 0;
            row = (uint16_t)(row + font_rows);
            if (ch == '\n')
                continue;
        }
        if (row + font_rows > h)
            break;

        for (uint8_t y = 0; y < font_rows; y++)
            for (uint8_t x = 0; x < 8; x++)
                if ((glyph[y] >> (7 - x)) & 1)
                    frame[(uint32_t)(row + y) * w + col + x] = 15; /* white */

        col = (uint16_t)(col + 8);
    }

    if (backend == BACKEND_WINDOW) {
        /* In the window, held the same way, with its keys. */
        uint64_t shown;

        winhost_publish_indexed(frame, w, h);
        shown = get_ticks();
        while (winhost_key())
            ; /* what was typed before it was shown does not answer it */

        while (get_ticks() - shown < 10000 && !winhost_quit()) {
            uint8_t sc = winhost_key();

            winhost_beat();
            if (sc && !(sc & 0x80) && sc != 0xE0 && get_ticks() - shown > 500)
                break;
            sleep_ms(10);
        }

        dirty = 1;
        return;
    }

    if (backend == BACKEND_VESA)
        present_vesa(frame, vga_palette(), w, h);
    else
        present_vga(frame, vga_palette());

    graphics_shown = 1;

    /*
     *  Hold it until the reader has seen it: any key, or ten seconds.
     *
     *  The keystroke that started the emulator is still in the keyboard
     *  pipe at this point, so the pending input is dropped first and the
     *  message held briefly -- otherwise it would dismiss itself with the
     *  very key that asked for it.
     */
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t drain[16];

        for (uint8_t j = 0; j < 16; j++)
            drain[j] = 0;

        pipe_read(drain);
    }

    until = get_ticks() + 1500;
    while (get_ticks() < until)
        ;

    until = get_ticks() + 10000;
    while (get_ticks() < until) {
        uint8_t drain[16];
        uint8_t pressed = 0;

        for (uint8_t j = 0; j < 16; j++)
            drain[j] = 0;

        pipe_read(drain);

        for (uint8_t j = 0; j < 16 && drain[j]; j++)
            if (!(drain[j] & 0x80)) /* a press, not a release */
                pressed = 1;

        if (pressed)
            break;
    }

    gfx_shutdown();
}

/*
 *  Record the frame's elapsed time. Window starts follow the fixed cadence;
 *  full-screen frames earn the interval that keeps their blits in budget.
 */
static void charge_for_frame(uint64_t started) {
    uint64_t done = get_ticks();

    frame_cost_ms = done - started;

    if (backend == BACKEND_WINDOW) {
        /* Spotify (or any other process) may run in the middle of conversion.
         * That elapsed time is not our CPU cost. Multiplying it by three
         * adds a second delay after the scheduler already held us back. */
        next_frame_ms = started + FRAME_INTERVAL_MS;
        return;
    }

    next_frame_ms = done + frame_cost_ms * (100 - FRAME_BUDGET_PCT) / FRAME_BUDGET_PCT;
}

static void present_text(Memory_T *mem) {
    uint16_t n = 0;

    for (uint8_t row = 0; row < 25; row++) {
        for (uint8_t col = 0; col < 80; col++) {
            uint8_t ch = mem->bytes[0xB8000 + 2 * ((uint32_t)row * 80 + col)];

            text_screen[n++] = (ch >= 0x20 && ch < 0x7F) ? ch : ' ';
        }

        text_screen[n++] = '\n';
    }

    text_screen[n] = '\0';

    clear_screen();
    print(text_screen);
}

void gfx_frame(Memory_T *mem) {
    uint64_t now = hw_millis();
    uint64_t started;

    if (!dirty && !vga_dirty())
        return;

    if (now - last_frame_ms < FRAME_INTERVAL_MS)
        return;

    /*
     *  Past the cheap gate, so ask what the time really is.
     *
     *  hw_millis() is a value the main loop refreshes now and then, which is
     *  all a rate limit needs but is useless to measure with: it does not move
     *  while the frame it is timing is being drawn, so every frame would look
     *  free and the budget below would never bite.
     */
    started = get_ticks();

    if (started < next_frame_ms)
        return;

    last_frame_ms = now;
    dirty = 0;
    vga_clear_dirty();

    choose_backend();
    if (backend == BACKEND_WINDOW) {
        /* Text mode too: the window has no console to put it on. */
        if (!winhost_publish(mem))
            dirty = 1; /* the window is still reading; the next pass tries again */
        else
            charge_for_frame(started);
        return;
    }

    if (vga_graphics()) {
        choose_backend();
        if (backend == BACKEND_NONE)
            return;

        if (backend == BACKEND_VESA) {
            present_vesa_native(mem);
        } else if (wants_host_planar()) {
            present_vga_planar();
        } else {
            uint16_t w = 0, h = 0;
            const uint8_t *frame = vga_frame(mem, &w, &h);

            if (!frame || !w || !h)
                return;

            present_vga(frame, vga_palette());
        }

        charge_for_frame(started);
        graphics_shown = 1;
        return;
    }

    if (graphics_shown || mem->bytes[0xB8000])
        present_text(mem);
}

/*
 *  The guest has ended: show what it left on the screen.
 *
 *  The rate limit above is right while a guest runs and wrong as it leaves.
 *  A program that cannot run says so on the way out -- DOS/4GW, finding no
 *  386, writes its error and exits in the same millisecond -- and that last
 *  picture was being held off until a next pass that never came.  The window
 *  then showed an exit code over an empty screen, which looks exactly like a
 *  program that never started.
 */
void gfx_flush(Memory_T *mem) {
    for (uint8_t tries = 0; tries < 50; tries++) {
        last_frame_ms = 0;
        next_frame_ms = 0;
        if (tries == 0)
            dirty = 1;

        gfx_frame(mem);
        if (!dirty)
            return;

        /* The window is still copying the previous picture. */
        uint64_t until = get_ticks() + 10;
        while (get_ticks() < until)
            ;
    }
}
