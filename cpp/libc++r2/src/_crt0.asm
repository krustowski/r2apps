;
;  _crt0.asm --- C runtime startup for C++ programs targeting the r2 kernel.
;
;  The kernel enters an ELF at e_entry with a SysV-style argv frame on the
;  stack it hands the process:
;
;      [rsp + 0]  = argc
;      [rsp + 8]  = argv[0]
;      [rsp + 16] = argv[1] ...
;
;  Two things happen here before any C++ runs:
;
;    1. The argv frame is read, then the process switches to a private stack.
;       The kernel-provided stacks for slots 0-7 live in the identity-mapped
;       region above 0x800_000 that every process can see, 128 KiB apart; ours
;       is BSS inside the process image, so it is private, sized by the program
;       that owns it, and inside the 0x600_000-0xA00_000 window that every
;       pointer-taking syscall range-checks against.
;
;    2. Control passes to __r2_start (src/start.cpp), which brings up the heap,
;       runs .init_array, calls main, and exits.
;
;  Override the stack size at assembly time:
;
;      nasm -f elf64 -DR2_STACK_BYTES=262144 _crt0.asm -o _crt0.o
;

%ifndef R2_STACK_BYTES
%define R2_STACK_BYTES (512 * 1024)
%endif

section .bss
align 16

global __r2_stack_bottom
global __r2_stack_top

__r2_stack_bottom:
    resb R2_STACK_BYTES
__r2_stack_top:

section .text

extern __r2_start

global _start
_start:
    ; Read the argv frame while the kernel's stack is still the current one.
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv

    mov rsp, __r2_stack_top
    and rsp, -16                ; SysV ABI: 16-byte aligned at the call site
    xor rbp, rbp                ; end the frame-pointer chain here

    call __r2_start

    ; __r2_start does not return.  If it ever did, exit(0) by hand rather than
    ; falling into whatever follows in .text.
    ;
    ; The syscall number goes in both RAX and RDX --- see r2::raw_syscall in
    ; include/r2/syscall.hpp for why.
    xor edi, edi                ; arg1 = pid
    xor esi, esi                ; arg2 = exit code
    xor edx, edx                ; syscall 0x00 = exit
    xor eax, eax
    int 0x7f

.hang:
    jmp .hang                   ; hlt is ring-0 only; spin if exit returns
