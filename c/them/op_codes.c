#include "cpu.h"
#include "int.h"
#include "mmu.h"
#include "printf.h"

/*
 *  theM
 *
 *  Simple 16bit CPU eMulator.
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

uint8_t get_next_byte(CPU_T *cpu, Memory_T *memory) {
    uint32_t addr = (cpu->CS << 4) + cpu->IP++;

    /*printf((const uint8_t *)"=> Address: 0x%x\n", addr);*/

    return mmu_read(memory, addr);
}

uint32_t get_stack_addr(CPU_T *cpu) { return (cpu->SS << 4) + cpu->SP; }

void switch_opcode(CPU_T *cpu, Memory_T *memory) {
    uint8_t halt = 0;

    while (!halt) {
        OP_CODES opcode = get_next_byte(cpu, memory);

        uint32_t stack = get_stack_addr(cpu);

        switch (opcode) {
        case ADD_8:
        case ADD_16: {
            GPR reg = get_next_byte(cpu, memory);
            uint16_t value;

            if (opcode == ADD_8) {
                value = (uint16_t)get_next_byte(cpu, memory);
            } else {
                value = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
            }

            switch (reg) {
            case ADD_AX: {
                cpu->AX += value;
                break;
            }
            case ADD_BX: {
                cpu->BX += value;
                break;
            }
            case ADD_CX: {
                cpu->CX += value;
                break;
            }
            case ADD_DX: {
                cpu->DX += value;
                break;
            }
            case ADD_SP: {
                cpu->SP += value;
                break;
            }
            case ADD_BP: {
                cpu->BP += value;
                break;
            }
            case ADD_SI: {
                cpu->SI += value;
                break;
            }
            case ADD_DI: {
                cpu->DI += value;
                break;
            }
            case SUB_AX: {
                cpu->AX -= value;
                break;
            }
            case SUB_BX: {
                cpu->BX -= value;
                break;
            }
            case SUB_CX: {
                cpu->CX -= value;
                break;
            }
            case SUB_DX: {
                cpu->DX -= value;
                break;
            }
            case SUB_SP: {
                cpu->SP -= value;
                break;
            }
            case SUB_BP: {
                cpu->BP -= value;
                break;
            }
            case SUB_SI: {
                cpu->SI -= value;
                break;
            }
            case SUB_DI: {
                cpu->DI -= value;
                break;
            }
            }
            break;
        }
        case BOUND_R16: {
            uint8_t dst = get_next_byte(cpu, memory);
            uint8_t bl = get_next_byte(cpu, memory);
            uint8_t bh = get_next_byte(cpu, memory);
            uint16_t offset = bh << 8 | bl;

            int16_t idx = 0;

            switch (dst) {
            case 0x36: {
                /* SI, DS:addr */
                idx = (int16_t)cpu->SI;
                break;
            }
            case 0x3E: {
                /* DI, DS:addr */
                idx = (int16_t)cpu->DI;
                break;
            }
            }

            uint8_t lower_bound_l = mmu_read(memory, (cpu->DS << 4) + offset);
            uint8_t lower_bound_h = mmu_read(memory, (cpu->DS << 4) + offset + 1);
            uint8_t upper_bound_l = mmu_read(memory, (cpu->DS << 4) + offset + 2);
            uint8_t upper_bound_h = mmu_read(memory, (cpu->DS << 4) + offset + 3);

            int16_t lower_bound = (int16_t)(lower_bound_h << 8 | lower_bound_l);
            int16_t upper_bound = (int16_t)(upper_bound_h << 8 | upper_bound_l);

            if (lower_bound > (1 << 15) - 1) {
                lower_bound -= (1 << 15) + 1;
            }

            if (upper_bound > (1 << 15) - 1) {
                upper_bound -= (1 << 15) + 1;
            }

            if (idx > (1 << 15) - 1) {
                idx -= (1 << 15) + 1;
            }

            if (idx < lower_bound || idx > upper_bound + 16) {
                /* Bound range exceeded exception */
                printf((const uint8_t *)"=> #BR: BOUND: lower: 0x%x, idx: 0x%x, upper: 0x%x\n", lower_bound, idx, upper_bound);

                dump_registers(cpu);
                exit(0, 255);
            }

            break;
        }
        case CALL_REL16: {
            uint8_t bl = get_next_byte(cpu, memory);
            uint8_t bh = get_next_byte(cpu, memory);

            uint8_t ripl = cpu->IP & 0xff;
            uint8_t riph = cpu->IP >> 8;

            /* Push the return address to the stack */
            mmu_write(memory, --stack, ripl);
            mmu_write(memory, --stack, riph);
            cpu->SP -= 2;

            cpu->IP = bh << 8 | bl;
            break;
        }
        case CMP_AL_IMM8: {
            uint8_t al = cpu->AX & 0xff;
            uint8_t to_cmp = get_next_byte(cpu, memory);

            if (al == to_cmp) {
                set_flag(cpu, ZF, 1);
            }
            break;
        }
        case INC_AX: {
            cpu->AX++;
            break;
        }
        case INC_BX: {
            cpu->BX++;
            break;
        }
        case INC_CX: {
            cpu->CX++;
            break;
        }
        case INC_DX: {
            cpu->DX++;
            break;
        }
        case INC_SP: {
            cpu->SP++;
            break;
        }
        case INC_BP: {
            cpu->BP++;
            break;
        }
        case INC_SI: {
            cpu->SI++;
            break;
        }
        case INC_DI: {
            cpu->DI++;
            break;
        }
        case DEC_AX: {
            cpu->AX--;
            break;
        }
        case DEC_BX: {
            cpu->BX--;
            break;
        }
        case DEC_CX: {
            cpu->CX--;
            break;
        }
        case DEC_DX: {
            cpu->DX--;
            break;
        }
        case DEC_SP: {
            cpu->SP--;
            break;
        }
        case DEC_BP: {
            cpu->BP--;
            break;
        }
        case DEC_SI: {
            cpu->SI--;
            break;
        }
        case DEC_DI: {
            cpu->DI--;
            break;
        }
        case INT_8: {
            int_bios(cpu, memory);
            break;
        }
        case JA_REL8: {
            if (!get_flag(cpu, CF) && !get_flag(cpu, ZF)) {
                jump_short(cpu, get_next_byte(cpu, memory));
            }
            break;
        }
        case JE_REL8: {
            if (get_flag(cpu, ZF)) {
                jump_short(cpu, get_next_byte(cpu, memory));
            }
            break;
        }
        case JMP_REL16: {
            uint8_t bl = get_next_byte(cpu, memory);
            uint8_t bh = get_next_byte(cpu, memory);

            cpu->IP = bh << 8 | bl;
            break;
        }
        case HLT: {
            printf((const uint8_t *)"=> Program stop (halt)\n");

            halt++;
            break;
        }
        case LEAVE: {
            cpu->SP = cpu->BP;

            printf((const uint8_t *)"=> LEAVE\n");

            /* pop BP */
            uint8_t bph = mmu_read(memory, --stack);
            uint8_t bpl = mmu_read(memory, --stack);

            cpu->BP = bph << 8 | bpl;
            cpu->SP += 4;
            break;
        }
        case LODS_M8: {
            uint8_t al = mmu_read(memory, (cpu->DS << 4) + cpu->SI);
            uint8_t ah = cpu->AX >> 8;

            cpu->AX = ah << 8 | al;
            break;
        }
        case LODS_M16: {
            uint8_t al = mmu_read(memory, (cpu->DS << 4) + cpu->SI);
            uint8_t ah = mmu_read(memory, (cpu->DS << 4) + cpu->SI + 1);

            cpu->AX = ah << 8 | al;
            break;
        }
        case LOOP_REL8: {
            uint8_t addr = get_next_byte(cpu, memory);

            if (cpu->CX) {
                cpu->CX--;
                cpu->IP += ((int8_t)addr - 128);
            }
            break;
        }
        case LOOPE_REL8: {
            uint8_t addr = get_next_byte(cpu, memory);

            if (cpu->CX && get_flag(cpu, ZF)) {
                cpu->CX--;
                cpu->IP += ((int8_t)addr - 128);
            }
            break;
        }
        case LOOPNE_REL8: {
            uint8_t addr = get_next_byte(cpu, memory);

            if (cpu->CX && !get_flag(cpu, ZF)) {
                cpu->CX--;
                cpu->IP += ((int8_t)addr - 128);
            }
            break;
        }
        case MOV_AL: {
            uint8_t al = cpu->AX & 0xff;
            uint8_t ah = cpu->AX >> 8;

            al = get_next_byte(cpu, memory);

            cpu->AX = al | ah << 8;
            break;
        }
        case MOV_AH: {
            uint8_t al = cpu->AX & 0xff;
            uint8_t ah = cpu->AX >> 8;

            ah = get_next_byte(cpu, memory);

            cpu->AX = al | ah << 8;
            break;
        }
        case MOV_AX: {
            cpu->AX = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
            break;
        }
        case MOV_BL: {
            uint8_t bl = cpu->BX & 0xff;
            uint8_t bh = cpu->BX >> 8;

            bl = get_next_byte(cpu, memory);

            cpu->BX = bl | bh << 8;
            break;
        }
        case MOV_BH: {
            uint8_t bl = cpu->BX & 0xff;
            uint8_t bh = cpu->BX >> 8;

            bh = get_next_byte(cpu, memory);

            cpu->BX = bl | bh << 8;
            break;
        }
        case MOV_BX: {
            cpu->BX = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
            break;
        }
        case MOV_CL: {
            uint8_t cl = cpu->CX & 0xff;
            uint8_t ch = cpu->CX >> 8;

            cl = get_next_byte(cpu, memory);

            cpu->CX = cl | ch << 8;
            break;
        }
        case MOV_CH: {
            uint8_t cl = cpu->CX & 0xff;
            uint8_t ch = cpu->CX >> 8;

            ch = get_next_byte(cpu, memory);

            cpu->CX = cl | ch << 8;
            break;
        }
        case MOV_CX: {
            cpu->CX = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
            break;
        }
        case MOV_DL: {
            uint8_t dl = cpu->DX & 0xff;
            uint8_t dh = cpu->DX >> 8;

            dl = get_next_byte(cpu, memory);

            cpu->DX = dl | dh << 8;
            break;
        }
        case MOV_DH: {
            uint8_t dl = cpu->DX & 0xff;
            uint8_t dh = cpu->DX >> 8;

            dh = get_next_byte(cpu, memory);

            cpu->DX = dl | dh << 8;
            break;
        }
        case MOV_DX: {
            cpu->DX = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
            break;
        }
        case MOV_SP: {
            cpu->SP = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
            break;
        }
        case MOV_BP: {
            cpu->BP = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
            break;
        }
        case MOV_SI: {
            cpu->SI = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
            break;
        }
        case MOV_DI: {
            cpu->DI = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
            break;
        }
        case MOV_SR: {
            GPR reg = get_next_byte(cpu, memory);

            switch (reg) {
            case MOV_DS_AX: {
                cpu->DS = cpu->AX;
                break;
            }
            case MOV_DS_BX: {
                cpu->DS = cpu->BX;
                break;
            }
            case MOV_DS_CX: {
                cpu->DS = cpu->CX;
                break;
            }
            case MOV_DS_DX: {
                cpu->DS = cpu->DX;
                break;
            }
            case MOV_DS_SP: {
                cpu->DS = cpu->SP;
                break;
            }
            case MOV_DS_BP: {
                cpu->DS = cpu->BP;
                break;
            }
            case MOV_DS_SI: {
                cpu->DS = cpu->SI;
                break;
            }
            case MOV_DS_DI: {
                cpu->DS = cpu->DI;
                break;
            }
            case MOV_ES_AX: {
                cpu->ES = cpu->AX;
                break;
            }
            case MOV_ES_BX: {
                cpu->ES = cpu->BX;
                break;
            }
            case MOV_ES_CX: {
                cpu->ES = cpu->CX;
                break;
            }
            case MOV_ES_DX: {
                cpu->ES = cpu->DX;
                break;
            }
            case MOV_ES_SP: {
                cpu->ES = cpu->SP;
                break;
            }
            case MOV_ES_BP: {
                cpu->ES = cpu->BP;
                break;
            }
            case MOV_ES_SI: {
                cpu->ES = cpu->SI;
                break;
            }
            case MOV_ES_DI: {
                cpu->ES = cpu->DI;
                break;
            }
            case MOV_SS_AX: {
                cpu->SS = cpu->AX;
                break;
            }
            case MOV_SS_BX: {
                cpu->SS = cpu->BX;
                break;
            }
            case MOV_SS_CX: {
                cpu->SS = cpu->CX;
                break;
            }
            case MOV_SS_DX: {
                cpu->SS = cpu->DX;
                break;
            }
            case MOV_SS_SP: {
                cpu->SS = cpu->SP;
                break;
            }
            case MOV_SS_BP: {
                cpu->SS = cpu->BP;
                break;
            }
            case MOV_SS_SI: {
                cpu->SS = cpu->SI;
                break;
            }
            case MOV_SS_DI: {
                cpu->SS = cpu->DI;
                break;
            }
            }

            break;
        }
        case MOV_PTR: {
            if (get_next_byte(cpu, memory) != 0xc7) {
                break;
            }

            switch (get_next_byte(cpu, memory)) {
            case 0x06: {
                uint16_t offset = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
                uint16_t ch = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;

                mmu_write(memory, (cpu->ES << 4) + offset, ch & 0xff);
            }
            }
        }
        case POP_AX: {
            pop_reg(cpu, memory, AX);
            break;
        }
        case POP_BX: {
            pop_reg(cpu, memory, BX);
            break;
        }
        case POP_CX: {
            pop_reg(cpu, memory, CX);
            break;
        }
        case POP_DX: {
            pop_reg(cpu, memory, DX);
            break;
        }
        case POP_SP: {
            pop_reg(cpu, memory, SP);
            break;
        }
        case POP_BP: {
            pop_reg(cpu, memory, BP);
            break;
        }
        case POP_SI: {
            pop_reg(cpu, memory, SI);
            break;
        }
        case POP_DI: {
            pop_reg(cpu, memory, DI);
            break;
        }
        case POP_DS: {
            pop_reg(cpu, memory, DS);
            break;
        }
        case POP_ES: {
            pop_reg(cpu, memory, ES);
            break;
        }
        case POP_SS: {
            pop_reg(cpu, memory, SS);
            break;
        }
        case POPA: {
            pop_reg(cpu, memory, DI);
            pop_reg(cpu, memory, SI);
            pop_reg(cpu, memory, BP);
            /* Skip next 2 bytes of stack: preserve SP; https://shell-storm.org/x86doc/POPA_POPAD.html */
            cpu->SP += 2;

            pop_reg(cpu, memory, BX);
            pop_reg(cpu, memory, DX);
            pop_reg(cpu, memory, CX);
            pop_reg(cpu, memory, AX);
            break;
        }
        case PREFIX_80: {
            PREFIX_80_SUBTYPE sub = get_next_byte(cpu, memory);

            switch (sub) {
            case CMP_BL: {
                uint8_t to_cmp = get_next_byte(cpu, memory);
                uint8_t bl = cpu->BX & 0xff;

                if (bl == to_cmp) {
                    set_flag(cpu, ZF, 1);
                }
                break;
            }
            }

            break;
        }
        case PREFIX_FE: {
            PREFIX_FE_SUBTYPE sub = get_next_byte(cpu, memory);

            switch (sub) {
            case INC_BL: {
                uint8_t bh = cpu->BX >> 8;
                uint8_t bl = cpu->BX & 0xff;

                bl++;

                cpu->BX = bh << 8 | bl;
                break;
            }
            }

            break;
        }
        case PUSH_AX: {
            push_reg(cpu, memory, AX);
            break;
        }
        case PUSH_BX: {
            push_reg(cpu, memory, BX);
            break;
        }
        case PUSH_CX: {
            push_reg(cpu, memory, CX);
            break;
        }
        case PUSH_DX: {
            push_reg(cpu, memory, DX);
            break;
        }
        case PUSH_CS: {
            push_reg(cpu, memory, CS);
            break;
        }
        case PUSH_DS: {
            push_reg(cpu, memory, DS);
            break;
        }
        case PUSH_ES: {
            push_reg(cpu, memory, ES);
            break;
        }
        case PUSH_SS: {
            push_reg(cpu, memory, DX);
            break;
        }
        case PUSH_SP: {
            push_reg(cpu, memory, SP);
            break;
        }
        case PUSH_BP: {
            push_reg(cpu, memory, BP);
            break;
        }
        case PUSH_SI: {
            push_reg(cpu, memory, SI);
            break;
        }
        case PUSH_DI: {
            push_reg(cpu, memory, DI);
            break;
        }
        case PUSHF: {
            push_reg(cpu, memory, FLAGS);
            break;
        }
        case PUSHA: {
            push_reg(cpu, memory, AX);
            push_reg(cpu, memory, CX);
            push_reg(cpu, memory, DX);
            push_reg(cpu, memory, BX);
            push_reg(cpu, memory, SP);
            push_reg(cpu, memory, BP);
            push_reg(cpu, memory, SI);
            push_reg(cpu, memory, DI);
            break;
        }
        case RET_IMM16_NEAR: {
            uint8_t bl = get_next_byte(cpu, memory);
            uint8_t bh = get_next_byte(cpu, memory);

            /* Pop the return IP from top of the stack */
            uint8_t riph = mmu_read(memory, stack++);
            uint8_t ripl = mmu_read(memory, stack++);
            cpu->SP += 2;

            cpu->IP = riph << 8 | ripl;

            /* Pop imm16 bytes from stack */
            cpu->SP += bh << 8 | bl;
            break;
        }
        default: {
            /*uint32_t addr = (cpu->CS << 4) + cpu->IP - 1;

            printf((const uint8_t *)"=> Unknown opcode: %x\n=> Address: 0x%x\n\n", opcode, addr);*/
            break;
        }
        }
    }

    return;
}
