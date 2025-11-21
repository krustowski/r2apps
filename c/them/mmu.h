#ifndef _THEM_MMU_INCLUDED_
#define _THEM_MMU_INCLUDED_

/*
 *  mmu.h
 *
 *  krusty@vxn.dev / Nov 5, 2025
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t bytes[1 << 20];
    uint16_t start;
} __attribute__((packed)) Memory_T;

uint8_t mmu_read(Memory_T *mem, uint32_t addr);
uint8_t mmu_write(Memory_T *mem, uint32_t addr, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
