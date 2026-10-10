/*
 *  stdlib.h --- what QuickJS asks of <stdlib.h> on r2.
 *
 *  The engine's own memory goes through jsr2::Heap (JSMallocFunctions); the
 *  plain malloc/realloc/free here are the program's (libc++r2's arena) and
 *  serve only the few temporary buffers QuickJS makes without a runtime.
 */
#ifndef JSR2_PORT_STDLIB_H
#define JSR2_PORT_STDLIB_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void abort(void) __attribute__((noreturn));
#define exit(code) abort()
static inline int abs(int v) { return v < 0 ? -v : v; }
static inline long labs(long v) { return v < 0 ? -v : v; }
static inline long long llabs(long long v) { return v < 0 ? -v : v; }
int atoi(const char *);
#define alloca(n) __builtin_alloca(n)
#ifdef __cplusplus
}
#endif
#endif
