#include "cpu.h"
#include "int.h"
#include "printf.h"
#include "vga.h"

uint8_t debug = 0;
static Memory_T ram;
static uint8_t pal16[48];
static uint32_t rgb;
static uint16_t width, height;

#define CHECK(expr) do { if (!(expr)) { \
    printf((const uint8_t *)"palette regression FAIL at line %u\n", (unsigned int)__LINE__); \
    return 1; } } while (0)

static void pixel(uint8_t value) {
    vga_port_write(0x3C4, 2);
    for (uint8_t p = 0; p < 4; p++) {
        vga_port_write(0x3C5, 1 << p);
        vga_mem_write(0, (value & (1 << p)) ? 0x80 : 0);
    }
    vga_port_write(0x3C5, 0x0F);
}

static uint32_t colour(void) {
    vga_frame_rgb(&ram, &rgb, 1, 1, &width, &height);
    return rgb;
}

static uint32_t planar_colour(uint8_t index) {
    vga_palette_16(pal16);
    return ((uint32_t)pal16[index * 3] << 16) |
           ((uint32_t)pal16[index * 3 + 1] << 8) | pal16[index * 3 + 2];
}

int main(void) {
    CHECK(vga_init());
    vga_set_mode(0x0D);
    pixel(6);
    CHECK(colour() == 0xAA5500); /* default brown in a 200-line mode */
    pixel(8);
    CHECK(colour() == 0x555555); /* bright colours use DAC 16-23 */
    CHECK(planar_colour(8) == colour());

    vga_set_mode(0x10);
    pixel(6);
    CHECK(colour() == 0xAA5500); /* default brown now uses DAC 20 */
    vga_set_ac_palette(6, 0x16);
    CHECK(colour() == 0xAAFF00); /* full six-bit EGA palette */
    CHECK(planar_colour(6) == colour());
    vga_set_ac_palette(6, 0x3A);
    CHECK(colour() == 0x55FF55);

    /* A game leaving custom DAC colours in the menu cannot poison the
     * default palette or AC state of the following level's mode set. */
    vga_set_dac(0x14, 0, 0, 0);
    vga_set_ac_palette(0x14, 0x0F);
    vga_set_mode(0x0D);
    pixel(6);
    CHECK(colour() == 0xAA5500);
    CHECK(vga_get_ac_palette(0x14) == 0);

    /* Both pixel decoders must follow colour banking and the PEL mask. */
    vga_set_ac_palette(6, 0x36);
    vga_set_ac_palette(0x10, 0x81); /* P54S: take bits 4-5 from Color Select */
    vga_set_ac_palette(0x14, 0x0A); /* resulting DAC index = A6 */
    vga_set_dac(0xA6, 0xFC, 0x80, 0x40);
    CHECK(colour() == 0xFF8241);
    CHECK(planar_colour(6) == colour());
    vga_set_dac(0x26, 0x40, 0xFC, 0x80);
    vga_port_write(0x3C6, 0x7F);
    CHECK(vga_port_read(0x3C6) == 0x7F);
    CHECK(colour() == 0x41FF82);
    CHECK(planar_colour(6) == colour());
    vga_port_write(0x3C6, 0xFF);
    vga_set_ac_palette(0x10, 1); /* retain palette bits 4-5, bank bits 6-7 */
    vga_set_dac(0xB6, 0x80, 0x40, 0xFC);
    CHECK(colour() == 0x8241FF);
    vga_set_ac_palette(0x12, 3); /* disable planes 2 and 3 for display */
    vga_set_ac_palette(2, 0x36);
    CHECK(planar_colour(6) == colour());

    /* BIOS colour-page queries and port writes share the same AC state. */
    CPU_T cpu = {0};
    cpu.AX = 0x1013;
    cpu.BX = 0x0100;
    handle_10h(&cpu, &ram);
    cpu.AX = 0x1013;
    cpu.BX = 0x0B01;
    handle_10h(&cpu, &ram);
    cpu.AX = 0x101A;
    handle_10h(&cpu, &ram);
    CHECK(cpu.BX == 0x0B01);
    cpu.AX = 0x1018;
    cpu.BX = 0x003F;
    handle_10h(&cpu, &ram);
    cpu.AX = 0x1019;
    handle_10h(&cpu, &ram);
    CHECK((cpu.BX & 0xFF) == 0x3F);

    /* BIOS and direct-port DAC writes expand the same six-bit intensities. */
    cpu.AX = 0x1010;
    cpu.BX = 5;
    cpu.CX = 0x2010;
    cpu.DX = 0x3F00;
    handle_10h(&cpu, &ram);
    vga_port_write(0x3C8, 7);
    vga_port_write(0x3C9, 63);
    vga_port_write(0x3C9, 32);
    vga_port_write(0x3C9, 16);
    const uint8_t *dac = vga_palette();
    for (uint8_t c = 0; c < 3; c++)
        CHECK(dac[5 * 3 + c] == dac[7 * 3 + c]);
    vga_port_write(0x3C7, 7);
    CHECK(vga_port_read(0x3C9) == 63);
    CHECK(vga_port_read(0x3C9) == 32);
    CHECK(vga_port_read(0x3C9) == 16);

    vga_set_mode(0x13);
    CHECK(vga_port_read(0x3C6) == 0xFF);
    ram.bytes[0xA0000] = 32;
    CHECK(colour() == 0x0000FF); /* first hue ring begins with blue */
    ram.bytes[0xA0000] = 56;
    CHECK(colour() == 0x7D7DFF); /* following ring is less saturated */
    ram.bytes[0xA0000] = 248;
    CHECK(colour() == 0); /* final eight defaults are black */
    vga_set_dac(5, 0xFC, 0, 0);
    ram.bytes[0xA0000] = 0x85;
    vga_port_write(0x3C6, 0x7F);
    CHECK(colour() == 0xFF0000);
    vga_port_write(0x3C4, 4);
    vga_port_write(0x3C5, 6); /* Mode X also applies the PEL mask */
    vga_port_write(0x3C4, 2);
    vga_port_write(0x3C5, 1);
    vga_mem_write(0, 0x85);
    CHECK(colour() == 0xFF0000);
    vga_free();
    printf((const uint8_t *)"palette regression PASS\n");
    return 0;
}
