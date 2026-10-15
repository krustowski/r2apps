#include "vga.h"
#include "io.h"
#include "log.h"
#include "printf.h"
#include "syscall.h"

/*
 *  vga.c
 *
 *  EGA/VGA adapter emulation.
 *
 *  The 16-colour modes keep their pixels in four bit planes, exactly like
 *  the real hardware: a byte written by the guest touches up to four planes
 *  at once, filtered through the Sequencer map mask, the Graphics Controller
 *  bit mask, the set/reset registers and the 4-byte latch. Games draw
 *  through that logic instead of writing pixels, so it has to be emulated
 *  bit-exactly or the picture falls apart.
 */

extern uint8_t debug;

static uint8_t *planes = (uint8_t *)0; /* 4 x 64 KiB */

/* The flattened frame is handed to the kernel by pointer, and r2 only
 * accepts pointers into the program image, not into the heap -- so it lives
 * in BSS at a fixed 320x200, the size every mode is presented at. */
#define FRAME_W 320
#define FRAME_H 200
static uint8_t chunky[FRAME_W * FRAME_H];

/* One decoded scanline, and the palette as whole 32bpp pixels. Both are
 * scratch for vga_frame_rgb(), which builds a picture at the guest's own
 * size a row at a time rather than holding a second one of its own. */
static uint8_t row_index[VGA_MAX_W];
static uint32_t rgb_lut[256];

static uint8_t latch[4];

/* Sequencer */
static uint8_t seq_index;
static uint8_t seq_reg[8];

/* Graphics controller */
static uint8_t gc_index;
static uint8_t gc_reg[16];

/* Attribute controller */
static uint8_t ac_index;
static uint8_t ac_flipflop; /* 0 = expecting index, 1 = expecting data */
static uint8_t ac_reg[32];

/* CRT controller */
static uint8_t crtc_index;
static uint8_t crtc_reg[32];

/* Misc output + input status */
static uint8_t misc_output;
static uint8_t status_toggle;

/* The length of a frame on the CRT, 14 ms being the 70 Hz of the VGA modes
 * near enough. */
#define RETRACE_PERIOD_MS 14
static uint64_t retrace_started_ms;
static uint64_t retrace_next_ms;
static uint8_t retrace_active;

/* DAC */
static uint8_t dac_write_index;
static uint8_t dac_read_index;
static uint8_t dac_latch;
static uint8_t pel_mask = 0xFF;
static uint8_t dac[256 * 3];

static uint8_t mode_number = 0x03;
static uint16_t mode_w = 640, mode_h = 400;
static uint8_t mode_planar = 0;
static uint8_t mode_graphics = 0;
static uint8_t dirty = 1;

/* Default EGA/VGA 16-colour entries, R/G/B in 0..255. */
static const uint8_t ega_default[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA}, {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF}, {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
};

/* The 16 attribute-controller defaults of a text/EGA mode: pixel value n
 * selects DAC entry ega_ac_default[n]. */
static const uint8_t ega_ac_default[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};

static uint8_t dac_expand(uint8_t value) {
    value &= 0x3F;
    return (uint8_t)((value << 2) | (value >> 4));
}

static void dac_default(uint8_t mode) {
    /* BIOS defaults differ by mode. EGA uses all six RGB bits of the
     * palette index; 200-line modes use the CGA RGBI palette with aliases.
     * Mode 13h uses sixteen RGBI colours, greys, then nine hue rings.
     * Mode/palette reference:
     * https://github.com/coreboot/seabios/blob/master/vgasrc/stdvgamodes.c */
    for (uint16_t i = 0; i < sizeof(dac); i++)
        dac[i] = 0;

    if (mode != 0x13) {
        for (uint8_t i = 0; i < 64; i++) {
            for (uint8_t c = 0; c < 3; c++) {
                if (mode == 0x0D || mode == 0x0E || (mode >= 4 && mode <= 6))
                    dac[i * 3 + c] = ega_default[(i & 7) | ((i & 0x10) >> 1)][c];
                else
                    dac[i * 3 + c] = (uint8_t)((((i >> (2 - c)) & 1) * 170) + (((i >> (5 - c)) & 1) * 85));
            }
        }
        return;
    }

    for (uint8_t i = 0; i < 16; i++)
        for (uint8_t c = 0; c < 3; c++)
            dac[i * 3 + c] = ega_default[i][c];

    static const uint8_t greys[16] = {0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63};
    static const uint8_t levels[9][5] = {{0, 16, 31, 47, 63},  {31, 39, 47, 55, 63}, {45, 49, 54, 58, 63}, {0, 7, 14, 21, 28},  {14, 17, 21, 24, 28},
                                         {20, 22, 24, 26, 28}, {0, 4, 8, 12, 16},    {8, 10, 12, 14, 16},  {11, 12, 13, 15, 16}};
    for (uint8_t i = 0; i < 16; i++)
        for (uint8_t c = 0; c < 3; c++)
            dac[(16 + i) * 3 + c] = dac_expand(greys[i]);

    for (uint8_t ring = 0; ring < 9; ring++) {
        for (uint8_t hue = 0; hue < 24; hue++) {
            uint8_t phase = hue / 4;
            uint8_t ramp = hue & 3;
            uint8_t rgb[3] = {0, 0, 0};
            /* Start at blue, travel through magenta/red/yellow/green/cyan. */
            static const uint8_t rising[6] = {0, 2, 1, 0, 2, 1};
            static const uint8_t fixed[6] = {2, 0, 0, 1, 1, 2};
            rgb[fixed[phase]] = 4;
            rgb[rising[phase]] = (phase & 1) ? 4 - ramp : ramp;
            for (uint8_t c = 0; c < 3; c++)
                dac[(32 + ring * 24 + hue) * 3 + c] = dac_expand(levels[ring][rgb[c]]);
        }
    }
}

