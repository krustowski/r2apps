#include "cpu.h"
#include "mmu.h"
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
    if (addr > (1 << 7) - 1) {
        addr -= (1 << 7);

        cpu->IP -= (1 << 7);
        cpu->IP += addr;
        return 0;
    }

    cpu->IP += addr;
    return 0;
}

/*void halt(uint8_t *hlt) {
    *hlt += 1;
    return;
}*/

uint8_t pop_reg(CPU_T *cpu, Memory_T *memory, REGISTER_T reg) {
    uint32_t stack_addr = (cpu->SS << 4) + cpu->SP;

    uint8_t hb = mmu_read(memory, stack_addr++);
    uint8_t lb = mmu_read(memory, stack_addr++);
    cpu->SP += 2;

    switch (reg) {
    case AX: {
        cpu->AX = hb << 8 | lb;
        break;
    }
    case BX: {
        cpu->BX = hb << 8 | lb;
        break;
    }
    case CX: {
        cpu->CX = hb << 8 | lb;
        break;
    }
    case DX: {
        cpu->DX = hb << 8 | lb;
        break;
    }
    case CS: {
        cpu->CS = hb << 8 | lb;
        break;
    }
    case DS: {
        cpu->DS = hb << 8 | lb;
        break;
    }
    case ES: {
        cpu->ES = hb << 8 | lb;
        break;
    }
    case SS: {
        cpu->SS = hb << 8 | lb;
        break;
    }
    case SP: {
        cpu->SP = hb << 8 | lb;
        break;
    }
    case BP: {
        cpu->BP = hb << 8 | lb;
        break;
    }
    case SI: {
        cpu->SI = hb << 8 | lb;
        break;
    }
    case DI: {
        cpu->DI = hb << 8 | lb;
        break;
    }
    case IP: {
        cpu->IP = hb << 8 | lb;
        break;
    }
    case FLAGS: {
        cpu->FLAGS = hb << 8 | lb;
        break;
    }
    }

    return 0;
}

uint8_t push_reg(CPU_T *cpu, Memory_T *memory, REGISTER_T reg) {
    uint32_t stack_addr = (cpu->SS << 4) + cpu->SP;

    uint8_t hb;
    uint8_t lb;

    switch (reg) {
    case AX: {
        hb = cpu->AX >> 8;
        lb = cpu->AX & 0xff;
        break;
    }
    case BX: {
        hb = cpu->BX >> 8;
        lb = cpu->BX & 0xff;
        break;
    }
    case CX: {
        hb = cpu->CX >> 8;
        lb = cpu->CX & 0xff;
        break;
    }
    case DX: {
        hb = cpu->DX >> 8;
        lb = cpu->DX & 0xff;
        break;
    }
    case CS: {
        hb = cpu->CS >> 8;
        lb = cpu->CS & 0xff;
        break;
    }
    case DS: {
        hb = cpu->DS >> 8;
        lb = cpu->DS & 0xff;
        break;
    }
    case ES: {
        hb = cpu->ES >> 8;
        lb = cpu->ES & 0xff;
        break;
    }
    case SS: {
        hb = cpu->SS >> 8;
        lb = cpu->SS & 0xff;
        break;
    }
    case SP: {
        hb = cpu->SP >> 8;
        lb = cpu->SP & 0xff;
        break;
    }
    case BP: {
        hb = cpu->BP >> 8;
        lb = cpu->BP & 0xff;
        break;
    }
    case SI: {
        hb = cpu->SI >> 8;
        lb = cpu->SI & 0xff;
        break;
    }
    case DI: {
        hb = cpu->DI >> 8;
        lb = cpu->DI & 0xff;
        break;
    }
    case IP: {
        hb = cpu->IP >> 8;
        lb = cpu->IP & 0xff;
        break;
    }
    case FLAGS: {
        hb = cpu->FLAGS >> 8;
        lb = cpu->FLAGS & 0xff;
        break;
    }
    }

    mmu_write(memory, --stack_addr, lb);
    mmu_write(memory, --stack_addr, hb);
    cpu->SP -= 2;

    return 0;
}
