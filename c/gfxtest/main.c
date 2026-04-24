#include "syscall.h"
#include "types.h"

/*
 *  gfxtest — VGA graphics driver test for r2 kernel
 *
 *  Exercises syscall 0x14 (MAP_VRAM) and 0x15 (SET_VIDEO_MODE).
 *  Switches to VGA mode 13h (320×200, 256-color), draws a test pattern,
 *  holds for ~3 seconds, then restores text mode and exits.
 *
 *  krusty@vxn.dev / Apr 23, 2026
 */

#define W 320
#define H 200

/* DAC palette helpers */

/*
 * Write a single byte to an I/O port using the kernel's PORT_WRITE ABI.
 * write_port() now correctly passes &port and &value as required by the
 * kernel (arg1 = ptr-to-u16 port, arg2 = ptr-to-u32 value).
 */
static void dac_set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    write_port(0x3C8, idx); /* set DAC write index */
    write_port(0x3C9, r);
    write_port(0x3C9, g);
    write_port(0x3C9, b);
}

/*
 * Load a 256-entry rainbow palette.
 *   0        — black
 *   1..254   — red → green → blue → red cycle (6-bit DAC, values 0–63)
 *   255      — white (used for the border frame)
 */
static void load_palette(void) {
    dac_set(0, 0, 0, 0);

    for (int i = 1; i < 255; i++) {
        /* Map i=1..254 linearly onto three 64-step colour segments. */
        int t = (i - 1) * 192 / 253; /* 0..191 */
        uint8_t r, g, b;

        if (t < 64) { /* red → green */
            r = (uint8_t)(63 - t);
            g = (uint8_t)t;
            b = 0;
        } else if (t < 128) { /* green → blue */
            t -= 64;
            r = 0;
            g = (uint8_t)(63 - t);
            b = (uint8_t)t;
        } else { /* blue → red */
            t -= 128;
            r = (uint8_t)t;
            g = 0;
            b = (uint8_t)(63 - t);
        }

        dac_set(i, r, g, b);
    }

    dac_set(255, 63, 63, 63); /* white for border */
}

/* Test pattern */

/*
 * Draw to raw VRAM (linear byte array; mode 13h = 320 bytes per row).
 *
 * Layout:
 *   rows  0-11   — 16 solid colour sample bars (20 px wide each)
 *   rows 12-87   — XOR plasma  pixel = (x ^ y) & 0xFF
 *   rows 88-99   — full palette sweep (all 256 colours left→right)
 *   rows 100-198 — diagonal gradient  pixel = ((x + y) / 2) & 0xFF
 *   border       — 1-pixel white frame around the whole screen
 */
static void draw(uint8_t *vram) {
    int x, y;

    /* Colour sample bars (top strip) */
    for (y = 0; y < 12; y++) {
        for (x = 0; x < W; x++) {
            /* 16 bars × 20 px; map bar index evenly into the rainbow. */
            int bar = x / 20;
            vram[y * W + x] = (uint8_t)(bar * 254 / 15 + 1);
        }
    }

    /* XOR plasma */
    for (y = 12; y < 88; y++) {
        for (x = 0; x < W; x++) {
            vram[y * W + x] = (uint8_t)((x ^ y) & 0xFF);
        }
    }

    /* Full palette sweep — shows all 256 colours in one band */
    for (y = 88; y < 100; y++) {
        for (x = 0; x < W; x++) {
            vram[y * W + x] = (uint8_t)(x * 255 / (W - 1));
        }
    }

    /* Diagonal gradient */
    for (y = 100; y < H - 1; y++) {
        for (x = 0; x < W; x++) {
            vram[y * W + x] = (uint8_t)(((x + y) >> 1) & 0xFF);
        }
    }

    /* White border frame (palette index 255) */
    for (x = 0; x < W; x++) {
        vram[0 * W + x] = 255;
        vram[(H - 1) * W + x] = 255;
    }
    for (y = 0; y < H; y++) {
        vram[y * W + 0] = 255;
        vram[y * W + (W - 1)] = 255;
    }
}

int main(void) {
    /* Map VGA graphics RAM into our address space */
    uint64_t vram_base = map_vram();
    if (vram_base == 0) {
        print((const uint8_t *)"gfxtest: MAP_VRAM failed\n");
        exit(0xff, 1);
    }

    uint8_t *vram = (uint8_t *)vram_base;

    /* Switch hardware to mode 13h FIRST.
     *
     * In text mode GC[06] maps the VGA aperture to 0xB8000, so any CPU write
     * to physical 0xA0000 is silently dropped by the hardware (and by QEMU's
     * vga_mem_write filter).
     *
     * The mode switch sets GC[06]=0x05 which opens the
     * 0xA0000–0xAFFFF window, making the VRAM writable from userland. */
    if (set_video_mode(0x13) != 0) {
        print((const uint8_t *)"gfxtest: SET_VIDEO_MODE 0x13 failed\n");
        exit(0xff, 2);
    }

    /* Load the DAC palette.
     *
     * The DAC is independent of the display mode and the values survive across
     * mode switches, so this can be done any time after the VRAM aperture is
     * open. */
    load_palette();

    /* Draw the test pattern.
     * The aperture is now open so writes go straight into VGA VRAM and become
     * visible on the next display refresh. */
    draw(vram);

    /* Hold display for ~3 seconds (busy-wait; QEMU ~100-300 MIPS) */
    for (volatile long i = 0; i < 500000000L; i++)
        ;

    /* Restore VGA text mode so the shell works again */
    set_video_mode(0x03);

    print((const uint8_t *)"gfxtest: OK\n");

    exit(0xff, 0);
}
