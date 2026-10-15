bits 16
cpu 286
org 100h
mov bx, 5
mov cx, 1
mov dx, buffer
mov ah, 3fh
int 21h
jc fail
cmp ax, 1
jne fail
cmp byte [buffer], 'C'
jne fail
mov ah, 3eh
int 21h
jc fail
mov dx, name
xor cx, cx
mov ax, 3c00h
int 21h
jc fail
cmp ax, 5
jne fail
mov bx, ax
mov dx, text
mov cx, 1
mov ah, 40h
int 21h
jc fail
; Leave a second generation's file open, and terminate from a timer IRQ.
; EXEC must release that abandoned frame when it restores the child CPU.
mov ax, 3508h
int 21h
mov [old_off], bx
mov [old_seg], es
mov dx, terminate_irq
mov ax, 2508h
int 21h
sti
wait_irq:
hlt
jmp wait_irq
terminate_irq:
mov dx, [cs:old_off]
mov ds, [cs:old_seg]
mov ax, 2508h
int 21h
mov ax, 4c00h
int 21h
fail:
mov ax, 4c01h
int 21h
name db 'GRAND.DAT',0
text db 'G'
buffer db 0
old_off dw 0
old_seg dw 0
