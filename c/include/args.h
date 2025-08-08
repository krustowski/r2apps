#ifndef _R2_ARGS_INCLUDED_
#define _R2_ARGS_INCLUDED_

/*
 *  args.h
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

#ifdef __cplusplus
}
#endif

#endif

