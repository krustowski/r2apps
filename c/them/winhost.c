#include "winhost.h"
#include "gfx.h"
#include "io.h"
#include "printf.h"
#include "syscall.h"
#include "types.h"
#include "vga.h"

/*
 *  winhost.c
 *
 *  The emulator's side of running in a Memento window: see winhost.h for the
 *  block and the rules for using it.
 */

ThemWinBlock *winhost = (ThemWinBlock *)0;

/* When the window was last heard from, and whether it has gone quiet. */
static uint32_t heard_beat = 0;
static uint64_t heard_at = 0;
static uint64_t beat_at = 0;
static uint8_t host_gone = 0;

uint8_t winhost_attach(const uint8_t *arg) {
    uint64_t addr = 0;
    uint8_t i = 0;

    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X'))
        i = 2;

    for (; arg[i]; i++) {
        uint8_t c = arg[i];
        uint8_t v;

        if (c >= '0' && c <= '9')
            v = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v = (uint8_t)(c - 'A' + 10);
        else
            return 0;

        addr = (addr << 4) | v;
    }

    if (!addr)
        return 0;

    winhost = (ThemWinBlock *)addr;

    if (winhost->magic != THEMWIN_MAGIC || winhost->version != THEMWIN_VERSION) {
        winhost = (ThemWinBlock *)0;
        return 0;
    }

    heard_beat = winhost->hostBeat;
    heard_at = get_ticks();

    return 1;
}

void winhost_beat(void) {
    uint64_t now;

    if (!winhost)
        return;

    now = get_ticks();
    if (now - beat_at < 100)
        return;

    beat_at = now;
    winhost->themBeat = winhost->themBeat + 1;

    if (winhost->hostBeat != heard_beat) {
        heard_beat = winhost->hostBeat;
        heard_at = now;
    } else if (now - heard_at > 10000) {
        host_gone = 1; /* the window died, or was taken down without asking */
    }
}

uint8_t winhost_quit(void) { return winhost && (winhost->quit || host_gone); }

uint8_t winhost_key(void) {
    uint8_t sc;

    if (!winhost || winhost->keyTail == winhost->keyHead)
        return 0;

    sc = winhost->keys[winhost->keyTail % THEMWIN_KEYS];
    __asm__ volatile("" ::: "memory");
    winhost->keyTail = winhost->keyTail + 1;

    return sc;
}

void winhost_fail(const uint8_t *why) {
    uint16_t i = 0;

    if (!winhost)
        return;

    for (; why[i] && i < sizeof(winhost->message) - 1; i++)
        winhost->message[i] = (char)why[i];
    winhost->message[i] = 0;
    winhost->failed = 1;
}

void winhost_detach(uint16_t exit_code) {
    if (!winhost)
        return;

    winhost->exitCode = exit_code;
    __asm__ volatile("" ::: "memory");
    winhost->exited = 1;
    winhost = (ThemWinBlock *)0;
}

/* ── Colours ──────────────────────────────────────────────────────────────── */

/*
 *  The window's colours: the 16 EGA ones, and with 256 Memento's cube and
 *  greys after them (R2_Palette in Memento's r2 backend). Only the guest's
 *  256 DAC colours need matching, not all 32768 possible five-bit RGBs.
 *  Cache each match until that DAC colour or the window's colour count changes.
 *  Nearest rather than dithered keeps the game's pixel art and text crisp.
 */
static const uint8_t ega[16][3] = {
    {0, 0, 0},    {0, 0, 170},   {0, 170, 0},   {0, 170, 170},  {170, 0, 0},   {170, 0, 170},  {170, 85, 0},   {170, 170, 170},
    {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255}, {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255},
};

static uint8_t palette_map[256];
static uint16_t palette_rgb[256]; /* five-bit RGB key of each cached match */
static uint16_t palette_for = 0;  /* the window's colour count */

