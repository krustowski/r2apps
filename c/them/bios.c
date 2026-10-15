#include "gfx.h"
#include "int.h"
#include "io.h"
#include "log.h"
#include "printf.h"
#include "syscall.h"
#include "vga.h"
#include "winhost.h"

/*
 *  bios.c
 *
 *  The ROM BIOS services.  Each handler is reached through a stub in the
 *  interrupt vector table, so a guest is free to hook any of these vectors
 *  and still chain back to the original.
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

/* BIOS data area offsets, all inside segment 0x0040. */
#define BDA_SEG 0x0040
#define BDA_EQUIPMENT 0x0010
#define BDA_MEMSIZE 0x0013
#define BDA_DISK_STATUS 0x0041
#define BDA_SHIFT1 0x0017
#define BDA_SHIFT2 0x0018
#define BDA_KBD_HEAD 0x001A
#define BDA_KBD_TAIL 0x001C
#define BDA_KBD_BUF 0x001E
#define BDA_KBD_END 0x003E
#define BDA_VIDEO_MODE 0x0049
#define BDA_VIDEO_COLS 0x004A
#define BDA_CURSOR 0x0050
#define BDA_TICKS 0x006C
#define BDA_VIDEO_ROWS 0x0084

/* US keyboard, scancode set 1. */
static const uint8_t kbd_plain[0x54] = {
    0,    0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-',  '=', 0x08, 0x09, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    0x0D, 0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*',
    0,    ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    0,   0,    '7',  '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};

static const uint8_t kbd_shift[0x54] = {
    0,    0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0x08, 0x09, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    0x0D, 0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,    '|',  'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*',
    0,    ' ',  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    '7',  '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};

static uint8_t bda_r8(Memory_T *mem, uint16_t off) { return mem_r8(mem, BDA_SEG, off); }
static void bda_w8(Memory_T *mem, uint16_t off, uint8_t v) { mem_w8(mem, BDA_SEG, off, v); }
static uint16_t bda_r16(Memory_T *mem, uint16_t off) { return mem_r16(mem, BDA_SEG, off); }
static void bda_w16(Memory_T *mem, uint16_t off, uint16_t v) { mem_w16(mem, BDA_SEG, off, v); }

static void kbd_buffer_put(Memory_T *mem, uint16_t entry) {
    uint16_t tail = bda_r16(mem, BDA_KBD_TAIL);
    uint16_t next = (uint16_t)(tail + 2);

    if (next >= BDA_KBD_END)
        next = BDA_KBD_BUF;

    if (next == bda_r16(mem, BDA_KBD_HEAD))
        return; /* full */

    mem_w16(mem, BDA_SEG, tail, entry);
    bda_w16(mem, BDA_KBD_TAIL, next);
}

static uint8_t kbd_buffer_peek(Memory_T *mem, uint16_t *entry) {
    uint16_t head = bda_r16(mem, BDA_KBD_HEAD);

    if (head == bda_r16(mem, BDA_KBD_TAIL))
        return 0;

    *entry = mem_r16(mem, BDA_SEG, head);
    return 1;
}

static uint8_t kbd_buffer_get(Memory_T *mem, uint16_t *entry) {
    uint16_t head = bda_r16(mem, BDA_KBD_HEAD);

    if (!kbd_buffer_peek(mem, entry))
        return 0;

    head = (uint16_t)(head + 2);
    if (head >= BDA_KBD_END)
        head = BDA_KBD_BUF;

    bda_w16(mem, BDA_KBD_HEAD, head);
    return 1;
}

/*
 *  Re-run the INT instruction that got us here.
 *
 *  A BIOS call that has to wait for input cannot block inside the emulator:
 *  the guest's own interrupt handlers have to keep running to produce that
 *  input.  Rewinding the return address turns the wait into a poll that
 *  costs one instruction per pass, exactly like a real BIOS spin loop.
 */
void bios_retry_instruction(CPU_T *cpu, Memory_T *mem) {
    uint16_t ip = mem_r16(mem, cpu->SS, cpu->SP);
    uint16_t flags = mem_r16(mem, cpu->SS, (uint16_t)(cpu->SP + 4));

    mem_w16(mem, cpu->SS, cpu->SP, (uint16_t)(ip - 2));
    mem_w16(mem, cpu->SS, (uint16_t)(cpu->SP + 4), (uint16_t)(flags | IF));
}

/*
 *  handle_05h()
 *
 *  BOUND range exceeded.
 */
void handle_05h(CPU_T *cpu, Memory_T *memory) {
    if (debug)
        log_line((const uint8_t *)"INT 05 BOUND at %x:%x", (unsigned int)cpu->CS, (unsigned int)cpu->IP);
}

/*
 *  handle_08h()
 *
 *  IRQ0, the 18.2 Hz system timer. Updates the BIOS tick count and hands
 *  control to whatever sits on INT 1Ch.
 */
void handle_08h(CPU_T *cpu, Memory_T *memory) {
    uint32_t ticks = hw_bios_ticks();

    mem_w16(memory, BDA_SEG, BDA_TICKS, (uint16_t)(ticks & 0xFFFF));
    mem_w16(memory, BDA_SEG, (uint16_t)(BDA_TICKS + 2), (uint16_t)(ticks >> 16));

    io_out8(0x20, 0x20); /* end of interrupt (EOI) */

    cpu_int(cpu, memory, 0x1C);
}

/*
 *  handle_09h()
 *
 *  IRQ1, a key make or break code is waiting in the keyboard controller.
 */
void handle_09h(CPU_T *cpu, Memory_T *memory) {
    uint8_t sc = io_in8(0x60);
    uint8_t make = (uint8_t)(sc & 0x7F);
    uint8_t released = (uint8_t)(sc & 0x80);
    uint8_t shift = bda_r8(memory, BDA_SHIFT1);

    switch (make) {
    case 0x2A: /* left shift */
        shift = (uint8_t)(released ? (shift & ~0x02) : (shift | 0x02));
        break;
    case 0x36: /* right shift */
        shift = (uint8_t)(released ? (shift & ~0x01) : (shift | 0x01));
        break;
    case 0x1D: /* ctrl */
        shift = (uint8_t)(released ? (shift & ~0x04) : (shift | 0x04));
        break;
    case 0x38: /* alt */
        shift = (uint8_t)(released ? (shift & ~0x08) : (shift | 0x08));
        break;
    case 0x3A: /* caps lock */
        if (!released)
            shift = (uint8_t)(shift ^ 0x40);
        break;
    default:
        break;
    }

    bda_w8(memory, BDA_SHIFT1, shift);

    if (!released && make < 0x54) {
        uint8_t ascii = (shift & 0x03) ? kbd_shift[make] : kbd_plain[make];

        if ((shift & 0x40) && ascii >= 'a' && ascii <= 'z')
            ascii = (uint8_t)(ascii - 32);
        else if ((shift & 0x40) && ascii >= 'A' && ascii <= 'Z' && (shift & 0x03))
            ascii = (uint8_t)(ascii + 32);

        if (shift & 0x04) { /* ctrl */
            if (ascii >= 'a' && ascii <= 'z')
                ascii = (uint8_t)(ascii - 'a' + 1);
            else if (ascii >= 'A' && ascii <= 'Z')
                ascii = (uint8_t)(ascii - 'A' + 1);
        }

        kbd_buffer_put(memory, (uint16_t)((make << 8) | ascii));
    }

    io_out8(0x20, 0x20);
}

/*
 *  handle_10h()
 *
 *  Video services.
 */
void handle_10h(CPU_T *cpu, Memory_T *memory) {
    SRV_10H service = cpu->AX >> 8;
    uint8_t al = (uint8_t)(cpu->AX & 0xFF);

    switch (service) {
    case SET_VIDEO_MODE: {
        uint8_t mode = (uint8_t)(al & 0x7F);

        vga_set_mode(mode);
        gfx_mode_changed(mode);

        bda_w8(memory, BDA_VIDEO_MODE, mode);
        bda_w16(memory, BDA_VIDEO_COLS, (mode == 0x0D || mode == 0x13) ? 40 : 80);
        bda_w8(memory, BDA_VIDEO_ROWS, 24);

        if (mode == 0x03)
            for (uint32_t i = 0; i < 80 * 25; i++)
                mem_w16(memory, 0xB800, (uint16_t)(i * 2), 0x0720);
        break;
    }
    case SET_CURSOR_SHAPE:
        break;
    case SET_CURSOR_POSITION:
        bda_w16(memory, BDA_CURSOR, cpu->DX);
        break;
    case GET_CURSOR_POSITION_AND_SHAPE:
        cpu->DX = bda_r16(memory, BDA_CURSOR);
        cpu->CX = 0x0607;
        break;
    case GET_DISPLAY_PAGE:
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | bda_r8(memory, BDA_VIDEO_MODE));
        cpu->BX &= 0x00FF;
        break;
    case CLEAR_SCROLL_SCREEN_UP:
    case CLEAR_SCROLL_SCREEN_DOWN: {
        /* Only the "blank the window" form is worth supporting here. */
        uint8_t top = (uint8_t)(cpu->CX >> 8);
        uint8_t left = (uint8_t)(cpu->CX & 0xFF);
        uint8_t bottom = (uint8_t)(cpu->DX >> 8);
        uint8_t right = (uint8_t)(cpu->DX & 0xFF);
        uint16_t cell = (uint16_t)(((cpu->BX >> 8) << 8) | ' ');

        if (bottom > 24)
            bottom = 24;
        if (right > 79)
            right = 79;

        for (uint8_t row = top; row <= bottom; row++)
            for (uint8_t col = left; col <= right; col++)
                mem_w16(memory, 0xB800, (uint16_t)(2 * (row * 80 + col)), cell);
        break;
    }
    case READ_CHAR_WITH_ATTR_AT_CURSOR: {
        uint16_t pos = bda_r16(memory, BDA_CURSOR);
        uint8_t row = (uint8_t)(pos >> 8);
        uint8_t col = (uint8_t)(pos & 0xFF);
        cpu->AX = mem_r16(memory, 0xB800, (uint16_t)(2 * (row * 80 + col)));
        break;
    }
    case WRITE_CHAR_WITH_ATTR_AT_CURSOR:
    case WRITE_CHAR_AT_CURSOR: {
        uint16_t pos = bda_r16(memory, BDA_CURSOR);
        uint8_t row = (uint8_t)(pos >> 8);
        uint8_t col = (uint8_t)(pos & 0xFF);
        uint16_t count = cpu->CX ? cpu->CX : 1;
        uint8_t attr = (service == WRITE_CHAR_WITH_ATTR_AT_CURSOR) ? (uint8_t)(cpu->BX & 0xFF) : mem_r8(memory, 0xB800, (uint16_t)(2 * (row * 80 + col) + 1));

        for (uint16_t i = 0; i < count && col + i < 80; i++)
            mem_w16(memory, 0xB800, (uint16_t)(2 * (row * 80 + col + i)), (uint16_t)((attr << 8) | al));
        break;
    }
    case WRITE_GRAPHICS_PIXEL: {
        /* AL = colour, CX = column, DX = row */
        uint16_t x = cpu->CX;
        uint16_t y = cpu->DX;

        if (vga_planar()) {
            uint16_t pitch = 40; /* mode 0Dh */
            if (vga_mode() != 0x0D)
                pitch = 80;
            {
                uint32_t addr = 0xA0000 + (uint32_t)y * pitch + (x >> 3);
                uint8_t bit = (uint8_t)(0x80 >> (x & 7));

                /* Read to fill the latches, then write one pixel through
                 * the bit mask, the way the real BIOS does it. */
                vga_port_write(0x3CE, 0x08);
                vga_port_write(0x3CF, bit);
                vga_port_write(0x3CE, 0x00);
                vga_port_write(0x3CF, (uint8_t)(al & 0x0F));
                vga_port_write(0x3CE, 0x01);
                vga_port_write(0x3CF, 0x0F);
                vga_mem_read(addr);
                vga_mem_write(addr, 0xFF);
                vga_port_write(0x3CE, 0x01);
                vga_port_write(0x3CF, 0x00);
                vga_port_write(0x3CE, 0x08);
                vga_port_write(0x3CF, 0xFF);
            }
        } else if (vga_mode() == 0x13) {
            mem_w8(memory, 0xA000, (uint16_t)(y * 320 + x), al);
        }
        break;
    }
    case READ_GRAPHICS_PIXEL:
        cpu->AX &= 0xFF00;
        break;
    case WRITE_CHAR_IN_TTY_MODE: {
        uint8_t pair[2] = {al, 0};
        con_print(memory, (const uint8_t *)pair);
        break;
    }
    case GET_VIDEO_MODE:
        cpu->AX = (uint16_t)((bda_r16(memory, BDA_VIDEO_COLS) << 8) | bda_r8(memory, BDA_VIDEO_MODE));
        cpu->BX &= 0x00FF;
        break;
    case SET_PALETTE_REGISTERS: {
        switch (al) {
        case 0x00: /* set one attribute controller register */
            vga_set_ac_palette((uint8_t)(cpu->BX & 0xFF), (uint8_t)(cpu->BX >> 8));
            break;
        case 0x01: /* border colour */
            vga_set_ac_palette(0x11, (uint8_t)(cpu->BX >> 8));
            break;
        case 0x02: { /* set all 16 registers plus overscan from ES:DX */
            for (uint8_t i = 0; i < 16; i++)
                vga_set_ac_palette(i, mem_r8(memory, cpu->ES, (uint16_t)(cpu->DX + i)));
            vga_set_ac_palette(0x11, mem_r8(memory, cpu->ES, (uint16_t)(cpu->DX + 16)));
            break;
        }
        case 0x07: /* read one attribute palette register */
            cpu->BX = (cpu->BX & 0xFF) | ((uint16_t)vga_get_ac_palette((uint8_t)cpu->BX) << 8);
            break;
        case 0x09: /* read palette and border */
            for (uint8_t i = 0; i < 16; i++)
                mem_w8(memory, cpu->ES, (uint16_t)(cpu->DX + i), vga_get_ac_palette(i));
            mem_w8(memory, cpu->ES, (uint16_t)(cpu->DX + 16), vga_get_ac_palette(0x11));
            break;
        case 0x10: /* set one DAC register */
            vga_set_dac((uint8_t)(cpu->BX & 0xFF), (uint8_t)(((cpu->DX >> 8) & 0x3F) << 2), (uint8_t)((cpu->CX >> 8) << 2), (uint8_t)((cpu->CX & 0x3F) << 2));
            break;
        case 0x12: { /* set a block of DAC registers from ES:DX */
            uint16_t first = cpu->BX;
            uint16_t count = cpu->CX;

            for (uint16_t i = 0; i < count; i++) {
                uint8_t r = mem_r8(memory, cpu->ES, (uint16_t)(cpu->DX + i * 3 + 0));
                uint8_t g = mem_r8(memory, cpu->ES, (uint16_t)(cpu->DX + i * 3 + 1));
                uint8_t b = mem_r8(memory, cpu->ES, (uint16_t)(cpu->DX + i * 3 + 2));

                vga_set_dac((uint8_t)(first + i), (uint8_t)((r & 0x3F) << 2), (uint8_t)((g & 0x3F) << 2), (uint8_t)((b & 0x3F) << 2));
            }
            break;
        }
        case 0x17: { /* read a block of DAC registers into ES:DX */
            uint16_t first = cpu->BX;
            uint16_t count = cpu->CX;

            for (uint16_t i = 0; i < count; i++) {
                uint8_t r, g, b;
                vga_get_dac((uint8_t)(first + i), &r, &g, &b);
                mem_w8(memory, cpu->ES, (uint16_t)(cpu->DX + i * 3 + 0), (uint8_t)(r >> 2));
                mem_w8(memory, cpu->ES, (uint16_t)(cpu->DX + i * 3 + 1), (uint8_t)(g >> 2));
                mem_w8(memory, cpu->ES, (uint16_t)(cpu->DX + i * 3 + 2), (uint8_t)(b >> 2));
            }
            break;
        }
        case 0x13: { /* BL=0 selects paging mode; BL=1 selects a colour page */
            uint8_t mode = vga_get_ac_palette(0x10);
            uint8_t select = vga_get_ac_palette(0x14);
            uint8_t page = (uint8_t)(cpu->BX >> 8);
            if ((cpu->BX & 0xFF) == 0)
                vga_set_ac_palette(0x10, (mode & 0x7F) | ((page & 1) << 7));
            else if ((cpu->BX & 0xFF) == 1)
                vga_set_ac_palette(0x14, (mode & 0x80) ? page & 0x0F : (select & 3) | ((page & 3) << 2));
            break;
        }
        case 0x18: /* set PEL mask */
            vga_port_write(0x3C6, (uint8_t)cpu->BX);
            break;
        case 0x19: /* read PEL mask */
            cpu->BX = (cpu->BX & 0xFF00) | vga_port_read(0x3C6);
            break;
        case 0x1A: { /* BL=paging mode, BH=current colour page */
            uint8_t mode = (vga_get_ac_palette(0x10) >> 7) & 1;
            uint8_t select = vga_get_ac_palette(0x14) & 0x0F;
            cpu->BX = mode | ((uint16_t)(mode ? select : select >> 2) << 8);
            break;
        }
        default:
            break;
        }
        break;
    }
    case CHAR_GENERATOR:
        /* AL=30h: return a font pointer; anything else is a no-op here. */
        if (al == 0x30) {
            cpu->ES = 0xF000;
            cpu->BP = 0x0000;
            cpu->CX = 16;
            cpu->DX = 24;
        }
        break;
    case VIDEO_SUBSYSTEM_CONFIGURATION: {
        uint8_t bl = (uint8_t)(cpu->BX & 0xFF);

        if (bl == 0x10) {
            /* EGA information: colour display, 256 KiB of video memory. */
            cpu->BX = 0x0003;
            cpu->CX = 0x0009;
        } else {
            cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | 0x12);
        }
        break;
    }
    case WRITE_STRING: {
        uint16_t count = cpu->CX;

        for (uint16_t i = 0; i < count; i++) {
            uint8_t step = (uint8_t)((al & 2) ? 2 : 1);
            uint8_t ch = mem_r8(memory, cpu->ES, (uint16_t)(cpu->BP + i * step));
            uint8_t pair[2] = {ch, 0};
            con_print(memory, (const uint8_t *)pair);
        }
        break;
    }
    case GET_SET_DISPLAY_COMBINATION_CODE:
        cpu->AX = 0x1A1A;
        cpu->BX = 0x0008; /* VGA with colour display */
        break;
    case VESA_BIOS_EXT_FUNC:
        cpu->AX = 0x0100; /* not a VESA BIOS */
        break;
    default:
        if (debug)
            log_line((const uint8_t *)"INT 10 svc %x NOT IMPLEMENTED", (unsigned int)service);
        break;
    }
}

