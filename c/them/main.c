#include "mem.h"
#include "net.h"
#include "printf.h"
#include "string.h"

/*
 *  them
 *
 *  Simple 16bit CPU emulator.
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
	ADD_AH_IB = 0x04,
	ADD_AX_IW = 0x05,
	AND_AL_IB = 0x24,
	AND_AX_IW = 0x25,
	HLT = 0xF4,
	INC_RM16 = 0xFF,
	INT_IMM8 = 0xCD,
	IRET = 0xCF,
	JMP_REL8 = 0xE8,
	JMP_REL16 = 0xE9,
	MOV_R16_IMM16 = 0xB8,
	NOP = 0x90
	
};

/* General-purpose registers */
enum GPR
{
	AH = 0x00,
	BH,
	CH,
	DH,
	AX,
	BX,
	CX,
	DX
};

void dump_cpu(CPU_T* cpu)
{
	printf("AX: %x, BX: %x, CX: %x, DX: %x\n", cpu->AX, cpu->BX, cpu->CX, cpu->DX);
	printf("SP: %x, BP: %x, CS: %x, IP: %x\n", cpu->SP, cpu->BP, cpu->CS, cpu->IP);
}

int main(int64_t pid, int64_t arg)
{
	print("theM: the 16bit CPU emulator\n");

	uint8_t halt = 0;
	uint8_t memory[256] = {0xB8, 0x04, 0x98, 0xFF, 0x04, 0xF4};
	CPU_T cpu;

	cpu.IP = 0;

	dump_cpu(&cpu);

	while (!halt)
	{
		uint8_t opcode = memory[cpu.IP++];

		switch (opcode)
		{
			case INC_RM16: /* Increment r/m word by 1. */
				{
					print("=> Incrementing...\n");

					enum GPR reg = memory[cpu.IP++];

					switch (reg)
					{
						case AX:
							{
								cpu.AX++;
								break;
							}
					}

					break;
				}
			case HLT:
				{
					print("=> Program stop (halt)\n");
					halt++;
					break;
				}
			case MOV_R16_IMM16:
				{
					print("=> Moving...\n");

					enum GPR reg = memory[cpu.IP++];

					switch (reg)
					{
						case AX:
							{
								cpu.AX = memory[cpu.IP++];
								break;
							}
					}

					break;
				}
			default: 
				{
					printf("=> Unknown opcode: %x\n", opcode);
					break;
				}
		}
	}

	dump_cpu(&cpu);

	exit(pid, 191);
}

