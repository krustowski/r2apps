/*
 *  Start-up code for C programs on r2, as libcr2/_crt0.asm, but leaving
 *  through exit() so that stdio gets flushed.
 *
 *  Everything a process has --- code, data, bss and this stack --- shares one
 *  2 MiB mapping at 0x600000, so the stack is kept modest.  Assemble with
 *  -DR2_STACK_KB=<n> for another size.
 */
#ifndef R2_STACK_KB
#define R2_STACK_KB 256
#endif

    .text
    .globl _start
_start:
    /* the kernel's SysV frame: [rsp] = argc, [rsp+8] = argv[0], ... */
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    /* the kernel's stack is too small to run on */
    lea r2_stack_top(%rip), %rsp
    and $-16, %rsp
    xor %edx, %edx
    call main
    mov %eax, %edi
    call exit
1:
    jmp 1b

    .bss
    .align 16
r2_stack:
    .skip R2_STACK_KB * 1024
r2_stack_top:
