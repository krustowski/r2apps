#include "mem.h"
#include "printf.h"

#include "cpu.h"
#include "int.h"

/*
 *  theM
 *
 *  Simple 16bit CPU eMulator.
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

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

					switch (int_code)
					{
						case 0x21:
							{
								int21h(cpu, memory);
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
