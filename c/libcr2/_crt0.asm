section .text
global _start
extern main

_start:
    ; rdi = argc
    mov rdi, [rsp]
    ; rsi = &argv[0]
    lea rsi, [rsp + 8]

    ; rax = argc
    mov rax, rdi
    add rax, 1
    shl rax, 3        ; multiply by 8
    add rax, rsi      ; rax = argv + (argc+1)
    mov rdx, rax      ; rdx = envp

    ; call main
    call main

    ; syscall exit(rax)
    ;mov rdi, rax
    ;mov rax, 60
    ;syscall

    mov rdi, rax
    mov rax, 0
    int 0x7f

    hlt

end:
	jmp end
