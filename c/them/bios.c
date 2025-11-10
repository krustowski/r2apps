#include "int.h"
#include "mmu.h"
#include "printf.h"
#include "them_types.h"

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
    case ALTERNATE_SELECT_FUNC:
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
