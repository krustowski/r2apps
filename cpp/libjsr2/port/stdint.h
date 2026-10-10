/*
 *  stdint.h --- GCC's, unless libc++r2's <r2/types.hpp> came first: that
 *  spells int8_t as char (as c/libcr2 does), which GCC's signed char would
 *  contradict.  Then only the limit and constant macros are added here.
 */
#ifndef JSR2_PORT_STDINT_H
#define JSR2_PORT_STDINT_H
#if defined(__cplusplus) && defined(_R2CXX_TYPES_HPP_)
#define INT8_MIN (-128)
#define INT8_MAX 127
#define UINT8_MAX 255
#define INT16_MIN (-32768)
#define INT16_MAX 32767
#define UINT16_MAX 65535
#define INT32_MIN (-INT32_MAX - 1)
#define INT32_MAX __INT32_MAX__
#define UINT32_MAX __UINT32_MAX__
#define INT64_MIN (-INT64_MAX - 1)
#define INT64_MAX __INT64_MAX__
#define UINT64_MAX __UINT64_MAX__
#define INTPTR_MAX __INTPTR_MAX__
#define UINTPTR_MAX __UINTPTR_MAX__
#define SIZE_MAX __SIZE_MAX__
#define INT32_C(c) c
#define UINT32_C(c) c##U
#define INT64_C(c) c##L
#define UINT64_C(c) c##UL
#else
#include_next <stdint.h>
#endif
#endif
