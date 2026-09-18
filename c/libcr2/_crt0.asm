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

    ; syscall exit(code) --- RAX = syscall No., RDI = arg1, RSI = arg2.
    ;
    ; main's return value arrives in RAX, which is exactly where the kernel
    ; looks for the syscall number, so it has to be moved out of the way
    ; first: a non-zero exit code would otherwise invoke the syscall of that
    ; number instead of exit, the process would spin in .hang below, and a
    ; shell waiting on it (fg) would never be woken again.
    mov rsi, rax               ; arg2 = exit code
    mov rdi, 0x00              ; arg1 = pid (unused)
    xor eax, eax               ; syscall No. 0x00 = exit
    int 0x7f

.hang:
    jmp .hang       ; hlt is ring-0 only; spin if exit syscall returns
