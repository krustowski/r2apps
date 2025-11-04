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
	ADD_16		= 0x81, /* SUB_16 */
	ADD_8		= 0x83, /* SUB_8 */
	HLT 		= 0xF4,

	INC_AX 		= 0x40,
	INC_BX 		= 0x43,
	INC_CX 		= 0x41,
	INC_DX 		= 0x42,
	INC_SP		= 0x44,
	INC_BP		= 0x45,
	INC_SI		= 0x46,
	INC_DI		= 0x47,

	DEC_AX 		= 0x48,
	DEC_BX 		= 0x4B,
	DEC_CX 		= 0x49,
	DEC_DX 		= 0x4A,
	DEC_SP		= 0x4C,
	DEC_BP		= 0x4D,
	DEC_SI		= 0x4E,
	DEC_DI		= 0x4F,

	INT_8 		= 0xCD,
	IRET 		= 0xCF,

	JMP_REL8 	= 0xE8,
	JMP_REL16 	= 0xE9,

	MOV_AL		= 0xB0,
	MOV_BL		= 0xB3,
	MOV_CL		= 0xB1,
	MOV_DL		= 0xB2,
	MOV_AH		= 0xB4,
	MOV_BH		= 0xB7,
	MOV_CH		= 0xB5,
	MOV_DH		= 0xB6,

	MOV_AX 		= 0xB8,
	MOV_BX 		= 0xBB,
	MOV_CX 		= 0xB9,
	MOV_DX 		= 0xBA,
	MOV_SP		= 0xBC,
	MOV_BP		= 0xBD,
	MOV_SI		= 0xBE,
	MOV_DI		= 0xBF,

	MOV_SR		= 0x8E,

	NOP 		= 0x90,
	
	PUSH_AX		= 0x50,
	PUSH_BX		= 0x53,
	PUSH_CX		= 0x51,
	PUSH_DX		= 0x52,
	PUSH_SP		= 0x54,
	PUSH_BP		= 0x55,
	PUSH_SI		= 0x56,
	PUSH_DI		= 0x57,
};

/* General-purpose registers */
enum GPR
{
	ADD_AX 		= 0xC0,
	ADD_BX		= 0xC3,
	ADD_CX		= 0xC1,
	ADD_DX		= 0xC2,
	ADD_SP		= 0xC4,
	ADD_BP		= 0xC5,
	ADD_SI		= 0xC6,
	ADD_DI		= 0xC7,

	MOV_DS_AX	= 0xD8,
	MOV_DS_BX	= 0xDB,
	MOV_DS_CX	= 0xD9,
	MOV_DS_DX	= 0xDA,
	MOV_DS_SP	= 0xDC,
	MOV_DS_BP	= 0xDD,
	MOV_DS_SI	= 0xDE,
	MOV_DS_DI	= 0xDF,

	MOV_ES_AX 	= 0xC0,
	MOV_ES_BX 	= 0xC3,
	MOV_ES_CX 	= 0xC1,
	MOV_ES_DX 	= 0xC2,
	MOV_ES_SP 	= 0xC4,
	MOV_ES_BP 	= 0xC5,
	MOV_ES_SI 	= 0xC6,
	MOV_ES_DI 	= 0xC7,

	MOV_SS_AX	= 0xD0,
	MOV_SS_BX	= 0xD3,
	MOV_SS_CX	= 0xD1,
	MOV_SS_DX	= 0xD2,
	MOV_SS_SP	= 0xD4,
	MOV_SS_BP	= 0xD5,
	MOV_SS_SI	= 0xD6,
	MOV_SS_DI	= 0xD7,

	SUB_AX	 	= 0xE8,
	SUB_BX	 	= 0xEB,
	SUB_CX	 	= 0xE9,
	SUB_DX	 	= 0xEA,
	SUB_SP		= 0xEC,
	SUB_BP		= 0xED,
	SUB_SI		= 0xEE,
	SUB_DI		= 0xEF
};

void dump_cpu(CPU_T* cpu)
{
	printf("[ AX: %x, BX: %x, CX: %x, DX: %x, SP: %x, BP: %x, SI: %x, DI: %x ]\n", cpu->AX, cpu->BX, cpu->CX, cpu->DX, cpu->SP, cpu->BP, cpu->SI, cpu->DI);
	printf("[ CS: %x, DS: %x, ES: %x, SS: %x, IP: %x ]\n", cpu->CS, cpu->DS, cpu->ES, cpu->SS, cpu->IP);
}

