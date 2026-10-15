#include "int.h"
#include "cpu.h"
#include "log.h"
#include "mmu.h"
#include "printf.h"

/*
 *  int.c
 *
 *  Dispatcher for the interrupt vectors the emulator services natively.
 *
 *  Every vector points at a three byte stub in the BIOS area: the callback
 *  opcode, the vector number, and an IRET. Reaching the callback lands
 *  here with the vector number still in the instruction stream, which is
 *  what makes hooking work -- a guest that replaces a vector simply never
 *  reaches its stub, and one that chains to the old address still does.
 *
 *  krusty@vxn.dev / Nov 4, 2025
 */

void int_bios(CPU_T *cpu, Memory_T *memory) {
    uint8_t int_code = mem_r8(memory, cpu->CS, cpu->IP);

    cpu->IP++;

    if (debug)
        log_line((const uint8_t *)"INT %x AX=%x from %x:%x", (unsigned int)int_code, (unsigned int)cpu->AX, (unsigned int)mem_r16(memory, cpu->SS, (uint16_t)(cpu->SP + 2)),
                 (unsigned int)mem_r16(memory, cpu->SS, cpu->SP));

    switch (int_code) {
    case 0x05:
        handle_05h(cpu, memory);
        break;
    case 0x08:
        handle_08h(cpu, memory);
        break;
    case 0x09:
        handle_09h(cpu, memory);
        break;
    case 0x10:
        handle_10h(cpu, memory);
        break;
    case 0x11:
        handle_11h(cpu, memory);
        break;
    case 0x12:
        handle_12h(cpu, memory);
        break;
    case 0x13:
        handle_13h(cpu, memory);
        break;
    case 0x14:
        handle_14h(cpu, memory);
        break;
    case 0x15:
        handle_15h(cpu, memory);
        break;
    case 0x16:
        handle_16h(cpu, memory);
        break;
    case 0x17:
        handle_17h(cpu, memory);
        break;
    case 0x18:
        handle_18h(cpu, memory);
        break;
    case 0x19:
        handle_19h(cpu, memory);
        break;
    case 0x1A:
        handle_1Ah(cpu, memory);
        break;
    case 0x1B:
        handle_1Bh(cpu, memory);
        break;
    case 0x1C:
        handle_1Ch(cpu, memory);
        break;
    case 0x20:
        handle_20h(cpu, memory);
        break;
    case 0x21:
        handle_21h(cpu, memory);
        break;
    case 0x27: /* terminate and stay resident */
        handle_20h(cpu, memory);
        break;
    case 0x33: /* mouse driver: report "not installed" */
        cpu->AX = 0;
        break;
    case 0x00: /* divide by zero */
    case 0x01:
    case 0x03:
    case 0x04:
    case 0x22:
    case 0x23:
    case 0x24:
    case 0x28:
    case 0x2F:
        break;
    default:
        if (debug)
            log_line((const uint8_t *)"INT %x NOT IMPLEMENTED", (unsigned int)int_code);
        break;
    }
}