static void retrace_reset(void) {
    retrace_active = 0;
    retrace_next_ms = hw_millis() + RETRACE_PERIOD_MS - 1;
}

uint8_t vga_init(void) {
    if (planes)
        return 1;

    planes = (uint8_t *)malloc(4 * VGA_PLANE_SIZE);
    log_line((const uint8_t *)"malloc planes %u -> %x", (unsigned int)(4 * VGA_PLANE_SIZE), (unsigned int)(uint64_t)planes);

    if (!planes)
        return 0;

    for (uint32_t i = 0; i < 4 * VGA_PLANE_SIZE; i++)
        planes[i] = 0;

    for (uint8_t i = 0; i < 16; i++)
        ac_reg[i] = ega_ac_default[i];

    seq_reg[2] = 0x0F; /* map mask: all planes enabled */
    gc_reg[8] = 0xFF;  /* bit mask: all bits writable */
    gc_reg[5] = 0x00;
    misc_output = 0x67;

    ac_reg[0x12] = 0x0F;
    dac_default(0x03);
    retrace_reset();

    return 1;
}

/*
 *  Hand the plane buffer back.
 *
 *  Nothing reclaims a heap block when a process exits -- the kernel heap is
 *  one region shared by every process -- so a quarter of a megabyte stays
 *  gone until the machine reboots if the emulator returns still holding it.
 */
void vga_free(void) {
    if (!planes)
        return;

    free(planes);
    planes = (uint8_t *)0;
}

/*
 *  What the adapter is actually doing.
 *
 *  The BIOS mode number is only where a mode starts. A program that wants
 *  256 colours with more than one page -- "Mode X" -- asks the BIOS for 13h
 *  or a planar mode and then reprograms the adapter by hand: chain-4 off in
 *  the Sequencer, the 256-colour shift on in the Graphics Controller, and
 *  its own CRTC timings. None of that changes the mode number, so the
 *  pixel format has to be read back out of the registers rather than
 *  assumed from the mode the guest asked for.
 */

/* 1 when video memory is unchained and every byte is one 8-bit pixel. */
uint8_t vga_unchained(void) { return (uint8_t)(mode_graphics && ((gc_reg[5] >> 6) & 1) && !((seq_reg[4] >> 3) & 1)); }

uint8_t vga_mode(void) { return mode_number; }

/* 1 only in the 16-colour planar modes: four planes, one bit a pixel. */
uint8_t vga_planar(void) { return (uint8_t)(mode_planar && !vga_unchained()); }

/* 1 when the 0xA0000 window is the adapter's write logic and not plain RAM. */
uint8_t vga_mapped(void) { return (uint8_t)(mode_planar || vga_unchained()); }

uint8_t vga_graphics(void) { return mode_graphics; }
uint8_t vga_dirty(void) { return dirty; }
void vga_clear_dirty(void) { dirty = 0; }

