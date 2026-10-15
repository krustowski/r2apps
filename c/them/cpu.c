#include "cpu.h"
#include "mmu.h"
#include "printf.h"

void dump_registers(CPU_T *cpu) {
    printf((const uint8_t *)"\n[ CPU Registers: ]\n");
    printf((const uint8_t *)"[ AX: %x, BX: %x, CX: %x, DX: %x, SP: %x, BP: %x, SI: %x, DI: %x ]\n", cpu->AX, cpu->BX, cpu->CX, cpu->DX, cpu->SP, cpu->BP, cpu->SI, cpu->DI);
    printf((const uint8_t *)"[ CS: %x, DS: %x, ES: %x, SS: %x, IP: %x ]\n", cpu->CS, cpu->DS, cpu->ES, cpu->SS, cpu->IP);
}

uint8_t get_flag(CPU_T *cpu, FLAGS_MASK flag) { return !!(cpu->FLAGS & flag); }

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

/* 16-bit GP register by index (AX=0, CX=1, DX=2, BX=3, SP=4, BP=5, SI=6, DI=7) */
uint16_t get_reg16(CPU_T *cpu, uint8_t idx) {
    switch (idx) {
    case 0:
        return cpu->AX;
    case 1:
        return cpu->CX;
    case 2:
        return cpu->DX;
    case 3:
        return cpu->BX;
    case 4:
        return cpu->SP;
    case 5:
        return cpu->BP;
    case 6:
        return cpu->SI;
    case 7:
        return cpu->DI;
    }
    return 0;
}

void set_reg16(CPU_T *cpu, uint8_t idx, uint16_t val) {
    switch (idx) {
    case 0:
        cpu->AX = val;
        break;
    case 1:
        cpu->CX = val;
        break;
    case 2:
        cpu->DX = val;
        break;
    case 3:
        cpu->BX = val;
        break;
    case 4:
        cpu->SP = val;
        break;
    case 5:
        cpu->BP = val;
        break;
    case 6:
        cpu->SI = val;
        break;
    case 7:
        cpu->DI = val;
        break;
    }
}

/* 8-bit registers: AL=0,CL=1,DL=2,BL=3,AH=4,CH=5,DH=6,BH=7 */
uint8_t get_reg8(CPU_T *cpu, uint8_t idx) {
    switch (idx) {
    case 0:
        return cpu->AX & 0xff;
    case 1:
        return cpu->CX & 0xff;
    case 2:
        return cpu->DX & 0xff;
    case 3:
        return cpu->BX & 0xff;
    case 4:
        return cpu->AX >> 8;
    case 5:
        return cpu->CX >> 8;
    case 6:
        return cpu->DX >> 8;
    case 7:
        return cpu->BX >> 8;
    }
    return 0;
}

void set_reg8(CPU_T *cpu, uint8_t idx, uint8_t val) {
    switch (idx) {
    case 0:
        cpu->AX = (cpu->AX & 0xff00) | val;
        break;
    case 1:
        cpu->CX = (cpu->CX & 0xff00) | val;
        break;
    case 2:
        cpu->DX = (cpu->DX & 0xff00) | val;
        break;
    case 3:
        cpu->BX = (cpu->BX & 0xff00) | val;
        break;
    case 4:
        cpu->AX = (cpu->AX & 0x00ff) | ((uint16_t)val << 8);
        break;
    case 5:
        cpu->CX = (cpu->CX & 0x00ff) | ((uint16_t)val << 8);
        break;
    case 6:
        cpu->DX = (cpu->DX & 0x00ff) | ((uint16_t)val << 8);
        break;
    case 7:
        cpu->BX = (cpu->BX & 0x00ff) | ((uint16_t)val << 8);
        break;
    }
}

/*
 *  Segmented memory access.
 *
 *  Real-mode addresses are (segment << 4) + offset, with the offset wrapping
 *  inside its own 64 KiB window: a word read at offset 0xFFFF takes its high
 *  byte from offset 0x0000 of the same segment, not from the next one.
 */
uint8_t mem_r8(Memory_T *mem, uint16_t seg, uint16_t off) {
    return mmu_read(mem, (uint32_t)(((uint32_t)seg << 4) + off) & 0xFFFFF);
}

void mem_w8(Memory_T *mem, uint16_t seg, uint16_t off, uint8_t value) {
    mmu_write(mem, (uint32_t)(((uint32_t)seg << 4) + off) & 0xFFFFF, value);
}

uint16_t mem_r16(Memory_T *mem, uint16_t seg, uint16_t off) {
    uint8_t lo = mem_r8(mem, seg, off);
    uint8_t hi = mem_r8(mem, seg, (uint16_t)(off + 1));

    return (uint16_t)((hi << 8) | lo);
}

void mem_w16(Memory_T *mem, uint16_t seg, uint16_t off, uint16_t value) {
    mem_w8(mem, seg, off, (uint8_t)(value & 0xFF));
    mem_w8(mem, seg, (uint16_t)(off + 1), (uint8_t)(value >> 8));
}
