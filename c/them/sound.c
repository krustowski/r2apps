#include "sound.h"
#include "syscall.h"

uint8_t play_melody() {
    uint16_t melody[8][2] = {{0, 50}, {71, 200}, {0, 50}, {72, 200}, {0, 50}, {74, 200}, {0, 50}, {76, 400}};

    for (uint8_t i = 0; i < 8; i++) {
        play_freq(melody[i][0], melody[i][1]);
    }

    return 0;
}
