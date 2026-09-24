/*
 *  string.h --- the part of <string.h> that BearSSL and the TLS glue use.
 *
 *  The C side of the browser is built with -nostdinc: GCC's own freestanding
 *  headers supply <stddef.h>, <stdint.h> and <limits.h>, and this supplies the
 *  rest.  The functions themselves come from libc++r2 (src/libc.cpp), which
 *  gives them C linkage and the standard signatures.
 */
#ifndef WEB_LIBC_STRING_H
#define WEB_LIBC_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