void vga_set_mode(uint8_t mode) {
    vga_init();

    log_line((const uint8_t *)"set_mode %x", (unsigned int)mode);

    mode_number = mode;
    dirty = 1;
    retrace_reset();

    switch (mode) {
    case 0x04:
    case 0x05:
    case 0x06: /* CGA graphics — not planar, handled as a linear window */
        mode_w = 320;
        mode_h = 200;
        mode_planar = 0;
        mode_graphics = 1;
        break;
    case 0x0D:
        mode_w = 320;
        mode_h = 200;
        mode_planar = 1;
        mode_graphics = 1;
        break;
    case 0x0E:
        mode_w = 640;
        mode_h = 200;
        mode_planar = 1;
        mode_graphics = 1;
        break;
    case 0x0F:
    case 0x10:
        mode_w = 640;
        mode_h = 350;
        mode_planar = 1;
        mode_graphics = 1;
        break;
    case 0x11:
    case 0x12:
        mode_w = 640;
        mode_h = 480;
        mode_planar = 1;
        mode_graphics = 1;
        break;
    case 0x13:
        mode_w = 320;
        mode_h = 200;
        mode_planar = 0;
        mode_graphics = 1;
        break;
    default: /* text */
        mode_w = 640;
        mode_h = 400;
        mode_planar = 0;
        mode_graphics = 0;
        break;
    }

    /* A mode set clears video memory and resets the drawing registers. */
    for (uint32_t i = 0; i < 4 * VGA_PLANE_SIZE; i++)
        planes[i] = 0;

    seq_reg[2] = 0x0F;
    gc_reg[0] = 0;
    gc_reg[1] = 0;
    gc_reg[3] = 0;
    gc_reg[8] = 0xFF;

    /* The registers the pixel format is read back from. 13h is the chained
     * 256-colour mode: eight bits a pixel, chain-4 on. A guest turning
     * chain-4 off from here is what makes Mode X. */
    if (mode == 0x13) {
        seq_reg[4] = 0x0E; /* chain-4 + extended memory */
        gc_reg[5] = 0x40;  /* 256-colour shift */
        ac_reg[0x10] = 0x41;
    } else {
        seq_reg[4] = mode_graphics ? 0x06 : 0x02;
        gc_reg[5] = 0x00;
        ac_reg[0x10] = mode_graphics ? 0x01 : 0x0C;
    }

    /*
     *  CRTC timings. vga_frame() takes the picture's geometry from these
     *  rather than from mode_w/mode_h, so a guest that reprograms them --
     *  which is the whole of setting up Mode X -- moves the picture with it.
     *  The values here are what the BIOS would have left behind, so the
     *  standard modes come out exactly as before.
     */
    {
        uint8_t bpp8 = (uint8_t)((gc_reg[5] >> 6) & 1);
        uint16_t bytes = (uint16_t)(mode_w / (bpp8 ? 4 : 8));
        uint8_t doubled = (uint8_t)(mode_h <= 200);
        uint16_t vde = (uint16_t)(mode_h * (doubled ? 2 : 1) - 1);

        crtc_reg[0x01] = (uint8_t)(bytes - 1);
        crtc_reg[0x07] = (uint8_t)(((vde >> 7) & 0x02) | ((vde >> 3) & 0x40));
        crtc_reg[0x09] = (uint8_t)(doubled ? 0x41 : 0x40);
        crtc_reg[0x12] = (uint8_t)(vde & 0xFF);
        crtc_reg[0x13] = (uint8_t)(bytes / 2);
    }

    crtc_reg[0x0C] = 0;
    crtc_reg[0x0D] = 0;

    for (uint8_t i = 0; i < 16; i++) {
        if (mode == 0x13)
            ac_reg[i] = i;
        else if (mode == 0x0D || mode == 0x0E)
            ac_reg[i] = (i & 7) | ((i & 8) << 1);
        else
            ac_reg[i] = ega_ac_default[i];
    }
    ac_reg[0x11] = 0;
    ac_reg[0x12] = 0x0F;
    ac_reg[0x14] = 0;
    ac_flipflop = 0;
    pel_mask = 0xFF;
    dac_default(mode);
}

/*
 *  Planar memory access.
 */

static uint8_t rotate_right8(uint8_t v, uint8_t n) {
    n &= 7;
    if (!n)
        return v;
    return (uint8_t)((v >> n) | (v << (8 - n)));
}

uint8_t vga_mem_read(uint32_t addr) {
    uint32_t off = (addr - 0xA0000) & 0xFFFF;

    latch[0] = planes[0 * VGA_PLANE_SIZE + off];
    latch[1] = planes[1 * VGA_PLANE_SIZE + off];
    latch[2] = planes[2 * VGA_PLANE_SIZE + off];
    latch[3] = planes[3 * VGA_PLANE_SIZE + off];

    if ((gc_reg[5] >> 3) & 1) {
        /* Read mode 1: colour compare */
        uint8_t care = gc_reg[7] & 0x0F;
        uint8_t cmp = gc_reg[2] & 0x0F;
        uint8_t result = 0;

        for (uint8_t bit = 0; bit < 8; bit++) {
            uint8_t pixel = 0;
            for (uint8_t p = 0; p < 4; p++)
                if ((latch[p] >> (7 - bit)) & 1)
                    pixel |= (uint8_t)(1 << p);
            if (((pixel ^ cmp) & care) == 0)
                result |= (uint8_t)(1 << (7 - bit));
        }

        return result;
    }

    return latch[gc_reg[4] & 3];
}

