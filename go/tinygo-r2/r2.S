/*
 *  r2.S
 *
 *  Program entry point and the syscall trampoline for the rou2exOS target.
 *
 *  The kernel loads an ELF at 0x600000 and jumps straight to e_entry with a
 *  SysV argv frame on a stack of its own choosing, so everything a C runtime
 *  would get from crt0 has to happen here.
 */

.section .text._start
.global _start
_start:
    /*
     *  The kernel builds argc/argv on the stack it hands us:
     *    [rsp] = argc, [rsp+8] = argv[0], ...
     *  Read it before switching stacks --- that frame lives on the kernel's
     *  stack and is gone the moment we move off it.
     */
    movq    (%rsp), %rax
    movq    %rax, r2_argc(%rip)
    leaq    8(%rsp), %rax
    movq    %rax, r2_argv(%rip)

    /*
     *  Switch to the stack the linker script reserved inside our own 2 MiB
     *  frame.  This is not an optimisation: the garbage collector scans from
     *  the live stack pointer up to _stack_top, so the stack we run on has to
     *  be the one the linker script describes.  The kernel picks our initial
     *  stack from a per-slot table, so its address is not knowable at link
     *  time and could not be described here.
     */
    movq    $_stack_top, %rsp
    andq    $-16, %rsp
    xorq    %rbp, %rbp

    /*
     *  16-byte alignment: SysV wants rsp+8 aligned on entry, and `call`
     *  pushes the 8 bytes that gets us there.
     */
    call    main

    /*
     *  main() returned.  Ask the kernel to reap us; 0x00 never returns.
     *  RAX = syscall number, RDI = arg1 (ignored by exit), RSI = arg2 = code.
     */
    xorl    %edi, %edi
    xorl    %esi, %esi
    xorl    %eax, %eax
    int     $0x7f
1:  jmp     1b


/*
 *  uintptr r2_syscall(uintptr num, uintptr a1, uintptr a2)
 *
 *  Go hands us num/a1/a2 in rdi/rsi/rdx and wants the result in rax; the
 *  kernel wants the number in rax and the arguments in rdi/rsi.
 *
 *  The handler preserves every general-purpose register except rcx (which it
 *  overwrites with rdx) and r9 (which it uses to carry the return value back
 *  across its own register restore).  Both are caller-saved under SysV, so
 *  there is nothing to do about them here.
 */
.section .text.r2_syscall
.global r2_syscall
r2_syscall:
    movq    %rdi, %rax
    movq    %rsi, %rdi
    movq    %rdx, %rsi
    int     $0x7f
    ret


/*
 *  argc/argv, stashed by _start for the runtime to hand to the program.
 */
.section .bss
.align 8
.global r2_argc
r2_argc:
    .space 8
.global r2_argv
r2_argv:
    .space 8


/*
 *  Accessors for the stashed argv frame.  Go could alias the symbols above
 *  directly, but a two-instruction getter costs nothing and keeps the
 *  program's view of its arguments independent of how the linker lays .bss out.
 */
.section .text.r2_get_argc
.global r2_get_argc
r2_get_argc:
    movq    r2_argc(%rip), %rax
    ret

.section .text.r2_get_argv
.global r2_get_argv
r2_get_argv:
    movq    r2_argv(%rip), %rax
    ret