static void host_rgb(uint16_t idx, int32_t *r, int32_t *g, int32_t *b) {
    static const uint8_t level[6] = {0, 51, 102, 153, 204, 255};

    if (idx < 16) {
        *r = ega[idx][0];
        *g = ega[idx][1];
        *b = ega[idx][2];
    } else if (idx < 232) {
        uint16_t c = (uint16_t)(idx - 16);

        *r = level[c / 36];
        *g = level[(c / 6) % 6];
        *b = level[c % 6];
    } else {
        *r = *g = *b = 8 + (idx - 232) * 10;
    }
}

static void build_palette(const uint8_t *dac, uint16_t colours) {
    uint16_t count = colours >= 256 ? 256 : 16;
    uint8_t same_host = (uint8_t)(palette_for == count);

    for (uint16_t slot = 0; slot < 256; slot++) {
        uint16_t k = (uint16_t)(((dac[slot * 3] >> 3) << 10) | ((dac[slot * 3 + 1] >> 3) << 5) | (dac[slot * 3 + 2] >> 3));

        if (same_host && palette_rgb[slot] == k)
            continue;

        /* Keep the same five-bit quantisation and tie order as the old
         * full RGB lookup, so the optimisation does not change the colours. */
        int32_t R = (int32_t)((k >> 10) & 31) * 255 / 31;
        int32_t G = (int32_t)((k >> 5) & 31) * 255 / 31;
        int32_t B = (int32_t)(k & 31) * 255 / 31;
        int32_t best = 0, best_d = 0x7FFFFFFF;
        uint16_t candidates = count == 256 ? 41 : 16;
        /* In the RGB cube each channel's closest level minimises its part
         * of the distance independently. Check that one cube colour, the
         * sixteen EGA colours and the 24 greys, in the original tie order. */
        uint16_t cube = (uint16_t)(16 + ((R + 25) / 51) * 36 + ((G + 25) / 51) * 6 + (B + 25) / 51);

        for (uint16_t n = 0; n < candidates; n++) {
            uint16_t i = n < 16 ? n : (n == 16 ? cube : (uint16_t)(232 + n - 17));
            int32_t r, g, b, dr, dg, db, d;

            host_rgb(i, &r, &g, &b);
            dr = R - r;
            dg = G - g;
            db = B - b;
            d = 3 * dr * dr + 4 * dg * dg + 2 * db * db; /* green counts most */
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }

        palette_map[slot] = (uint8_t)best;
        palette_rgb[slot] = k;
    }

    palette_for = count;
}

/* ── Text mode ────────────────────────────────────────────────────────────── */

static uint8_t font[256 * 16];
static uint8_t font_rows = 0;

static void load_font(void) {
    if (font_rows)
        return;

    int64_t rows = get_kernel_font(font, sizeof(font));

    font_rows = (rows > 0 && rows <= 16) ? (uint8_t)rows : 0;
}

/* The text screen, character cells drawn with the kernel font in their
 * attribute's colours: 640x400 for 80 columns and a 16-row font. */
static void render_text(Memory_T *mem, uint8_t *out, uint16_t *w, uint16_t *h) {
    uint16_t cols = (uint16_t)(mem->bytes[0x44A] | (mem->bytes[0x44B] << 8));
    uint8_t rows = 25;

    load_font();
    if (cols != 40)
        cols = 80;

    *w = (uint16_t)(cols * 8);
    *h = (uint16_t)(rows * (font_rows ? font_rows : 16));

    for (uint8_t row = 0; row < rows; row++)
        for (uint16_t col = 0; col < cols; col++) {
            uint32_t at = 0xB8000 + 2 * ((uint32_t)row * cols + col);
            uint8_t ch = mem->bytes[at];
            uint8_t attr = mem->bytes[at + 1];
            uint8_t fg = (uint8_t)(attr & 15), bg = (uint8_t)((attr >> 4) & 7);

            for (uint8_t y = 0; y < (font_rows ? font_rows : 16); y++) {
                uint8_t bits = font_rows ? font[(uint32_t)ch * font_rows + y] : 0;
                uint8_t *p = out + ((uint32_t)row * (font_rows ? font_rows : 16) + y) * *w + col * 8;

                for (uint8_t x = 0; x < 8; x++)
                    p[x] = ((bits >> (7 - x)) & 1) ? fg : bg;
            }
        }
}

