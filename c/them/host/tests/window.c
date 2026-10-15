#include "gfx.h"
#include "printf.h"
#include "vga.h"
#include "winhost.h"

uint8_t debug = 0;
static Memory_T ram;
static ThemWinBlock block;
static uint32_t rgb[VGA_MAX_W * VGA_MAX_H];
static uint8_t indexed[VGA_MAX_W * VGA_MAX_H];
static uint8_t map[256];

#define CHECK(expr) do { if (!(expr)) { \
    printf((const uint8_t *)"window regression FAIL at line %u\n", (unsigned int)__LINE__); \
    return 1; } } while (0)

/* Compare the new indexed path with the RGB renderer and the window's
 * original five-bit nearest-colour rule, including its first-match ties. */
static uint8_t nearest(uint32_t colour, uint16_t count) {
    static const uint8_t ega[16][3] = {
        {0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170},
        {170, 0, 0}, {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
        {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255},
        {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255}
    };
    int32_t R = (int32_t)((colour >> 19) & 31) * 255 / 31;
    int32_t G = (int32_t)((colour >> 11) & 31) * 255 / 31;
    int32_t B = (int32_t)((colour >> 3) & 31) * 255 / 31;
    int32_t best_d = 0x7FFFFFFF;
    uint8_t best = 0;

    for (uint16_t i = 0; i < count; i++) {
        int32_t r, g, b;
        if (i < 16) {
            r = ega[i][0]; g = ega[i][1]; b = ega[i][2];
        } else if (i < 232) {
            r = ((i - 16) / 36) * 51;
            g = (((i - 16) / 6) % 6) * 51;
            b = ((i - 16) % 6) * 51;
        } else {
            r = g = b = 8 + (i - 232) * 10;
        }
        int32_t dr = R - r, dg = G - g, db = B - b;
        int32_t d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (d < best_d) {
            best_d = d;
            best = (uint8_t)i;
        }
    }
    return best;
}

static void strip(void) {
    vga_port_write(0x3C4, 2);
    for (uint8_t p = 0; p < 4; p++) {
        vga_port_write(0x3C5, 1 << p);
        for (uint8_t x = 0; x < 16; x += 8) {
            uint8_t bits = 0;
            for (uint8_t b = 0; b < 8; b++)
                if ((x + b) & (1 << p)) bits |= (uint8_t)(0x80 >> b);
            vga_mem_write(x >> 3, bits);
        }
    }
    vga_port_write(0x3C5, 0x0F);
}

static int compare(void) {
    uint16_t w, h, iw, ih;
    const uint8_t *dac = vga_palette();
    uint16_t count = block.colours >= 256 ? 256 : 16;
    vga_frame_rgb(&ram, rgb, VGA_MAX_W, VGA_MAX_H, &w, &h);
    CHECK(winhost_publish(&ram));
    CHECK(block.width == w && block.height == h);
    uint8_t *pixels = block.pixels[block.front];
    for (uint16_t i = 0; i < 256; i++) {
        uint32_t colour = ((uint32_t)dac[i * 3] << 16) |
                          ((uint32_t)dac[i * 3 + 1] << 8) | dac[i * 3 + 2];
        map[i] = nearest(colour, count);
    }
    vga_frame_indexed(&ram, indexed, (const uint8_t *)0, VGA_MAX_W, VGA_MAX_H, &iw, &ih);
    CHECK(w == iw && h == ih);
    for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
        uint16_t slot = indexed[i];
        CHECK(rgb[i] == (((uint32_t)dac[slot * 3] << 16) |
                         ((uint32_t)dac[slot * 3 + 1] << 8) | dac[slot * 3 + 2]));
        CHECK(pixels[i] == map[slot]);
    }

    /* Clamping/sampling must produce the same picture in both APIs and
     * leave everything beyond the caller's dimensions alone. */
    indexed[35] = 0xE5;
    vga_frame_indexed(&ram, indexed, map, 7, 5, &iw, &ih);
    vga_frame_rgb(&ram, rgb, 7, 5, &w, &h);
    CHECK(iw == 7 && ih == 5 && w == iw && h == ih);
    for (uint8_t i = 0; i < 35; i++)
        CHECK(indexed[i] == nearest(rgb[i], count));
    CHECK(indexed[35] == 0xE5);
    return 0;
}

