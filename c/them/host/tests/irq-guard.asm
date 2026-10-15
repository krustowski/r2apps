bits 16
cpu 286
org 100h
mov dx, handler
mov ax, 2508h
int 21h
mov dx, chained
mov ax, 2560h
int 21h
mov al, 36h
out 43h, al
mov ax, 8522
out 40h, al
mov al, ah
out 40h, al
restart:
mov al, 0fch
out 21h, al
sti
wait_irq:
cmp word [reentered], 0
jne fail
cmp word [rounds], 16
jb wait_irq
cli
cmp byte [far_return], 0
jne passed
; RETF 2 consumes a hardware frame too; it must release the hold.
mov byte [far_return], 1
mov word [rounds], 0
jmp restart
passed:
mov dx, pass_msg
mov ah, 9
int 21h
mov ax, 4c00h
int 21h
fail:
cli
mov dx, fail_msg
mov ah, 9
int 21h
mov ax, 4c01h
int 21h
handler:
push ax
push cx
cmp byte [cs:active], 0
je outer
inc word [cs:reentered]
jmp done
outer:
inc byte [cs:active]
mov al, 20h
out 20h, al
; Like HAUNTED: save the original stack in fixed variables and call a
; callback on a private stack. Reentry would overwrite those variables.
mov [cs:saved_ss], ss
mov [cs:saved_sp], sp
mov ax, 1200h
mov ss, ax
mov sp, 1000h
sti
int 60h
mov cx, 30000
work:
loop work
cli
mov ss, [cs:saved_ss]
mov sp, [cs:saved_sp]
dec byte [cs:active]
inc word [cs:rounds]
cmp word [cs:rounds], 16
jb done
; End the timer burst so the main loop can inspect the result even when
; the simulated guest work takes longer than the PIT interval.
mov al, 0fdh
out 21h, al
done:
pop cx
pop ax
cmp byte [cs:far_return], 0
je .iret
sti
retf 2
.iret:
iret
chained:
; This software IRET belongs to a different frame and cannot release IRQ0.
iret
active db 0
far_return db 0
reentered dw 0
rounds dw 0
saved_ss dw 0
saved_sp dw 0
pass_msg db 'IRQ guard regression PASS',13,10,'$'
fail_msg db 'IRQ guard regression FAIL',13,10,'$'
