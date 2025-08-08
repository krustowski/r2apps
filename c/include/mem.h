#ifndef _R2_MEM_INCLUDED_
#define _R2_MEM_INCLUDED_

/*
 *  mem.h
 *
 *  Memory-related definitions, declarations and constants for the r2 kernel project.
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

void *memcpy(void *dest, const void *src, uint16_t n);

#ifdef __cplusplus
}
#endif

#endif


