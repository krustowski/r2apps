bits 16
cpu 286
org 0

; Exercise the saved FLAGS in the real INT/BIOS callback/IRET path.
xor ax, ax
xor dx, dx
stc
int 13h
jc fail
push ds
pop es
mov bx, buffer
mov ax, 0201h
mov cx, 1
int 13h
jc fail
cmp word [buffer + 11], 512
jne fail
cmp word [buffer + 19], 2880
jne fail
cmp word [buffer + 510], 0aa55h
jne fail
mov ax, 0301h
int 13h
jnc fail
cmp ah, 3
jne fail
mov ah, 1
int 13h
jnc fail
cmp ah, 3
jne fail
xor ax, ax
int 13h
jc fail
mov dx, passed
mov ah, 9
int 21h
mov ax, 4c00h
int 21h
fail:
mov dx, failed
mov ah, 9
int 21h
mov ax, 4c01h
int 21h
passed db 'BIOS floppy interrupt regression PASS',13,10,'$'
failed db 'BIOS floppy interrupt regression FAIL',13,10,'$'
buffer times 512 db 0a5h