/*
 *  handle_11h()
 *
 *  Equipment list.
 */
void handle_11h(CPU_T *cpu, Memory_T *memory) { cpu->AX = bda_r16(memory, BDA_EQUIPMENT); }

/*
 *  handle_12h()
 *
 *  Conventional memory size in KiB.
 */
void handle_12h(CPU_T *cpu, Memory_T *memory) { cpu->AX = bda_r16(memory, BDA_MEMSIZE); }

/*
 *  handle_13h()
 *
 *  A read-only, empty 1.44 MB FAT12 floppy on BIOS drive A:.
 *
 *  Standalone real-mode kernels read the boot record through INT 13h before
 *  starting their shell. Failing reset made their retry loops spin forever;
 *  succeeding reset without a valid BPB would leave them dividing by zero.
 *  Generate the empty image a byte at a time rather than allocate a whole floppy.
 *  DOS file services still use the host filesystem, independently of this.
 */
#define FLOPPY_SECTORS 2880u
#define FLOPPY_TRACK_SECTORS 18u
#define FLOPPY_PARAM_SEG 0xF000
#define FLOPPY_PARAM_OFF 0x1400 /* past the interrupt callback stubs */

static uint8_t disk_status;
static const uint8_t floppy_boot[512] = {[0] = 0xEB,   0x3C, 0x90,       't',  'h',  'e',        'M', ' ', ' ', ' ', ' ',         [11] = 0x00, 0x02, /* 512 bytes per sector */
                                         [13] = 1,     1,    0,          2,             /* one sector/cluster, one reserved sector, two FATs */
                                         [17] = 0xE0,  0,    0x40,       0x0B, 0xF0,    /* 224 root entries, 2880 sectors */
                                         [22] = 9,     0,    18,         0,    2,    0, /* FAT sectors, sectors/track, heads */
                                         [38] = 0x29,  1,    0,          0,    0,    [43] = 'T', 'H', 'E', 'M', ' ', 'E',         'M',         'P',  'T',
                                         'Y',          ' ',  [54] = 'F', 'A',  'T',  '1',        '2', ' ', ' ', ' ', [62] = 0xFA, 0xF4,        0xEB, 0xFD, /* blank disk: no bootable OS */
                                         [510] = 0x55, 0xAA};

