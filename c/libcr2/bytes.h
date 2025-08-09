#ifndef _R2_BYTES_INCLUDED_
#define _R2_BYTES_INCLUDED_

/*
 *  bytes.h
 *
 *  Byte processing-related definitions, declarations and constants for the r2 kernel project.
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

uint16_t swap16(uint16_t x) { return __builtin_bswap16(x); }
uint32_t swap32(uint32_t x) { return __builtin_bswap32(x); }
uint64_t swap64(uint64_t x) { return __builtin_bswap64(x); }

#ifdef __cplusplus
}
#endif

#endif