void vga_mem_write(uint32_t addr, uint8_t value) {
    uint32_t off = (addr - 0xA0000) & 0xFFFF;
    uint8_t write_mode = gc_reg[5] & 3;
    uint8_t map_mask = seq_reg[2] & 0x0F;
    uint8_t func = (gc_reg[3] >> 3) & 3;
    uint8_t bitmask = gc_reg[8];
    uint8_t set_reset = gc_reg[0] & 0x0F;
    uint8_t enable_sr = gc_reg[1] & 0x0F;
    uint8_t data = value;

    dirty = 1;

    if (write_mode == 0 || write_mode == 3)
        data = rotate_right8(value, gc_reg[3] & 7);

    if (write_mode == 3) {
        bitmask &= data;
        func = 0;
    }

    for (uint8_t p = 0; p < 4; p++) {
        uint8_t src;

        if (!((map_mask >> p) & 1))
            continue;

        switch (write_mode) {
        case 0:
            src = ((enable_sr >> p) & 1) ? (((set_reset >> p) & 1) ? 0xFF : 0x00) : data;
            break;
        case 1:
            /* Latch-to-screen copy: bit mask and ALU are bypassed. */
            planes[p * VGA_PLANE_SIZE + off] = latch[p];
            continue;
        case 2:
            src = ((value >> p) & 1) ? 0xFF : 0x00;
            break;
        default: /* 3 */
            src = ((set_reset >> p) & 1) ? 0xFF : 0x00;
            break;
        }

        switch (func) {
        case 1:
            src &= latch[p];
            break;
        case 2:
            src |= latch[p];
            break;
        case 3:
            src ^= latch[p];
            break;
        default:
            break;
        }

        planes[p * VGA_PLANE_SIZE + off] = (uint8_t)((src & bitmask) | (latch[p] & ~bitmask));
    }
}

/*
 *  Adapter registers.
 */

static uint8_t vertical_retrace(void) {
    uint64_t now = hw_millis();

    if (retrace_active) {
        if (now == retrace_started_ms)
            return 1;

        /* Let the guest see the trailing edge even if its next clock sample
         * is already past another frame. Otherwise a wait for retrace to
         * end could stay high forever under equally coarse polling. */
        retrace_active = 0;
        return 0;
    }

    if (now < retrace_next_ms)
        return 0;

    /* The cached clock can jump over a one-ms blanking interval. Deliver
     * one pulse when the guest next samples it, then pace the next from
     * here. Never replay missed frames or toggle on successive INs. */
    retrace_active = 1;
    retrace_started_ms = now;
    retrace_next_ms = now + RETRACE_PERIOD_MS;
    return 1;
}

uint8_t vga_port_read(uint16_t port) {
    switch (port) {
    case 0x3C0:
        return ac_index;
    case 0x3C1:
        return ac_reg[ac_index & 0x1F];
    case 0x3C2:
        return 0x10; /* colour display attached */
    case 0x3C4:
        return seq_index;
    case 0x3C5:
        return seq_reg[seq_index & 7];
    case 0x3C6:
        return pel_mask;
    case 0x3C7:
        return 0;
    case 0x3C8:
        return dac_write_index;
    case 0x3C9: {
        uint8_t v = dac[(uint16_t)dac_read_index * 3 + dac_latch];
        if (++dac_latch == 3) {
            dac_latch = 0;
            dac_read_index++;
        }
        return (uint8_t)(v >> 2); /* the DAC is 6 bits wide */
    }
    case 0x3CC:
        return misc_output;
    case 0x3CE:
        return gc_index;
    case 0x3CF:
        return gc_reg[gc_index & 0x0F];
    case 0x3B4:
    case 0x3D4:
        return crtc_index;
    case 0x3B5:
    case 0x3D5:
        return crtc_reg[crtc_index & 0x1F];
    case 0x3BA:
    case 0x3DA:
        /* Input status 1. Bit 3 = vertical retrace, bit 0 = display enable
         * off (horizontal or vertical blanking).
         *
         * Retrace is paced by real time, near 70 Hz, but its edges stay
         * observable when scheduling or a slow polling pass skips the narrow
         * blanking interval. An exact ms%14==13 test misses every pulse when
         * the cached clock advances in even steps, freezing HAUNTED.EXE in
         * its IN loop. Toggling on every read would instead let Ugh!'s
         * 2100-frame menu finish in milliseconds. Horizontal blanking still
         * toggles per read, as often as anything polling it could notice. */
        ac_flipflop = 0;
        status_toggle++;
        {
            uint8_t vr = vertical_retrace();

            return (uint8_t)((vr ? 0x08 : 0x00) | ((vr || (status_toggle & 1)) ? 0x01 : 0x00));
        }
    default:
        return 0xFF;
    }
}