void bios_disk_init(Memory_T *mem) {
    static const uint8_t parameters[11] = {0xDF, 0x02, 0x25, 0x02, 18, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08};
    disk_status = 0;
    bda_w8(mem, BDA_DISK_STATUS, 0);
    for (uint8_t i = 0; i < sizeof(parameters); i++)
        mem_w8(mem, FLOPPY_PARAM_SEG, (uint16_t)(FLOPPY_PARAM_OFF + i), parameters[i]);
    mem_w16(mem, 0, 0x1E * 4, FLOPPY_PARAM_OFF);
    mem_w16(mem, 0, 0x1E * 4 + 2, FLOPPY_PARAM_SEG);
}

static void disk_result(CPU_T *cpu, Memory_T *mem, uint8_t status) {
    disk_status = status;
    bda_w8(mem, BDA_DISK_STATUS, status);
    cpu->AX = (uint16_t)(((uint16_t)status << 8) | (cpu->AX & 0xFF));
    set_flag(cpu, CF, status != 0);
}

static uint8_t floppy_byte(uint32_t sector, uint16_t off) {
    if (!sector)
        return floppy_boot[off];
    /* The reserved entries in the first sector of each FAT. */
    if ((sector == 1 || sector == 10) && off < 3)
        return off ? 0xFF : 0xF0;
    return 0;
}

