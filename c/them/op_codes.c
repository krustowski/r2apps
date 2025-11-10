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

    return memory->bytes[addr];
}

void switch_opcode(CPU_T *cpu, Memory_T *memory) {
    uint8_t halt = 0;

    uint8_t stack[256];

    while (!halt) {
        enum OP_CODES opcode = get_next_byte(cpu, memory);

        switch (opcode) {
        case ADD_8:
        case ADD_16: {
            enum GPR reg = get_next_byte(cpu, memory);
            uint16_t value;

            switch (opcode) {
            case ADD_8: {
                value = (uint16_t)get_next_byte(cpu, memory);
                break;
            }
            case ADD_16: {
                value = get_next_byte(cpu, memory) | get_next_byte(cpu, memory) << 8;
                break;
            }
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
            INT_BIOS int_code = get_next_byte(cpu, memory);

            switch (int_code) {
            case BOUND_FAIL: {
                handle_05h(cpu, memory);
                break;
            }
            case REAL_TIME_CLOCK_TICKS: {
                handle_08h(cpu, memory);
                break;
            }
            case KEYBOARD_INT: {
                handle_09h(cpu, memory);
                break;
            }
            case VIDEO: {
                handle_10h(cpu, memory);
                break;
            }
            case EQUIPMENT_LIST: {
                handle_11h(cpu, memory);
                break;
            }
            case MEMORY_CONVENTIONAL_SIZE: {
                handle_12h(cpu, memory);
                break;
            }
            case DISK_SERVICES: {
                handle_13h(cpu, memory);
                break;
            }
            case SERIAL_PORT_SERVICES: {
                handle_14h(cpu, memory);
                break;
            }
            case MISC_SYSTEM_SERVICES: {
                handle_15h(cpu, memory);
                break;
            }
            case KEYBOARD_SERVICES: {
                handle_16h(cpu, memory);
                break;
            }
            case PRINTER_SERVICES: {
                handle_17h(cpu, memory);
                break;
            }
            case EXEC_CASSETTE_BASIC: {
                handle_18h(cpu, memory);
                break;
            }
            case LOAD_OPERATING_SYSTEM: {
                handle_19h(cpu, memory);
                break;
            }
            case RTC_AND_PCI_SERVICES: {
                handle_1Ah(cpu, memory);
                break;
            }
            case CTRL_BREAK_HANDLER: {
                handle_1Bh(cpu, memory);
                break;
            }
            case TIMER_TICK_HANDLER: {
                handle_1Ch(cpu, memory);
                break;
            }
            case _POINTER_TO_VPT: {
                handle_1Dh(cpu, memory);
                break;
            }
            case _POINTER_TO_DPT: {
                handle_1Eh(cpu, memory);
                break;
            }
            case _POINTER_TO_VGCT: {
                handle_1Fh(cpu, memory);
                break;
            }
            case DOS_RESERVED: {
                handle_20h(cpu, memory);
                break;
            }
            case DOS_SERVICES: {
                handle_21h(cpu, memory);
                break;
            }
            case ADDRESS_POINTER_FDPT_DRV1: {
                handle_41h(cpu, memory);
                break;
            }
            case ADDRESS_POINTER_FDPT_DRV2: {
                handle_46h(cpu, memory);
                break;
            }
            case RTC_ALARM: {
                handle_4Ah(cpu, memory);
                break;
            }

            break;
            }
        case HLT: {
            printf((const uint8_t *)"=> Program stop (halt)\n");

            halt++;
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
            enum GPR reg = get_next_byte(cpu, memory);

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
        case PUSH_AX: {
            stack[cpu->SP--] = cpu->AX >> 8;
            stack[cpu->SP--] = cpu->AX & 0xff;
            break;
        }
        case PUSH_BX: {
            stack[cpu->SP--] = cpu->BX >> 8;
            stack[cpu->SP--] = cpu->BX & 0xff;
            break;
        }
        case PUSH_SP: {
            uint16_t sp = cpu->SP;

            stack[cpu->SP--] = sp >> 8;
            stack[cpu->SP--] = sp & 0xff;
            break;
        }
        default: {
            uint32_t addr = (cpu->CS << 4) + cpu->IP;

            // printf((const uint8_t *)"=> Unknown opcode: %x\n=< Address: 0x%x\n", opcode, --addr);

            // return;
            break;
        }
        }
        }
    }
