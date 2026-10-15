bits 16
cpu 286
org 100h
mov sp, 1000h
mov bx, 100h
mov ah, 4ah
int 21h
jc fail
mov [params+4], ds
mov bx, 5
mov cx, 1
mov dx, buffer
mov ah, 3fh
int 21h
jc fail
cmp ax, 1
jne fail
cmp byte [buffer], 'A'
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
xor cx, cx
xor dx, dx
mov ax, 4200h
int 21h
jc fail
push ds
pop es
mov dx, child
mov bx, params
mov ax, 4b00h
int 21h
jc fail
mov ah, 4dh
int 21h
test ax, ax
jnz fail
; Grandchild closed its own reference; the child's handle still points
; at CHILD.DAT with the shared position advanced by one byte.
mov bx, 5
mov cx, 1
mov dx, buffer
mov ah, 3fh
int 21h
jc fail
test ax, ax
jnz fail
; Leave this buffered file open. Termination must flush and release it.
mov ax, 4c00h
int 21h
fail:
mov ax, 4c01h
int 21h
name db 'CHILD.DAT',0
child db 'GRAND.COM',0
text db 'C'
buffer db 0
params dw 0, tail, 0, 0, 0, 0, 0
tail db 0,13
