#ifndef _THEM_INT_INCLUDED_
#define _THEM_INT_INCLUDED_ 

/*
 *  cpu.h
 *
 *  krusty@vxn.dev / Nov 4, 2025
 */

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

void int21h(CPU_T *cpu, uint8_t *memory);

#ifdef __cplusplus
}
#endif

#endif
