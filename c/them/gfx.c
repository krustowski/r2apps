#include "gfx.h"
#include "mmu.h"
#include "printf.h"
#include "syscall.h"

const uint32_t TEXT_MODE_BASE = 0xB8000;
const uint8_t TEXT_MODE_ROWS = 25;
const uint8_t TEXT_MODE_COLS = 80;

void render_text_mode(Memory_T *mem) {
    clear_screen();

    uint8_t screen[TEXT_MODE_ROWS * TEXT_MODE_COLS];

    for (uint8_t row = 0; row < TEXT_MODE_ROWS; row++) {
        for (uint8_t col = 0; col < TEXT_MODE_COLS; col++) {
            uint32_t offset = TEXT_MODE_BASE + 2 * (row * TEXT_MODE_COLS + col);

            uint8_t ch = mmu_read(mem, offset);
            uint8_t attr = mmu_read(mem, offset + 1);

            uint8_t fg = attr & 0x0f;
            uint8_t bg = (attr >> 4) & 0x07;

            screen[row * TEXT_MODE_COLS + col] = ch;

            if (!ch) {
                mem->render_lock = 1;
                mmu_write(mem, offset, ' ');
                mmu_write(mem, offset + 1, ' ');
                screen[row * TEXT_MODE_COLS + col] = ' ';
                mem->render_lock = 0;
            }
        }
    }

    printf((const uint8_t *)"%s", screen);

    return;
}