/* How often the guest has flipped the two bits that decide the pixel format.
 * A picture that turns to stripes has usually had one of these move. */
static uint16_t shift256_flips = 0;
static uint16_t chain4_flips = 0;

void vga_flip_counts(uint16_t *shift256, uint16_t *chain4) {
    if (shift256)
        *shift256 = shift256_flips;
    if (chain4)
        *chain4 = chain4_flips;
}

void vga_port_write(uint16_t port, uint8_t value) {
    switch (port) {
    case 0x3C0:
        if (!ac_flipflop) {
            ac_index = value & 0x1F;
            ac_flipflop = 1;
        } else {
            ac_reg[ac_index & 0x1F] = value;
            ac_flipflop = 0;
            dirty = 1;
        }
        break;
    case 0x3C2:
        misc_output = value;
        break;
    case 0x3C4:
        seq_index = value;
        break;
    case 0x3C5:
        /* Counted, not written: a game may flip these every frame, and this
         * runs inside the interpreter. */
        if ((seq_index & 7) == 4 && ((seq_reg[4] ^ value) & 0x08))
            chain4_flips++;

        seq_reg[seq_index & 7] = value;
        if ((seq_index & 7) == 4) /* chain-4 decides the pixel format */
            dirty = 1;
        break;
    case 0x3C7:
        dac_read_index = value;
        dac_latch = 0;
        break;
    case 0x3C6:
        pel_mask = value;
        dirty = 1;
        break;
    case 0x3C8:
        dac_write_index = value;
        dac_latch = 0;
        break;
    case 0x3C9: {
        uint8_t v = (uint8_t)((value & 0x3F) << 2);
        v = (uint8_t)(v | (v >> 6)); /* 6 -> 8 bits */
        dac[(uint16_t)dac_write_index * 3 + dac_latch] = v;
        if (++dac_latch == 3) {
            dac_latch = 0;
            dac_write_index++;
        }
        dirty = 1;
        break;
    }
    case 0x3CE:
        gc_index = value;
        break;
    case 0x3CF:
        if ((gc_index & 0x0F) == 5 && ((gc_reg[5] ^ value) & 0x40))
            shift256_flips++;

        gc_reg[gc_index & 0x0F] = value;
        if ((gc_index & 0x0F) == 5) /* so does the 256-colour shift */
            dirty = 1;
        break;
    case 0x3B4:
    case 0x3D4:
        crtc_index = value;
        break;
    case 0x3B5:
    case 0x3D5:
        crtc_reg[crtc_index & 0x1F] = value;
        if ((crtc_index & 0x1F) == 0x0C || (crtc_index & 0x1F) == 0x0D)
            dirty = 1;
        break;
    default:
        break;
    }
}

void vga_set_dac(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    dac[(uint16_t)index * 3 + 0] = dac_expand(r >> 2);
    dac[(uint16_t)index * 3 + 1] = dac_expand(g >> 2);
    dac[(uint16_t)index * 3 + 2] = dac_expand(b >> 2);
    dirty = 1;
}

void vga_get_dac(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = dac[(uint16_t)index * 3 + 0];
    *g = dac[(uint16_t)index * 3 + 1];
    *b = dac[(uint16_t)index * 3 + 2];
}

void vga_set_ac_palette(uint8_t index, uint8_t value) {
    ac_reg[index & 0x1F] = value;
    dirty = 1;
}

uint8_t vga_get_ac_palette(uint8_t index) { return ac_reg[index & 0x1F]; }

const uint8_t *vga_palette(void) { return dac; }

/* AC palette registers supply bits 0-5. P54S optionally replaces bits
 * 4-5 with Color Select; Color Select always supplies bits 6-7. The PEL
 * mask is applied after this mapping, including in eight-bit modes.
 * https://www.scs.stanford.edu/10wi-cs140/pintos/specs/freevga/vga/attrreg.htm */
