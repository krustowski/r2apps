; System V AMD64 callee-saved registers, stack and return address.
bits 64
section .text
global r2js_setjmp
r2js_setjmp:
    mov [rdi], rbx
    mov [rdi+8], rbp
    mov [rdi+16], r12
    mov [rdi+24], r13
    mov [rdi+32], r14
    mov [rdi+40], r15
    lea rax, [rsp+8]
    mov [rdi+48], rax
    mov rax, [rsp]
    mov [rdi+56], rax
    xor eax, eax
    ret
global r2js_longjmp
r2js_longjmp:
    mov eax, esi
    test eax, eax
    jnz .nonzero
    inc eax
.nonzero:
    mov rbx, [rdi]
    mov rbp, [rdi+8]
    mov r12, [rdi+16]
    mov r13, [rdi+24]
    mov r14, [rdi+32]
    mov r15, [rdi+40]
    mov rsp, [rdi+48]
    jmp [rdi+56]
section .note.GNU-stack noalloc noexec nowrite progbits
