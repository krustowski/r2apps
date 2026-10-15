bits 16
cpu 286
org 100h
; Keep the test and stack inside the resized PSP block.
mov sp, 1000h
mov bx, 100h
mov ah, 4ah
int 21h
jc fail
; An impossible allocation reports the largest available range.
mov bx, 0ffffh
mov ah, 48h
int 21h
jnc fail
cmp ax, 8
jne fail
cmp bx, 8f00h
jne fail
; Probe, free, and allocate again: Lemmings' startup sequence.
mov bx, 5ad8h
mov ah, 48h
int 21h
jc fail
mov [first], ax
mov es, ax
mov ah, 49h
int 21h
jc fail
mov bx, 20h
mov ah, 48h
int 21h
jc fail
cmp ax, [first]
jne fail
mov [first], ax
mov bx, 20h
mov ah, 48h
int 21h
jc fail
mov [second], ax
; Growing into a live neighbour must fail and return maximum size.
mov es, [first]
mov bx, 21h
mov ah, 4ah
int 21h
jnc fail
cmp ax, 8
jne fail
cmp bx, 20h
jne fail
mov es, [second]
mov ah, 49h
int 21h
jc fail
mov es, [first]
mov bx, 40h
mov ah, 4ah
int 21h
jc fail
; Invalid block handles must fail, including a double free.
mov ah, 49h
int 21h
jc fail
mov ah, 49h
int 21h
jnc fail
cmp ax, 9
jne fail
; AH=1Bh returns a fixed-drive media descriptor via DS:BX.
push ds
mov ah, 1bh
int 21h
cmp byte [bx], 0f8h
pop ds
jne fail
; Enabling TF with POPF must trap after the first NOP, not POPF.
mov dx, trap_handler
mov ax, 2501h
int 21h
pushf
pop ax
or ax, 100h
push ax
popf
nop
nop
cmp word [traps], 2
jne fail
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
trap_handler:
push bp
mov bp, sp
inc word [cs:traps]
cmp word [cs:traps], 2
jne .return
and word [ss:bp+6], 0feffh
.return:
pop bp
iret
first dw 0
second dw 0
traps dw 0
passed db 'memory and trap regression PASS',13,10,'$'
failed db 'memory and trap regression FAIL',13,10,'$'