void handle_13h(CPU_T *cpu, Memory_T *memory) {
    uint8_t service = (uint8_t)(cpu->AX >> 8);
    uint8_t count = (uint8_t)cpu->AX;

    if ((cpu->DX & 0xFF) != 0) {
        disk_result(cpu, memory, 0x80); /* no such drive */
        return;
    }

    switch (service) {
    case 0x00: /* reset the controller */
        disk_result(cpu, memory, 0);
        break;
    case 0x01: /* status of the last operation */
        disk_result(cpu, memory, disk_status);
        break;
    case 0x02:   /* read CHS sectors to ES:BX */
    case 0x04: { /* verify sectors */
        uint16_t cylinder = (uint16_t)((cpu->CX >> 8) | ((cpu->CX & 0xC0) << 2));
        uint8_t sector = (uint8_t)(cpu->CX & 0x3F);
        uint8_t head = (uint8_t)(cpu->DX >> 8);
        uint32_t first = ((uint32_t)cylinder * 2 + head) * FLOPPY_TRACK_SECTORS + sector - 1;

        if (!count || count > 127 || cylinder >= 80 || head >= 2 || !sector || sector > FLOPPY_TRACK_SECTORS || first + count > FLOPPY_SECTORS) {
            cpu->AX &= 0xFF00;
            disk_result(cpu, memory, 0x04); /* sector not found */
            break;
        }
        if (service == 0x02) {
            uint32_t size = (uint32_t)count * 512;
            uint32_t dma = (((uint32_t)cpu->ES << 4) + cpu->BX) & 0xFFFF;
            if (dma + size > 0x10000u || (uint32_t)cpu->BX + size > 0x10000u) {
                cpu->AX &= 0xFF00;
                disk_result(cpu, memory, 0x09); /* DMA crosses a 64 KiB boundary */
                break;
            }
            for (uint8_t n = 0; n < count; n++)
                for (uint16_t i = 0; i < 512; i++)
                    mem_w8(memory, cpu->ES, (uint16_t)(cpu->BX + (uint32_t)n * 512 + i), floppy_byte(first + n, i));
        }
        disk_result(cpu, memory, 0);
        break;
    }
    case 0x03: /* writes are confined to DOS host-file services */
    case 0x05: /* format track */
        cpu->AX &= 0xFF00;
        disk_result(cpu, memory, 0x03); /* write protected */
        break;
    case 0x08: /* geometry and diskette parameter table */
        cpu->AX = 0;
        cpu->BX = 4; /* 1.44 MB drive */
        cpu->CX = (79 << 8) | FLOPPY_TRACK_SECTORS;
        cpu->DX = 0x0101; /* maximum head 1, one drive */
        cpu->ES = FLOPPY_PARAM_SEG;
        cpu->DI = FLOPPY_PARAM_OFF;
        disk_result(cpu, memory, 0);
        break;
    case 0x15: /* diskette without a change-line signal */
        disk_result(cpu, memory, 0);
        cpu->AX = 0x0100;
        break;
    default:
        disk_result(cpu, memory, 0x01); /* unsupported function */
        break;
    }
}

