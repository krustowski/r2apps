section .bss
align 16
r2_stack: resb 1024 * 1536   ; 1.5 MB private stack
r2_stack_top:

section .text

extern main

global _start
_start:
    ; The kernel pushes an SysV argv frame on the initial stack:
    ;   [rsp+0] = argc, [rsp+8] = argv[0], [rsp+16] = argv[1], ...
    ; Read them before switching to our private stack.
    mov rdi, [rsp]      ; argc
    lea rsi, [rsp+8]    ; argv

    ; Switch to our own stack (kernel-provided stack is too small).
    lea rsp, [r2_stack_top]
    and rsp, -16               ; ensure 16-byte alignment (SysV ABI)

    xor rdx, rdx               ; envp = NULL

    call main

    ; syscall exit(rax)
    mov rsi, rax
    mov rdi, 0x00
    mov rdx, 0x00
    int 0x7f

.hang:
    jmp .hang       ; hlt is ring-0 only; spin if exit syscall returns
