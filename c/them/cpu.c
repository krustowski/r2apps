#include "printf.h"

#include "cpu.h"

void dump_registers(CPU_T* cpu)
{
	printf("\n[ CPU Registers: ]\n");
	printf("[ AX: %x, BX: %x, CX: %x, DX: %x, SP: %x, BP: %x, SI: %x, DI: %x ]\n", cpu->AX, cpu->BX, cpu->CX, cpu->DX, cpu->SP, cpu->BP, cpu->SI, cpu->DI);
	printf("[ CS: %x, DS: %x, ES: %x, SS: %x, IP: %x ]\n", cpu->CS, cpu->DS, cpu->ES, cpu->SS, cpu->IP);
}

