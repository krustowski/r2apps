#include "cpu.h"
#include "int.h"
#include "mmu.h"
#include "printf.h"

void handle_05h(CPU_T *cpu, Memory_T *memory) {
    /*
     *  BOUND failure
     *
     *  no input
     */
    printf((const uint8_t *)"=> Interrupt 0x05: Out of bounds, address: 0x%x\n", (cpu->DS << 4) + (cpu->IP - 1));

    return;
}

void handle_08h(CPU_T *cpu, Memory_T *memory) {
    /*
     *  RTC
     *
     *  no input
     */
    printf((const uint8_t *)"=> Interrupt 0x08: RTC\n");

    return;
}

void handle_09h(CPU_T *cpu, Memory_T *memory) {
    /*
     *  Keyboard: key pressed event
     *
     *  no input
     */
    printf((const uint8_t *)"=> Interrupt 0x09: A key pressed");

    return;
}

void handle_10h(CPU_T *cpu, Memory_T *memory) {
    /*
     *  Video services
     *
     *  input: AH = service
     */
    SRV_10H service = cpu->AX >> 8;

    switch (service) {
    case SET_VIDEO_MODE: {
        break;
    }
    case SET_CURSOR_SHAPE: {
        break;
    }
    default:
        break;
    }

    return;
}
