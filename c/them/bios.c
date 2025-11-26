#include "int.h"
#include "printf.h"

/*
 *  bios.c
 *
 *  Implementations of the software interrupts handlers.
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

/*
 *  handle_05h()
 *
 *  BOUND failure handler
 *
 *  no input
 */
void handle_05h(CPU_T *cpu, Memory_T *memory) {
    printf((const uint8_t *)"=> Interrupt 0x05: Out of bounds, address: 0x%x\n", (cpu->DS << 4) + (cpu->IP - 1));

    return;
}

/*
 *  handle_08h()
 *
 *  RTC-related handler
 *
 *  no input
 */
void handle_08h(CPU_T *cpu, Memory_T *memory) {
    printf((const uint8_t *)"=> Interrupt 0x08: RTC\n");

    return;
}

/*
 *  handle_09h()
 *
 *  Keyboard: key pressed event
 *
 *  no input
 */
void handle_09h(CPU_T *cpu, Memory_T *memory) {
    printf((const uint8_t *)"=> Interrupt 0x09: A key pressed");

    return;
}
/*
 *  handle_10h()
 *
 *  Video services
 *
 *  input: AH = service
 */
void handle_10h(CPU_T *cpu, Memory_T *memory) {
    SRV_10H service = cpu->AX >> 8;

    switch (service) {
    case SET_VIDEO_MODE: {
        break;
    }
    case SET_CURSOR_SHAPE: {
        break;
    }
    case SET_CURSOR_POSITION: {
        break;
    }
    case GET_LIGHT_PEN_POSITION:
    case GET_DISPLAY_PAGE:
    case CLEAR_SCROLL_SCREEN_UP:
    case CLEAR_SCROLL_SCREEN_DOWN:
    case READ_CHAR_WITH_ATTR_AT_CURSOR:
    case WRITE_CHAR_WITH_ATTR_AT_CURSOR:
    case WRITE_CHAR_AT_CURSOR:
    case SET_BORDER_COLOR:
    case WRITE_GRAPHICS_PIXEL:
    case READ_GRAPHICS_PIXEL:
    case WRITE_CHAR_IN_TTY_MODE:
    case GET_VIDEO_MODE:
    case SET_PALETTE_REGISTERS:
    case CHAR_GENERATOR:
    case VIDEO_SUBSYSTEM_CONFIGURATION: {
        /*
         *  AH = 12
         *
         */
        uint8_t bh = cpu->BX >> 8;
        uint8_t bl = cpu->BX & 0xff;

        switch (bl) {
        case 0x10: {
            cpu->BX = 0x0000;

            break;
        }
        }

        break;
    }
    case WRITE_STRING:
    case GET_SET_DISPLAY_COMBINATION_CODE:
    case GET_FUNC_INFO:
    case SAVE_RESTORE_VIDEO_STATE:
    case VESA_BIOS_EXT_FUNC:
    default: {
        break;
    }
    }

    return;
}

void handle_11h(CPU_T *cpu, Memory_T *memory) {}

void handle_12h(CPU_T *cpu, Memory_T *memory) {}

void handle_13h(CPU_T *cpu, Memory_T *memory) {}

void handle_14h(CPU_T *cpu, Memory_T *memory) {}

void handle_15h(CPU_T *cpu, Memory_T *memory) {}

void handle_16h(CPU_T *cpu, Memory_T *memory) {}

void handle_17h(CPU_T *cpu, Memory_T *memory) {}

void handle_18h(CPU_T *cpu, Memory_T *memory) {}

void handle_19h(CPU_T *cpu, Memory_T *memory) {}

void handle_1Ah(CPU_T *cpu, Memory_T *memory) {}

void handle_1Bh(CPU_T *cpu, Memory_T *memory) {}

void handle_1Ch(CPU_T *cpu, Memory_T *memory) {}

void handle_1Dh(CPU_T *cpu, Memory_T *memory) {}

void handle_1Eh(CPU_T *cpu, Memory_T *memory) {}

void handle_1Fh(CPU_T *cpu, Memory_T *memory) {}

void handle_41h(CPU_T *cpu, Memory_T *memory) {}

void handle_46h(CPU_T *cpu, Memory_T *memory) {}

void handle_4Ah(CPU_T *cpu, Memory_T *memory) {}
