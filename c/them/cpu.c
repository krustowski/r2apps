#include "cpu.h"
#include "printf.h"

void dump_registers(CPU_T *cpu) {
    printf((const uint8_t *)"\n[ CPU Registers: ]\n");
    printf((const uint8_t *)"[ AX: %x, BX: %x, CX: %x, DX: %x, SP: %x, BP: %x, SI: %x, DI: %x ]\n", cpu->AX, cpu->BX, cpu->CX, cpu->DX, cpu->SP, cpu->BP, cpu->SI, cpu->DI);
    printf((const uint8_t *)"[ CS: %x, DS: %x, ES: %x, SS: %x, IP: %x ]\n", cpu->CS, cpu->DS, cpu->ES, cpu->SS, cpu->IP);
}

uint8_t get_flag(CPU_T *cpu, FLAGS_MASK flag) { return cpu->FLAGS & flag; }

uint8_t set_flag(CPU_T *cpu, FLAGS_MASK flag, uint8_t value) {
    if (get_flag(cpu, flag) == value) {
        return 0;
    }

    if (value) {
        cpu->FLAGS |= flag;
    } else {
        cpu->FLAGS &= ~flag;
    }

    return 1;
}

uint8_t jump_short(CPU_T *cpu, uint8_t addr) {
    cpu->IP = 0;
    cpu->IP = (uint16_t)addr;

    return 0;
}
