#include "io.h"
#include "printf.h"
#include "winhost.h"

/* A Memento host block driven by the workstation clock, with "help" typed
 * after startup. The BIOS/CPU/display path is the unmodified emulator. */
static ThemWinBlock block;
static uint8_t next_key;
uint8_t __real_winhost_attach(const uint8_t *arg);
void __real_winhost_beat(void);
void __real_host_dump(const void *data, unsigned long len);

uint8_t __wrap_winhost_attach(const uint8_t *arg) {
    uint8_t address[19] = {'0', 'x'};
    uint64_t value = (uint64_t)&block;
    (void)arg;
    block.magic = THEMWIN_MAGIC;
    block.version = THEMWIN_VERSION;
    block.colours = 16;
    block.reading = THEMWIN_NONE;
    block.hostBeat = 1;
    for (uint8_t i = 0; i < 16; i++)
        address[i + 2] = "0123456789abcdef"[(value >> (60 - i * 4)) & 15];
    address[18] = 0;
    return __real_winhost_attach(address);
}

void __wrap_winhost_beat(void) {
    static const uint8_t keys[] = {0x23, 0x12, 0x26, 0x19, 0x1C};
    uint64_t now = hw_millis();
    block.hostBeat++;
    while (next_key < sizeof(keys) && now >= 600u + next_key * 100u) {
        block.keys[block.keyHead % THEMWIN_KEYS] = keys[next_key++];
        block.keyHead++;
    }
    if (now >= 2200)
        block.quit = 1;
    __real_winhost_beat();
}

void __wrap_host_dump(const void *data, unsigned long len) {
    const uint8_t *ram = (const uint8_t *)data;
    __real_host_dump(data, len);
    printf((const uint8_t *)"[window] frames=%u width=%u height=%u failed=%u\n",
           block.frame, (unsigned int)block.width, (unsigned int)block.height,
           (unsigned int)block.failed);
    for (uint16_t row = 0; row < 25; row++) {
        uint8_t line[82];
        for (uint16_t col = 0; col < 80; col++) {
            uint8_t ch = ram[0xB8000 + 2 * (row * 80 + col)];
            line[col] = ch >= 32 && ch < 127 ? ch : ' ';
        }
        line[80] = '\n';
        line[81] = 0;
        printf((const uint8_t *)"%s", line);
    }
}