int main(void) {
    CHECK(vga_init());
    winhost = &block;
    block.reading = THEMWIN_NONE;
    block.colours = 16;
    vga_set_mode(0x0D);
    strip();
    CHECK(winhost_publish(&ram));
    CHECK(block.width == 320 && block.height == 200 && block.frame == 1);
    for (uint8_t x = 0; x < 16; x++)
        CHECK(block.pixels[block.front][x] == x);

    /* AC remapping and DAC changes must take effect on the next frame. */
    vga_set_ac_palette(6, 0x3F);
    CHECK(winhost_publish(&ram));
    CHECK(block.pixels[block.front][6] == 15);
    vga_set_dac(0x3F, 0xFC, 0x80, 0x40);
    CHECK(winhost_publish(&ram));
    CHECK(block.pixels[block.front][6] == 12);
    block.colours = 256;
    CHECK(winhost_publish(&ram));
    CHECK(block.pixels[block.front][6] == 215);
    block.colours = 16;
    CHECK(compare() == 0);

    /* A busy back buffer must remain untouched and unpublished. */
    uint32_t frame = block.frame;
    uint8_t front = block.front;
    block.reading = (uint8_t)(front ^ 1);
    block.pixels[block.reading][0] = 0xE5;
    CHECK(!winhost_publish(&ram));
    CHECK(block.frame == frame && block.front == front);
    CHECK(block.pixels[block.reading][0] == 0xE5);
    block.reading = THEMWIN_NONE;

    vga_set_mode(0x10);
    strip();
    vga_set_ac_palette(6, 0x36);
    vga_set_ac_palette(0x10, 0x81);
    vga_set_ac_palette(0x14, 0x0A);
    vga_set_dac(0xA6, 0xFC, 0x80, 0x40);
    vga_port_write(0x3C6, 0x7F);
    vga_set_ac_palette(0x12, 7);
    CHECK(compare() == 0);
    block.colours = 256;
    CHECK(compare() == 0);
    vga_set_mode(0x12);
    strip();
    CHECK(compare() == 0);

    /* All DAC indices, cycling, PEL masking and unchained Mode X. */
    vga_set_mode(0x13);
    for (uint32_t i = 0; i < 64000; i++) ram.bytes[0xA0000 + i] = (uint8_t)i;
    CHECK(compare() == 0);
    for (uint16_t i = 0; i < 256; i++)
        vga_set_dac((uint8_t)i, (uint8_t)(i * 13), (uint8_t)(i * 37), (uint8_t)(i * 71));
    CHECK(compare() == 0);
    block.colours = 16;
    CHECK(compare() == 0);
    vga_port_write(0x3C6, 0x7F);
    CHECK(compare() == 0);
    vga_port_write(0x3C4, 4);
    vga_port_write(0x3C5, 6);
    vga_port_write(0x3C4, 2);
    for (uint8_t p = 0; p < 4; p++) {
        vga_port_write(0x3C5, 1 << p);
        for (uint32_t i = 0; i < 16000; i++) vga_mem_write(i, (uint8_t)(i * 4 + p));
    }
    CHECK(compare() == 0);

    /* Exhaust all five-bit RGB keys against the original full palette
     * search, including ties between EGA, cube and grey entries. A 256x1
     * mode keeps this colour-matching check independent of frame cost. */
    vga_set_mode(0x13);
    vga_port_write(0x3D4, 1); vga_port_write(0x3D5, 63);
    vga_port_write(0x3D4, 7); vga_port_write(0x3D5, 0);
    vga_port_write(0x3D4, 0x12); vga_port_write(0x3D5, 1);
    for (uint16_t i = 0; i < 256; i++) ram.bytes[0xA0000 + i] = (uint8_t)i;
    for (uint8_t bank = 0; bank < 2; bank++) {
        block.colours = bank ? 256 : 16;
        for (uint32_t base = 0; base < 32768; base += 256) {
            for (uint16_t i = 0; i < 256; i++) {
                uint32_t key = base + i;
                vga_set_dac((uint8_t)i, (uint8_t)(((key >> 10) & 31) << 3),
                            (uint8_t)(((key >> 5) & 31) << 3), (uint8_t)((key & 31) << 3));
            }
            CHECK(winhost_publish(&ram));
            CHECK(block.width == 256 && block.height == 1);
            const uint8_t *dac = vga_palette();
            for (uint16_t i = 0; i < 256; i++) {
                uint32_t colour = ((uint32_t)dac[i * 3] << 16) |
                                  ((uint32_t)dac[i * 3 + 1] << 8) | dac[i * 3 + 2];
                CHECK(block.pixels[block.front][i] == nearest(colour, block.colours));
            }
        }
    }
    vga_free();
    winhost = (ThemWinBlock *)0;
    printf((const uint8_t *)"window regression PASS\n");
    return 0;
}