void handle_14h(CPU_T *cpu, Memory_T *memory) { cpu->AX = 0; }

/*
 *  handle_15h()
 *
 *  Miscellaneous system services.
 */
void handle_15h(CPU_T *cpu, Memory_T *memory) {
    uint8_t ah = (uint8_t)(cpu->AX >> 8);

    switch (ah) {
    case 0x86: { /* wait CX:DX microseconds */
        uint32_t us = ((uint32_t)cpu->CX << 16) | cpu->DX;
        sleep_ms(us / 1000);
        set_flag(cpu, CF, 0);
        break;
    }
    case 0x88: /* extended memory size */
        cpu->AX = 0;
        break;
    default:
        cpu->AX = (uint16_t)((0x86 << 8) | (cpu->AX & 0xFF));
        set_flag(cpu, CF, 1);
        break;
    }
}

/*
 *  handle_16h()
 *
 *  Keyboard services.
 */
void handle_16h(CPU_T *cpu, Memory_T *memory) {
    SRV_16H service = cpu->AX >> 8;
    uint16_t entry = 0;

    switch (service) {
    case READ_CHAR:
    case READ_CHAR_EXTENDED:
        if (!kbd_buffer_get(memory, &entry)) {
            bios_retry_instruction(cpu, memory);
            return;
        }
        cpu->AX = entry;
        break;
    case READ_INPUT_STATUS:
    case READ_INPUT_STATUS_EXTENDED:
        if (!kbd_buffer_peek(memory, &entry)) {
            set_flag(cpu, ZF, 1);
            break;
        }
        cpu->AX = entry;
        set_flag(cpu, ZF, 0);
        break;
    case READ_KEYBOARD_SHIFT_STATUS:
    case READ_KEYBOARD_SHIFT_STATUS_EXTENDED:
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | bda_r8(memory, BDA_SHIFT1));
        break;
    case STORE_KEYSTROKE_IN_KEYBOARD_BUF:
        kbd_buffer_put(memory, cpu->CX);
        cpu->AX &= 0xFF00;
        break;
    default:
        break;
    }
}

