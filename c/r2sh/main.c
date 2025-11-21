#include "printf.h"
#include "syscall.h"

int main(void) {
    uint8_t buf[17];
    uint8_t cap = sizeof(buf);
    uint8_t head = 15;
    uint8_t tail = 15;

    for (uint8_t i = 0; i < sizeof(buf); i++) {
        buf[i] = 0x00;
    }

    if (!pipe_subscribe((const uint8_t *)buf)) {
        print((const uint8_t *)"-> Buffer not subscribed!\n");
    }

    for (;;) {
        while (buf[0]) {
            buf[head] = buf[0];
            buf[0] = 0x00;

            if (--head == 1) {
                head = 15;
            }
        }

        do {
            if (buf[tail]) {
                printf((const uint8_t *)"%x", buf[tail]);
                buf[tail] = 0x00;
            }

            if (--tail == 1) {
                tail = 15;
            }
        } while (tail > head);
    }

    if (!pipe_unsubscribe((const uint8_t *)buf)) {
        print((const uint8_t *)"-> Buffer not unsubscribed!\n");
    }

    return 0;
}
