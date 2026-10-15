bits 16
cpu 286
org 100h
mov sp, 1000h
mov bx, 100h
mov ah, 4ah
int 21h
jc fail
mov [params+4], ds
mov dx, name
mov ax, 3c00h
xor cx, cx
int 21h
jc fail
cmp ax, 5
jne fail
mov bx, ax
mov dx, text
mov cx, 3
mov ah, 40h
int 21h
jc fail
mov word [runs], 64
again:
; Reset the shared position. The child reads A and closes its inherited
; reference, then reuses local handle 5 for a different file.
mov bx, 5
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
mov bx, 5
mov cx, 2
mov dx, buffer
mov ah, 3fh
int 21h
jc fail
cmp ax, 2
jne fail
cmp word [buffer], 'BC'
jne fail
dec word [runs]
jnz again
; A failed EXEC must not retain references either.
mov dx, missing
mov bx, params
mov ax, 4b00h
int 21h
jnc fail
mov bx, 5
mov ah, 3eh
int 21h
jc fail
; Every file object must now be free, not just the local handle slots.
mov word [next_handle], 5
fill:
mov dx, probe
xor cx, cx
mov ax, 3c00h
int 21h
jc fail
cmp ax, [next_handle]
jne fail
inc word [next_handle]
cmp word [next_handle], 20
jb fill
mov bx, 5
close_all:
mov ah, 3eh
int 21h
jc fail
inc bx
cmp bx, 20
jb close_all
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
name db 'PARENT.DAT',0
probe db 'PROBE.DAT',0
child db 'CHILD.COM',0
missing db 'MISSING.COM',0
text db 'ABC'
buffer dw 0
runs dw 0
next_handle dw 0
params dw 0, tail, 0, 0, 0, 0, 0
tail db 0,13
passed db 'file lifetime regression PASS',13,10,'$'
failed db 'file lifetime regression FAIL',13,10,'$'
