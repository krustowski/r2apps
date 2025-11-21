#include "int.h"
#include "cpu.h"
#include "mmu.h"
#include "printf.h"

void int_bios(CPU_T *cpu, Memory_T *memory) {
    INT_BIOS int_code = mmu_read(memory, (cpu->CS << 4) + cpu->IP++);

    printf((const uint8_t *)"=> Interrupt 0x%x\n", int_code);

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
    default: {
        printf((const uint8_t *)"=> Interrupt not implemented\n");
        break;
    }
    }

    return;
}
