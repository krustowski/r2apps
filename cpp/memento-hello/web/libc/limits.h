/*
 *  limits.h --- for the freestanding C build of BearSSL.
 *
 *  GCC's own <limits.h> reaches for the system's with #include_next, which
 *  -nostdinc takes away, so the few limits that matter on x86-64 are here.
 *  BearSSL itself only asks for ULONG_MAX (to tell a 64-bit long).
 */
#ifndef WEB_LIBC_LIMITS_H
#define WEB_LIBC_LIMITS_H

#define CHAR_BIT 8
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define SHRT_MAX 32767
#define USHRT_MAX 65535
#define INT_MAX 2147483647
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX 4294967295U
#define LONG_MAX 9223372036854775807L
#define LONG_MIN (-LONG_MAX - 1L)
#define ULONG_MAX 18446744073709551615UL

#endif
