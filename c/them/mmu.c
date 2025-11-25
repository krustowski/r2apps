#include "mmu.h"
#include "gfx.h"

uint8_t mmu_read(Memory_T *mmu, uint32_t addr) { return mmu->bytes[addr - mmu->start]; }

uint8_t mmu_write(Memory_T *mmu, uint32_t addr, uint8_t value) {
    mmu->bytes[addr - mmu->start] = value;

    if (addr >= 0xB8000 && addr <= 0xB8000 + 2 * 80 * 24 && !mmu->render_lock) {
        render_text_mode(mmu);
    }

    return 0;
}
