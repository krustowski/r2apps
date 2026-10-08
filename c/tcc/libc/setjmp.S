/*
 *  setjmp / longjmp for x86-64 SysV: the callee-saved registers, the stack
 *  pointer the caller will have, and where to return to.
 *
 *  jmp_buf: rbx, rbp, r12, r13, r14, r15, rsp, rip
 */
    .text
    .globl setjmp
setjmp:
    mov %rbx, 0(%rdi)
    mov %rbp, 8(%rdi)
    mov %r12, 16(%rdi)
    mov %r13, 24(%rdi)
    mov %r14, 32(%rdi)
    mov %r15, 40(%rdi)
    lea 8(%rsp), %rdx
    mov %rdx, 48(%rdi)
    mov (%rsp), %rdx
    mov %rdx, 56(%rdi)
    xor %eax, %eax
    ret

    .globl longjmp
longjmp:
    mov %esi, %eax
    test %eax, %eax
    jnz 1f
    inc %eax            /* longjmp(env, 0) returns 1 */
1:
    mov 0(%rdi), %rbx
    mov 8(%rdi), %rbp
    mov 16(%rdi), %r12
    mov 24(%rdi), %r13
    mov 32(%rdi), %r14
    mov 40(%rdi), %r15
    mov 48(%rdi), %rsp
    jmp *56(%rdi)
