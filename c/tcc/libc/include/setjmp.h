#ifndef _SETJMP_H
#define _SETJMP_H

/* rbx, rbp, r12-r15, rsp and the return address. */
typedef long jmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#define _setjmp setjmp
#define _longjmp longjmp

#endif
