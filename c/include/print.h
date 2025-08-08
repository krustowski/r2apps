#ifndef _R2_PRINT_INCLUDED_
#define _R2_PRINT_INCLUDED_

/*
 *  print.h
 *
 *  Print-related definitions, declarations and constants for the r2 kernel project.
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

void u32_to_str(uint32_t value, uint8_t *buffer);

#ifdef __cplusplus
}
#endif

#endif



