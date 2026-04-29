section .bss
align 16
r2_stack: resb 1024 * 1536   ; 1.5 MB private stack
r2_stack_top:

section .text

extern main

global _start
_start:
    ; Switch to our own stack before touching any C code.
    ; The kernel-provided stack is too small for deep C++ call chains.
    lea rsp, [r2_stack_top]
    and rsp, -16               ; ensure 16-byte alignment (SysV ABI)

    ; Preserve argc/argv/envp from the kernel stack — but we've already
    ; moved RSP, so we can't read [old_rsp] any more.  For this bare-metal
    ; app we don't need them; pass zeros so main(void) works cleanly.
    xor rdi, rdi
    xor rsi, rsi
    xor rdx, rdx

    call main

    ; syscall exit(rax)
    mov rsi, rax
    mov rdi, 0x00
    mov rdx, 0x00
    int 0x7f

.hang:
    jmp .hang       ; hlt is ring-0 only; spin if exit syscall returns
