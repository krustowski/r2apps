#include "mmu.h"

uint8_t mmu_read(Memory_T *mmu, uint32_t addr) { return mmu->bytes[addr - mmu->start]; }

uint8_t mmu_write(Memory_T *mmu, uint32_t addr, uint8_t value) {
    mmu->bytes[addr - mmu->start] = value;

    return 0;
}
