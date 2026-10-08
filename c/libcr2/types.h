#ifndef _R2_TYPES_INCLUDED_
#define _R2_TYPES_INCLUDED_

/*
 *  types.h
 *
 *  Basic type definitions to work with the r2 kernel project.
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

/*
 *  R2_LIBC
 *
 *  Set when r2's C library (c/tcc/libc) is on the include path, as it is for
 *  every program tcc builds on r2.  Its headers own the C names --- the integer
 *  types, exit, chdir, read, write, close, printf, strlen, memcpy --- and the
 *  libcr2 headers then take those from it and give their own calls an r2_ or
 *  tcp_ name instead.  Without it, as the apps here are built (-nostdinc),
 *  everything stays as it was.
 */
#if defined(__has_include) && !defined(R2_LIBC)
#if __has_include(<sys/r2libc.h>)
#include <sys/r2libc.h>
#endif
#endif

#ifdef R2_LIBC

#include <stdint.h>

#else

#ifdef __cplusplus
extern "C" {
#endif

typedef char 	int8_t;
typedef short 	int16_t;
typedef int  	int32_t;
typedef long 	int64_t;

typedef unsigned char 	uint8_t;
typedef unsigned short 	uint16_t;
typedef unsigned int 	uint32_t;
typedef unsigned long 	uint64_t;

#ifdef __cplusplus
}
#endif

#endif

#endif

