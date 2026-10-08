#ifndef _R2_STRING_INCLUDED_
#define _R2_STRING_INCLUDED_

/*
 *  string.h
 *
 *  String processing-related definitions, declarations and constants for the r2 kernel project.
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

#ifdef R2_LIBC
/*
 *  The C library's string.h, which this one hides from anything built with
 *  libcr2 on its -I path.  Installed as <r2/string.h> among the C library's
 *  headers (tcc's sysroot), that one is ../string.h.
 */
#if __has_include("../string.h")
#include "../string.h"
#else
#include_next <string.h>
#endif
#else

/*
 *  uint32_t strlen() prototype
 *
 *  A macro-like function to count the given uint8_t array size. The string should be null-ended.
 */
uint32_t strlen(const uint8_t *str);

#endif

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



