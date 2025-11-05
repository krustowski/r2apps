#ifndef _THEM_MMU_INCLUDED_
#define _THEM_MMU_INCLUDED_ 

/*
 *  mmu.h
 *
 *  krusty@vxn.dev / Nov 5, 2025
 */

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	uint8_t bytes[1 << 20];
	uint16_t start;
} __attribute__((packed)) Memory_T;

#ifdef __cplusplus
}
#endif

#endif

