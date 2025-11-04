#include "mem.h"
#include "printf.h"

/*
 *  theM
 *
 *  Simple 16bit CPU eMulator.
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

typedef struct 
{
	/* 16-bit general-purpose (GP) registers */
	uint16_t AX;
	uint16_t BX;
	uint16_t CX;
	uint16_t DX;

	/* The stack pointer (top of the stack) */
	uint16_t SP;
	/* Base pointer */
	uint16_t BP;

	/* Address registers */
	uint16_t SI;
	uint16_t DI;

	/* Segment registers (to form a memory address) */
	uint16_t CS;
	uint16_t DS;
	uint16_t ES;
	uint16_t SS;

	uint16_t FLAGS;
	uint16_t IP;
}
__attribute__((packed)) CPU_T;

enum OP_CODES
{
	ADD_16		= 0x81,
	ADD_8		= 0x83,
	HLT 		= 0xF4,

	INC_AX 		= 0x40,
	INC_BX 		= 0x43,
	INC_CX 		= 0x41,
	INC_DX 		= 0x42,

	INT_8 		= 0xCD,
	IRET 		= 0xCF,

	JMP_REL8 	= 0xE8,
	JMP_REL16 	= 0xE9,

	MOV_AX 		= 0xB8,
	MOV_BX 		= 0xBB,
	MOV_CX 		= 0xB9,
	MOV_DX 		= 0xBA,

	NOP 		= 0x90
	
};

/* General-purpose registers */
enum GPR
{
	AH 	= 0x00,
	BH,
	CH,
	DH,

	AX_ADD 	= 0xC0,
	BX_ADD	= 0xC3,
	CX_ADD	= 0xC1,
	DX_ADD	= 0xC2,

	AX_SUB 	= 0xE8,
	BX_SUB 	= 0xEB,
	CX_SUB 	= 0xE9,
	DX_SUB 	= 0xEA,
};

void dump_cpu(CPU_T* cpu)
{
	printf("AX: %x, BX: %x, CX: %x, DX: %x\n", cpu->AX, cpu->BX, cpu->CX, cpu->DX);
	printf("SP: %x, BP: %x, CS: %x, IP: %x\n", cpu->SP, cpu->BP, cpu->CS, cpu->IP);
}

void switch_opcode(CPU_T* cpu, uint8_t* memory)
{
	uint8_t halt = 0;

	while (!halt)
	{
		enum OP_CODES opcode = memory[cpu->IP++];

		switch (opcode)
		{
			case ADD_8: /* + SUB_8 */
				{
					enum GPR reg = memory[cpu->IP++];
					uint16_t value = (uint16_t) memory[cpu->IP++];

					switch (reg)
					{
						case AX_ADD:
							{
								cpu->AX += value;
								break;
							}
						case BX_ADD:
							{
								cpu->BX += value;
								break;
							}
						case CX_ADD:
							{
								cpu->CX += value;
								break;
							}
						case DX_ADD:
							{
								cpu->DX += value;
								break;
							}
						case AX_SUB:
							{
								cpu->AX -= value;
								break;
							}
						case BX_SUB:
							{
								cpu->BX -= value;
								break;
							}
						case CX_SUB:
							{
								cpu->CX -= value;
								break;
							}
						case DX_SUB:
							{
								cpu->DX -= value;
								break;
							}
					}
					break;
				}
			case ADD_16: /* + SUB_16 */
				{
					enum GPR reg = memory[cpu->IP++];
					uint16_t value = memory[cpu->IP++] | memory[cpu->IP++] << 8;

					switch (reg)
					{
						case AX_ADD:
							{
								cpu->AX += value;
								break;
							}
						case BX_ADD:
							{
								cpu->BX += value;
								break;
							}
						case CX_ADD:
							{
								cpu->CX += value;
								break;
							}
						case DX_ADD:
							{
								cpu->DX += value;
								break;
							}
						case AX_SUB:
							{
								cpu->AX -= value;
								break;
							}
						case BX_SUB:
							{
								cpu->BX -= value;
								break;
							}
						case CX_SUB:
							{
								cpu->CX -= value;
								break;
							}
						case DX_SUB:
							{
								cpu->DX -= value;
								break;
							}
					}
					break;
				}
			case INC_AX: /* Increment r/m word by 1 */
				{
					cpu->AX++;
					break;
				}
			case INC_BX:
				{
					cpu->BX++;
					break;
				}
			case INC_CX:
				{
					cpu->CX++;
					break;
				}
			case INC_DX:
				{
					cpu->DX++;
					break;
				}
			case INT_8:
				{
					uint8_t int_code = memory[cpu->IP++];
					break;
				}
			case HLT:
				{
					print("=> Program stop (halt)\n");

					halt++;
					break;
				}
			case MOV_AX:
				{
					print("=> Moving...\n");

					cpu->AX = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_BX:
				{
					print("=> Moving...\n");

					cpu->BX = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_CX:
				{
					print("=> Moving...\n");

					cpu->CX = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_DX:
				{
					print("=> Moving...\n");

					cpu->DX = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			default: 
				{
					printf("=> Unknown opcode: %x\n", opcode);
					break;
				}
		}
	}
}

int main(int64_t pid, int64_t arg)
{
	print("\ntheM: the 16bit CPU emulator\n");

	uint8_t memory[256];
	CPU_T cpu;

	/* Reset CPU's program instruction counter */
	cpu.IP = 0;

	/* Load the program */
	read_file((const uint8_t *) "PRG0.BIN", memory);
	/*{
		print("=> Cannot read the binary file... Program exit.\n");
		exit(pid, 161);
	}*/

	/* Print the initial CPU state */
	dump_cpu(&cpu);

	/* Switch opcode and emulate the operation */
	switch_opcode(&cpu, memory);

	/* Print the final CPU state */
	dump_cpu(&cpu);

	exit(pid, 191);
}

