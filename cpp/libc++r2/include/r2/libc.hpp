#ifndef _R2CXX_LIBC_HPP_
#define _R2CXX_LIBC_HPP_

/*
 *  libc.hpp — the C functions the compiler assumes exist.
 *
 *  GCC emits calls to memcpy, memset, memmove and memcmp from ordinary C++ (a
 *  struct assignment, a zero-initialised array, a vector reallocation) whatever
 *  -ffreestanding says, so a freestanding runtime has to provide them.  The
 *  rest are here because they are too useful to leave out.
 *
 *  These have C linkage and the standard signatures, so C code built against
 *  c/libcr2 links against them too.  Note that libcr2's own memcpy takes a
 *  uint16_t length and silently truncates anything over 65535 bytes; if a
 *  program links both libraries, put libc++r2.a first on the link line so this
 *  one wins.
 */

#include "types.hpp"

extern "C" {

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

} // extern "C"

#endif
