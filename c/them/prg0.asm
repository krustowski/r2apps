[BITS 16]

section .data
msg 	db	"TEST LMAO$", 0

section .text
_start:
	MOV DX, 0x0098
	MOV DX, 0x1098
	INC DX

	MOV BX, 0x2345

	ADD AX, 0x06
	ADD BX, 0x06
	ADD CX, 0x06
	ADD DX, 0x1006
	ADD SI, 0x77

	SUB AX, 0x05
	SUB BX, 0x02
	SUB CX, 0x02
	SUB DX, 0x1002
	SUB SI, 0x71

	MOV AX, 0x08 
	MOV ES, AX
	MOV ES, BX
	MOV SS, AX
	MOV SS, BX
	MOV SS, CX
	MOV SS, DX
	MOV SS, SP
	MOV SS, BP
	MOV SS, SI
	MOV SS, DI
	MOV SP, 0xffe8

	MOV AL, 0
	MOV DL, 0
	MOV AH, 0

    MOV AX, 0xB800
    MOV ES, AX

    MOV word [ES:0x0000], 0x2F4F   ; 'O'
    MOV word [ES:0x0002], 0x2F59   ; 'Y'

	; Test 21h service 02
	MOV DL, 0x58
	MOV AH, 0x02
	INT 0x21

	; Test 21h service 09
	MOV DX, msg
	MOV AH, 0x09
	INT 0x21

	HLT
