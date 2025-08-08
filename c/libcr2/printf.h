#ifndef _R2_PRINTF_INCLUDED_
#define _R2_PRINTF_INCLUDED_ 

/*
 *  printf.h
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "args.h"
#include "syscall.h"

static void print_string(const uint8_t *s);
static void print_decimal(int val);
static void print_unsigned(unsigned int val);
static void print_hex(unsigned int val);
void printf(const uint8_t *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif

