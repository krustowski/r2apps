#include "cpu.h"
#include "printf.h"

void dump_registers(CPU_T *cpu) {
    printf((const uint8_t *)"\n[ CPU Registers: ]\n");
    printf((const uint8_t *)"[ AX: %x, BX: %x, CX: %x, DX: %x, SP: %x, BP: %x, "
                            "SI: %x, DI: %x ]\n",
           cpu->AX, cpu->BX, cpu->CX, cpu->DX, cpu->SP, cpu->BP, cpu->SI, cpu->DI);
    printf((const uint8_t *)"[ CS: %x, DS: %x, ES: %x, SS: %x, IP: %x ]\n", cpu->CS, cpu->DS, cpu->ES, cpu->SS, cpu->IP);
}
