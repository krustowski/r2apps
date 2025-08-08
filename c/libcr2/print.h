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

/*
 *  uint32_t strlen() prototype
 *
 *  A macro-like function to count the given uint8_t array size. The string should be null-ended.
 */
uint32_t strlen(const uint8_t *str);

/*
 *  void u32_to_str() prototype
 *
 *  A simple helper function to convert unsigned 32bit integer into a string representation in
 *  provided <buffer>.
 */
void u32_to_str(uint32_t value, uint8_t *buffer);

#ifdef __cplusplus
}
#endif

#endif



