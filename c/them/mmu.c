#include "mmu.h"
#include "gfx.h"
#include "vga.h"

/*
 *  mmu.c
 *
 *  The guest's one megabyte of address space. Everything is plain RAM
 *  except the 0xA0000 video window, which in the planar and unchained
 *  256-colour modes is not memory at all but the adapter's write logic.
 *
 *  krusty@vxn.dev / Nov 5, 2025
 */

uint8_t init_ram(Memory_T *mem) {
    for (uint32_t i = 0; i < sizeof(mem->bytes); i++) {
        mem->bytes[i] = 0x00;
    }

    mem->start = 0x0000;
    /* First paragraph a DOS allocation may hand out: past the PSP and the
     * 64 KiB the .COM image owns (0x1000 + 0x1000 paragraphs). */
    mem->next_free_seg = 0x2000;

    return 0;
}

uint8_t mmu_read(Memory_T *mmu, uint32_t addr) {
    if (addr >= sizeof(mmu->bytes)) {
        return 0xFF;
    }

    if (addr >= 0xA0000 && addr <= 0xAFFFF && vga_mapped()) {
        return vga_mem_read(addr);
    }

    return mmu->bytes[addr];
}

uint8_t mmu_write(Memory_T *mmu, uint32_t addr, uint8_t value) {
    if (addr >= sizeof(mmu->bytes)) {
        return 1;
    }

    if (addr >= 0xA0000 && addr <= 0xAFFFF) {
        if (vga_mapped()) {
            vga_mem_write(addr, value);
            return 0;
        }
        mmu->bytes[addr] = value;
        gfx_mark_dirty();
        return 0;
    }

    mmu->bytes[addr] = value;

    if (addr >= 0xB8000 && addr < 0xB8000 + 2 * 80 * 25) {
        gfx_mark_dirty();
    }

    return 0;
}