/* ── Publishing ───────────────────────────────────────────────────────────── */

/* The buffer to write next, or THEMWIN_NONE while the window is reading it. */
static uint8_t back_buffer(void) {
    uint8_t b = (uint8_t)(winhost->front ^ 1);

    return winhost->reading == b ? (uint8_t)THEMWIN_NONE : b;
}

static void publish(uint8_t b, uint16_t w, uint16_t h) {
    winhost->width = w;
    winhost->height = h;
    __asm__ volatile("" ::: "memory");
    winhost->front = b;
    winhost->frame = winhost->frame + 1;
}

uint8_t winhost_publish(Memory_T *mem) {
    uint8_t b;
    uint16_t w = 0, h = 0;

    if (!winhost)
        return 1;

    b = back_buffer();
    if (b == THEMWIN_NONE)
        return 0;

    winhost->mode = vga_mode();

    if (vga_graphics()) {
        uint8_t *dst = winhost->pixels[b];

        build_palette(vga_palette(), winhost->colours);
        vga_frame_indexed(mem, dst, palette_map, THEMWIN_MAX_W, THEMWIN_MAX_H, &w, &h);
        if (!w || !h)
            return 1;
    } else {
        render_text(mem, winhost->pixels[b], &w, &h);
    }

    publish(b, w, h);

    return 1;
}

void winhost_publish_indexed(const uint8_t *frame, uint16_t w, uint16_t h) {
    uint8_t b;

    if (!winhost || (uint32_t)w * h > (uint32_t)THEMWIN_MAX_W * THEMWIN_MAX_H)
        return;

    /* A message is worth waiting a moment for the window to finish reading. */
    for (uint8_t tries = 0; tries < 50; tries++) {
        b = back_buffer();
        if (b != THEMWIN_NONE)
            break;
        sleep_ms(2);
    }
    if (b == THEMWIN_NONE)
        return;

    for (uint32_t i = 0; i < (uint32_t)w * h; i++)
        winhost->pixels[b][i] = (uint8_t)(frame[i] & 15);

    publish(b, w, h);
}

/* ── Console output ───────────────────────────────────────────────────────── */

/*
 *  What DOS and the BIOS print, as a terminal would show it: into the text
 *  screen at the cursor the BIOS data area keeps, scrolling at the bottom.
 *  On the kernel console when not in a window.
 */
void con_print(Memory_T *mem, const uint8_t *text) {
    uint16_t cols;
    uint8_t col, row;

    if (!winhost) {
        print(text);
        return;
    }

    cols = (uint16_t)(mem->bytes[0x44A] | (mem->bytes[0x44B] << 8));
    if (cols != 40)
        cols = 80;
    col = mem->bytes[0x450];
    row = mem->bytes[0x451];

    for (; *text; text++) {
        uint8_t ch = *text;

        if (ch == '\r') {
            col = 0;
        } else if (ch == '\n') {
            row++;
        } else if (ch == 8) {
            if (col)
                col--;
        } else if (ch == 7) {
            /* the bell: nothing to ring here */
        } else if (ch == '\t') {
            col = (uint8_t)((col + 8) & ~7);
        } else {
            uint32_t at = 0xB8000 + 2 * ((uint32_t)row * cols + col);

            mem->bytes[at] = ch;
            if (!mem->bytes[at + 1])
                mem->bytes[at + 1] = 0x07;
            col++;
        }

        if (col >= cols) {
            col = 0;
            row++;
        }

        if (row >= 25) {
            /* Scroll one line up and blank the last. */
            uint8_t *screen = &mem->bytes[0xB8000];

            for (uint32_t i = 0; i < (uint32_t)24 * cols * 2; i++)
                screen[i] = screen[i + cols * 2];
            for (uint32_t i = (uint32_t)24 * cols * 2; i < (uint32_t)25 * cols * 2; i += 2) {
                screen[i] = ' ';
                screen[i + 1] = 0x07;
            }
            row = 24;
        }
    }

    mem->bytes[0x450] = col;
    mem->bytes[0x451] = row;
    gfx_mark_dirty();
}