static uint8_t palette_index(uint8_t pixel) {
    uint8_t index = ac_reg[pixel & ac_reg[0x12] & 0x0F] & 0x3F;
    if (ac_reg[0x10] & 0x80)
        index = (index & 0x0F) | ((ac_reg[0x14] & 3) << 4);
    index |= (ac_reg[0x14] & 0x0C) << 4;
    return index & pel_mask;
}

uint8_t *vga_frame_buffer(void) { return planes ? chunky : (uint8_t *)0; }
uint16_t vga_frame_width(void) { return FRAME_W; }
uint16_t vga_frame_height(void) { return FRAME_H; }

/*
 *  Live geometry, read back from the CRT controller.
 *
 *  A guest in Mode X picks its own width, height and scan doubling, so the
 *  numbers vga_set_mode() started with only hold until it says otherwise.
 */

/* Bytes one scanline occupies in each plane.  The Offset register counts
 * words, so a 320 pixel Mode X row of 80 bytes reads back as 40. */
static uint16_t vga_pitch(void) {
    uint16_t pitch = (uint16_t)(crtc_reg[0x13] * 2);

    if (!pitch) {
        uint8_t bpp8 = (uint8_t)((gc_reg[5] >> 6) & 1);

        pitch = (uint16_t)(mode_w / (bpp8 ? 4 : 8));
    }

    return pitch ? pitch : 1;
}

/* Displayed pixels across. A character clock is eight dots, which the
 * 256-colour modes show as four pixels because the clock is halved.
 *
 * What halves it is eight-bit colour, not unchaining: 13h is chained and
 * halves it just the same, and vga_set_mode() writes the CRTC from that same
 * bit. Reading it back as "unchained" made a chained 256-colour mode measure
 * 640 pixels across when it is 320, and a row decoded that wide runs on into
 * the row below -- which at 200 lines is a second copy of the picture beside
 * the first, one scanline out and indistinguishable from it. */
static uint16_t vga_visible_width(void) {
    uint8_t bpp8 = (uint8_t)((gc_reg[5] >> 6) & 1);
    uint16_t w = (uint16_t)((crtc_reg[0x01] + 1) * (bpp8 ? 4 : 8));

    return w ? w : mode_w;
}

/* Displayed rows: the vertical display end, in scanlines, divided by the
 * scanlines each row of memory is repeated over. */
static uint16_t vga_visible_height(void) {
    uint16_t vde = crtc_reg[0x12];
    uint8_t per_row = (uint8_t)((crtc_reg[0x09] & 0x1F) + 1);
    uint16_t rows;

    if (crtc_reg[0x07] & 0x02)
        vde |= 0x100;
    if (crtc_reg[0x07] & 0x40)
        vde |= 0x200;
    vde++;

    if (crtc_reg[0x09] & 0x80) /* double scan */
        per_row = (uint8_t)(per_row * 2);

    rows = (uint16_t)(vde / (per_row ? per_row : 1));

    return rows ? rows : mode_h;
}

/* The numbers the frame will be decoded with. For the log: a picture that
 * turns to stripes has usually had one of these change under it. */
void vga_geometry(uint16_t *pitch, uint16_t *width, uint16_t *height) {
    if (pitch)
        *pitch = vga_pitch();
    if (width)
        *width = vga_visible_width();
    if (height)
        *height = vga_visible_height();
}

/*
 *  Resolve the display palette once per frame, rather than doing the AC's
 *  banking and PEL masking for every pixel. A window can also map the DAC
 *  indices into its own palette here, avoiding a full RGB intermediate.
 */
static void decode_palette(uint8_t *out, const uint8_t *map) {
    uint8_t planar = vga_planar();
    uint16_t count = planar ? 16 : 256;

    for (uint16_t i = 0; i < count; i++) {
        uint8_t slot = planar ? palette_index((uint8_t)i) : (uint8_t)(i & pel_mask);

        out[i] = map ? map[slot] : slot;
    }
}

/*
 *  Decode one row of the picture into palette indices.
 *
 *  <sy> is the source scanline to read; <dst> receives <dst_w> pixels sampled
 *  across the visible width. The three memory layouts differ only in how a
 *  pixel is fetched, so that is all this does -- which rows to ask for, and
 *  what becomes of them, is left to the callers below.
 */