/* Keyboard buffer access for the DOS console services. */
uint8_t bios_key_ready(Memory_T *mem) {
    uint16_t entry;

    return kbd_buffer_peek(mem, &entry);
}

uint8_t bios_key_read(Memory_T *mem, uint16_t *entry) { return kbd_buffer_get(mem, entry); }

void handle_17h(CPU_T *cpu, Memory_T *memory) { cpu->AX = (uint16_t)((cpu->AX & 0x00FF) | 0x9000); }

void handle_18h(CPU_T *cpu, Memory_T *memory) { cpu_halt(); }

void handle_19h(CPU_T *cpu, Memory_T *memory) { cpu_halt(); }

/*
 *  handle_1Ah()
 *
 *  Real time clock services.
 */
void handle_1Ah(CPU_T *cpu, Memory_T *memory) {
    uint8_t ah = (uint8_t)(cpu->AX >> 8);

    switch (ah) {
    case 0x00: { /* read the tick counter */
        uint32_t ticks = hw_bios_ticks();

        cpu->CX = (uint16_t)(ticks >> 16);
        cpu->DX = (uint16_t)(ticks & 0xFFFF);
        cpu->AX &= 0x00FF;
        break;
    }
    case 0x01: /* set the tick counter: the emulated clock is free running */
        break;
    case 0x02: { /* read the real time clock */
        RTC_T rtc = {0};

        read_rtc(&rtc);
        cpu->CX = (uint16_t)((((rtc.hours / 10) << 4 | (rtc.hours % 10)) << 8) | ((rtc.minutes / 10) << 4 | (rtc.minutes % 10)));
        cpu->DX = (uint16_t)(((rtc.seconds / 10) << 4 | (rtc.seconds % 10)) << 8);
        set_flag(cpu, CF, 0);
        break;
    }
    case 0x04: { /* read the date */
        RTC_T rtc = {0};

        read_rtc(&rtc);
        cpu->CX = (uint16_t)((((rtc.year / 1000) % 10) << 12) | (((rtc.year / 100) % 10) << 8) | (((rtc.year / 10) % 10) << 4) | (rtc.year % 10));
        cpu->DX = (uint16_t)((((rtc.month / 10) << 4 | (rtc.month % 10)) << 8) | ((rtc.day / 10) << 4 | (rtc.day % 10)));
        set_flag(cpu, CF, 0);
        break;
    }
    default:
        set_flag(cpu, CF, 1);
        break;
    }
}

void handle_1Bh(CPU_T *cpu, Memory_T *memory) {}

/*
 *  handle_1Ch()
 *
 *  The user timer tick. Programs hook this vector; the default does nothing.
 */
void handle_1Ch(CPU_T *cpu, Memory_T *memory) {}

void handle_1Dh(CPU_T *cpu, Memory_T *memory) {}
void handle_1Eh(CPU_T *cpu, Memory_T *memory) {}
void handle_1Fh(CPU_T *cpu, Memory_T *memory) {}
void handle_41h(CPU_T *cpu, Memory_T *memory) {}
void handle_46h(CPU_T *cpu, Memory_T *memory) {}
void handle_4Ah(CPU_T *cpu, Memory_T *memory) {}
