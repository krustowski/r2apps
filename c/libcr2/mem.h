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

/*
 *  uint32_t memcmp() prototype
 *
 *  Custom memory comparison procedure.
 */
uint32_t memcmp(const uint8_t *s1, const uint8_t *s2, uint32_t len);

/*
 *  void *memcpy() prototype
 *
 *  Custom memory copying procedure.
 */
void *memcpy(void *dest, const void *src, uint16_t n);

#ifdef __cplusplus
}
#endif

#endif