static void decode_row(Memory_T *mem, uint16_t sy, uint8_t *dst, uint16_t dst_w, const uint8_t *palette) {
    uint16_t start = (uint16_t)((crtc_reg[0x0C] << 8) | crtc_reg[0x0D]);
    uint16_t pitch = vga_pitch();
    uint16_t gw = vga_visible_width();

    if (vga_unchained()) {
        /*
         *  Mode X. Every byte is one whole pixel, and pixel x of a row
         *  lives in plane x & 3 at byte x >> 2 -- the four planes are read
         *  in turn across the row rather than one bit each. The attribute
         *  controller is bypassed in 8-bit colour: the byte is already a
         *  DAC index.
         */
        for (uint16_t x = 0; x < dst_w; x++) {
            uint16_t sx = (uint16_t)((uint32_t)x * gw / dst_w);
            uint32_t off = (uint32_t)start + (uint32_t)sy * pitch + (sx >> 2);

            dst[x] = palette[planes[(sx & 3) * VGA_PLANE_SIZE + (off & (VGA_PLANE_SIZE - 1))]];
        }
    } else if (mode_planar) {
        for (uint16_t x = 0; x < dst_w; x++) {
            uint16_t sx = (uint16_t)((uint32_t)x * gw / dst_w);
            uint32_t off = (uint32_t)start + (uint32_t)sy * pitch + (sx >> 3);
            uint8_t bit = (uint8_t)(7 - (sx & 7));
            uint8_t pixel = 0;

            off &= (VGA_PLANE_SIZE - 1);

            for (uint8_t p = 0; p < 4; p++)
                if ((planes[p * VGA_PLANE_SIZE + off] >> bit) & 1)
                    pixel |= (uint8_t)(1 << p);

            /* The attribute controller maps the pixel value to a DAC slot. */
            dst[x] = palette[pixel];
        }
    } else if (mode_number == 0x13) {
        /* Linear 256-colour memory lives straight in the guest's RAM window,
         * one byte a pixel. Chain-4 spreads a row over the four planes behind
         * the CRTC, so a row is four times what the Offset register counts --
         * 320 bytes for the 80 it holds in 13h, and whatever the guest has
         * made of it otherwise. */
        uint32_t stride = (uint32_t)pitch * 4;

        for (uint16_t x = 0; x < dst_w; x++) {
            uint16_t sx = (uint16_t)((uint32_t)x * gw / dst_w);
            uint32_t off = (uint32_t)start + (uint32_t)sy * stride + sx;

            dst[x] = palette[mem->bytes[0xA0000 + (off & 0xFFFF)]];
        }
    } else {
        for (uint16_t x = 0; x < dst_w; x++)
            dst[x] = palette[0];
    }
}

/*
 *  Flatten video memory into one byte per pixel, at 320x200.
 *
 *  This is the picture for the mode 13h fallback, which is the size the
 *  hardware is in and cannot be talked out of, so a wider mode such as
 *  640x480 is sampled down into it. The framebuffer path does not have to
 *  give anything away and uses vga_frame_rgb() instead.
 */
const uint8_t *vga_frame(Memory_T *mem, uint16_t *w, uint16_t *h) {
    uint16_t gh = vga_visible_height();
    uint8_t palette[256];

    *w = FRAME_W;
    *h = FRAME_H;

    if (!planes)
        return (const uint8_t *)0;

    decode_palette(palette, (const uint8_t *)0);

    for (uint16_t y = 0; y < FRAME_H; y++)
        decode_row(mem, (uint16_t)((uint32_t)y * gh / FRAME_H), &chunky[(uint32_t)y * FRAME_W], FRAME_W, palette);

    return chunky;
}

void vga_frame_indexed(Memory_T *mem, uint8_t *dst, const uint8_t *map, uint16_t max_w, uint16_t max_h, uint16_t *w, uint16_t *h) {
    uint16_t gw = vga_visible_width(), gh = vga_visible_height();
    uint16_t dw, dh;
    uint8_t palette[256];

    *w = 0;
    *h = 0;
    if (!planes || !dst || !gw || !gh || !max_w || !max_h)
        return;

    if (max_w > VGA_MAX_W)
        max_w = VGA_MAX_W;
    dw = gw > max_w ? max_w : gw;
    dh = gh > max_h ? max_h : gh;
    decode_palette(palette, map);

    for (uint16_t y = 0; y < dh; y++)
        decode_row(mem, (uint16_t)((uint32_t)y * gh / dh), dst + (uint32_t)y * dw, dw, palette);

    *w = dw;
    *h = dh;
}