void switch_opcode(CPU_T* cpu, uint8_t* memory)
{
	uint8_t halt = 0;

	uint8_t stack[256];

	while (!halt)
	{
		enum OP_CODES opcode = memory[cpu->IP++];

		switch (opcode)
		{
			case ADD_8:
			case ADD_16:
				{
					enum GPR reg = memory[cpu->IP++];
					uint16_t value;

					switch (opcode)
					{
						case ADD_8:
							{
								value = (uint16_t) memory[cpu->IP++];
								break;
							}
						case ADD_16:
							{
								value = memory[cpu->IP++] | memory[cpu->IP++] << 8;
								break;
							}
					}

					switch (reg)
					{
						case ADD_AX:
							{
								cpu->AX += value;
								break;
							}
						case ADD_BX:
							{
								cpu->BX += value;
								break;
							}
						case ADD_CX:
							{
								cpu->CX += value;
								break;
							}
						case ADD_DX:
							{
								cpu->DX += value;
								break;
							}
						case ADD_SP:
							{
								cpu->SP += value;
								break;
							}
						case ADD_BP:
							{
								cpu->BP += value;
								break;
							}
						case ADD_SI:
							{
								cpu->SI += value;
								break;
							}
						case ADD_DI:
							{
								cpu->DI += value;
								break;
							}
						case SUB_AX:
							{
								cpu->AX -= value;
								break;
							}
						case SUB_BX:
							{
								cpu->BX -= value;
								break;
							}
						case SUB_CX:
							{
								cpu->CX -= value;
								break;
							}
						case SUB_DX:
							{
								cpu->DX -= value;
								break;
							}
						case SUB_SP:
							{
								cpu->SP -= value;
								break;
							}
						case SUB_BP:
							{
								cpu->BP -= value;
								break;
							}
						case SUB_SI:
							{
								cpu->SI -= value;
								break;
							}
						case SUB_DI:
							{
								cpu->DI -= value;
								break;
							}
					}
					break;
				}
			case INC_AX:
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
			case INC_SP:
				{
					cpu->SP++;
					break;
				}
			case INC_BP:
				{
					cpu->BP++;
					break;
				}
			case INC_SI:
				{
					cpu->SI++;
					break;
				}
			case INC_DI:
				{
					cpu->DI++;
					break;
				}
			case DEC_AX:
				{
					cpu->AX--;
					break;
				}
			case DEC_BX:
				{
					cpu->BX--;
					break;
				}
			case DEC_CX:
				{
					cpu->CX--;
					break;
				}
			case DEC_DX:
				{
					cpu->DX--;
					break;
				}
			case DEC_SP:
				{
					cpu->SP--;
					break;
				}
			case DEC_BP:
				{
					cpu->BP--;
					break;
				}
			case DEC_SI:
				{
					cpu->SI--;
					break;
				}
			case DEC_DI:
				{
					cpu->DI--;
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
			case MOV_AL: 
				{
					uint8_t al = cpu->AX & 0xff;
					uint8_t ah = cpu->AX >> 8;

					al = memory[cpu->IP++];

					cpu->AX = al | ah << 8;
					break;
				}
			case MOV_AH: 
				{
					uint8_t al = cpu->AX & 0xff;
					uint8_t ah = cpu->AX >> 8;

					ah = memory[cpu->IP++];

					cpu->AX = al | ah << 8;
					break;
				}
			case MOV_AX:
				{
					cpu->AX = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_BL: 
				{
					uint8_t bl = cpu->BX & 0xff;
					uint8_t bh = cpu->BX >> 8;

					bl = memory[cpu->IP++];

					cpu->BX = bl | bh << 8;
					break;
				}
			case MOV_BH: 
				{
					uint8_t bl = cpu->BX & 0xff;
					uint8_t bh = cpu->BX >> 8;

					bh = memory[cpu->IP++];

					cpu->BX = bl | bh << 8;
					break;
				}
			case MOV_BX:
				{
					cpu->BX = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_CL: 
				{
					uint8_t cl = cpu->CX & 0xff;
					uint8_t ch = cpu->CX >> 8;

					cl = memory[cpu->IP++];

					cpu->CX = cl | ch << 8;
					break;
				}
			case MOV_CH: 
				{
					uint8_t cl = cpu->CX & 0xff;
					uint8_t ch = cpu->CX >> 8;

					ch = memory[cpu->IP++];

					cpu->CX = cl | ch << 8;
					break;
				}
			case MOV_CX:
				{
					cpu->CX = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_DL: 
				{
					uint8_t dl = cpu->DX & 0xff;
					uint8_t dh = cpu->DX >> 8;

					dl = memory[cpu->IP++];

					cpu->DX = dl | dh << 8;
					break;
				}
			case MOV_DH: 
				{
					uint8_t dl = cpu->DX & 0xff;
					uint8_t dh = cpu->DX >> 8;

					dh = memory[cpu->IP++];

					cpu->DX = dl | dh << 8;
					break;
				}
			case MOV_DX:
				{
					cpu->DX = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_SP:
				{
					cpu->SP = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_BP:
				{
					cpu->BP = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_SI:
				{
					cpu->SI = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_DI:
				{
					cpu->DI = memory[cpu->IP++] | memory[cpu->IP++] << 8;
					break;
				}
			case MOV_SR:
				{
					enum GPR reg = memory[cpu->IP++];

					switch (reg)
					{
						case MOV_DS_AX:
							{
								cpu->DS = cpu->AX;
								break;
							}
						case MOV_DS_BX:
							{
								cpu->DS = cpu->BX;
								break;
							}
						case MOV_DS_CX:
							{
								cpu->DS = cpu->CX;
								break;
							}
						case MOV_DS_DX:
							{
								cpu->DS = cpu->DX;
								break;
							}
						case MOV_DS_SP:
							{
								cpu->DS = cpu->SP;
								break;
							}
						case MOV_DS_BP:
							{
								cpu->DS = cpu->BP;
								break;
							}
						case MOV_DS_SI:
							{
								cpu->DS = cpu->SI;
								break;
							}
						case MOV_DS_DI:
							{
								cpu->DS = cpu->DI;
								break;
							}
						case MOV_ES_AX:
							{
								cpu->ES = cpu->AX;
								break;
							}
						case MOV_ES_BX:
							{
								cpu->ES = cpu->BX;
								break;
							}
						case MOV_ES_CX:
							{
								cpu->ES = cpu->CX;
								break;
							}
						case MOV_ES_DX:
							{
								cpu->ES = cpu->DX;
								break;
							}
						case MOV_ES_SP:
							{
								cpu->ES = cpu->SP;
								break;
							}
						case MOV_ES_BP:
							{
								cpu->ES = cpu->BP;
								break;
							}
						case MOV_ES_SI:
							{
								cpu->ES = cpu->SI;
								break;
							}
						case MOV_ES_DI:
							{
								cpu->ES = cpu->DI;
								break;
							}
						case MOV_SS_AX:
							{
								cpu->SS = cpu->AX;
								break;
							}
						case MOV_SS_BX:
							{
								cpu->SS = cpu->BX;
								break;
							}
						case MOV_SS_CX:
							{
								cpu->SS = cpu->CX;
								break;
							}
						case MOV_SS_DX:
							{
								cpu->SS = cpu->DX;
								break;
							}
						case MOV_SS_SP:
							{
								cpu->SS = cpu->SP;
								break;
							}
						case MOV_SS_BP:
							{
								cpu->SS = cpu->BP;
								break;
							}
						case MOV_SS_SI:
							{
								cpu->SS = cpu->SI;
								break;
							}
						case MOV_SS_DI:
							{
								cpu->SS = cpu->DI;
								break;
							}
					}

					break;
				}
			case PUSH_AX:
				{
					stack[cpu->SP--] = cpu->AX >> 8;
					stack[cpu->SP--] = cpu->AX & 0xff;
					break;
				}
			case PUSH_BX:
				{
					stack[cpu->SP--] = cpu->BX >> 8;
					stack[cpu->SP--] = cpu->BX & 0xff;
					break;
				}
			case PUSH_SP:
				{
					uint16_t sp = cpu->SP;

					stack[cpu->SP--] = sp >> 8;
					stack[cpu->SP--] = sp & 0xff;
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

