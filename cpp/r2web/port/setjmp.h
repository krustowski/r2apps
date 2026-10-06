#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned long jmp_buf[8];
int r2js_setjmp(void *) __attribute__((returns_twice));
void r2js_longjmp(void *, int) __attribute__((noreturn));
#define setjmp r2js_setjmp
#define longjmp r2js_longjmp
#ifdef __cplusplus
}
#endif
