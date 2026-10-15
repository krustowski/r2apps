bits 16
cpu 286
%ifndef TEST_ORG
%define TEST_ORG 100h
%endif
org TEST_ORG

jmp start
; A zero-origin image must survive the later environment installation at
; PSP:2Ch, as well as the PSP's command tail at 80h.
times 90h - ($ - $$) db 0a5h
start:
cmp word [TEST_ORG + 2ch], 0a5a5h
jne fail
cmp word [TEST_ORG + 80h], 0a5a5h
jne fail
cmp si, TEST_ORG
jne fail
call .ip
.ip:
pop ax
cmp ax, .ip
jne fail
mov ax, cs
cmp ax, 1000h
jne fail
mov ax, ds
cmp ax, 1000h
jne fail
mov ax, es
cmp ax, 1000h
jne fail
mov ax, ss
cmp ax, 1000h
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
passed db 'origin regression PASS',13,10,'$'
failed db 'origin regression FAIL',13,10,'$'