/*
 *  Copy one plane of the guest picture out in a host planar mode's layout.
 *
 *  <dst> receives <dst_h> rows of <dst_pitch> bytes, which is what the VGA
 *  wants written through its map mask with one plane selected. Nothing is
 *  sampled: a 16-colour guest is already in planes of exactly this shape, so
 *  the rows go out whole and a 640x480 mode arrives on the screen as the
 *  640x480 it is. A picture shorter than the host mode is centred and the
 *  rest left blank.
 */
void vga_plane_rows(uint8_t plane, uint8_t *dst, uint16_t dst_pitch, uint16_t dst_h) {
    uint16_t start = (uint16_t)((crtc_reg[0x0C] << 8) | crtc_reg[0x0D]);
    uint16_t pitch = vga_pitch();
    uint16_t gw = vga_visible_width();
    uint16_t gh = vga_visible_height();
    uint16_t copy = (uint16_t)(gw / 8);
    uint16_t top = 0;
    const uint8_t *src;

    if (!planes || !dst || !dst_pitch)
        return;

    if (copy > dst_pitch)
        copy = dst_pitch;
    if (gh < dst_h)
        top = (uint16_t)((dst_h - gh) / 2);

    src = &planes[(uint32_t)(plane & 3) * VGA_PLANE_SIZE];

    for (uint16_t y = 0; y < dst_h; y++) {
        uint8_t *out = dst + (uint32_t)y * dst_pitch;
        uint16_t sy = (uint16_t)(y - top);
        uint32_t off;
        uint16_t x = 0;

        if (y < top || sy >= gh) { /* above or below the guest's picture */
            for (; x < dst_pitch; x++)
                out[x] = 0;
            continue;
        }

        off = (uint32_t)start + (uint32_t)sy * pitch;

        for (; x < copy; x++)
            out[x] = src[(off + x) & (VGA_PLANE_SIZE - 1)];
        for (; x < dst_pitch; x++)
            out[x] = 0;
    }
}

/*
 *  The sixteen colours a planar mode is actually showing, as RGB triplets.
 *
 *  A pixel value is not a DAC slot in these modes: the attribute controller
 *  maps it to one first, and a guest that wants a colour it was not given
 *  changes that map rather than the palette. Both steps are followed here so
 *  the host's first sixteen DAC entries can be loaded to match.
 */
void vga_palette_16(uint8_t *out) {
    for (uint8_t i = 0; i < 16; i++) {
        uint16_t slot = palette_index(i);

        out[i * 3 + 0] = dac[slot * 3 + 0];
        out[i * 3 + 1] = dac[slot * 3 + 1];
        out[i * 3 + 2] = dac[slot * 3 + 2];
    }
}

/*
 *  Build the picture at the size the guest is actually running, as 32bpp
 *  0x00RRGGBB, ready for the kernel's scaled blit.
 *
 *  A 640x480 mode sampled down to 320x200 loses every other column and three
 *  rows in five, which no 8x8 font survives -- the whole reason a program
 *  asking for 640x480 was unreadable here. Decoded at its own size it is
 *  the framebuffer, not the emulator, that does the scaling, and the text
 *  arrives intact. Anything larger than the caller's buffer is still
 *  sampled down, there being nowhere else to put it.
 */
void vga_frame_rgb(Memory_T *mem, uint32_t *dst, uint16_t max_w, uint16_t max_h, uint16_t *w, uint16_t *h) {
    uint16_t gw = vga_visible_width();
    uint16_t gh = vga_visible_height();
    uint16_t dw, dh;
    uint8_t palette[256];

    *w = 0;
    *h = 0;

    if (!planes || !dst || !gw || !gh || !max_w || !max_h)
        return;

    /* decode_row() builds into a scanline of VGA_MAX_W, so that is the most
     * that can be asked for however generous the caller's buffer is. */
    if (max_w > VGA_MAX_W)
        max_w = VGA_MAX_W;

    dw = gw > max_w ? max_w : gw;
    dh = gh > max_h ? max_h : gh;
    decode_palette(palette, (const uint8_t *)0);

    /* The palette as whole pixels, looked up once rather than per pixel. */
    for (uint16_t i = 0; i < 256; i++)
        rgb_lut[i] = ((uint32_t)dac[i * 3 + 0] << 16) | ((uint32_t)dac[i * 3 + 1] << 8) | (uint32_t)dac[i * 3 + 2];

    for (uint16_t y = 0; y < dh; y++) {
        uint32_t *out = dst + (uint32_t)y * dw;

        decode_row(mem, (uint16_t)((uint32_t)y * gh / dh), row_index, dw, palette);

        for (uint16_t x = 0; x < dw; x++)
            out[x] = rgb_lut[row_index[x]];
    }

    *w = dw;
    *h = dh;
}
