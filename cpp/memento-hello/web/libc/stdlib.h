/*
 *  stdlib.h --- what stb_image (stb_image.c) asks of <stdlib.h>: abs().  Its
 *  allocation goes through STBI_MALLOC and friends, set to the engine's
 *  allocator there.
 */
#ifndef WEB_LIBC_STDLIB_H
#define WEB_LIBC_STDLIB_H

#include <stddef.h>

static inline int abs(int v) { return v < 0 ? -v : v; }

#endif
