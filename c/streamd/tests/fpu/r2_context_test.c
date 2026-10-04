/* Run twice on r2: bg fputest one; bg fputest two.
 * Reports to QEMU's debugcon at port 0xe9. */
#include "syscall.h"

static uint8_t reference[512] __attribute__((aligned(16)));
static uint8_t actual[512] __attribute__((aligned(16)));
static void report(const char *text) {
    for (; *text; ++text) write_port(0xe9, (uint8_t)*text);
}
static int matches(void) {
    for (uint32_t i = 0; i < 416; ++i) {
        /* FIP, FDP and reserved bytes are not part of this comparison. */
        if ((i < 5 || (i >= 24 && i < 28) || i >= 32) &&
            reference[i] != actual[i]) return 0;
    }
    return 1;
}
int main(int argc, uint8_t **argv) {
    uint32_t id = argc > 1 && argv[1][0] == 't' ? 2 : 1;
    report(id == 1 ? "ONE starting\n" : "TWO starting\n");
    /* New processes must have default controls and empty registers. */
    __asm__ volatile("fxsave64 %0" : "=m"(actual) : : "memory");
    if (*(uint16_t *)actual != 0x037f || actual[4] != 0 ||
        *(uint32_t *)(actual + 24) != 0x1f80) {
        report("FAIL initial FP state\n");
        return 1;
    }
    __asm__ volatile("fninit\n\tfld1\n\tfldpi\n\tfxsave64 %0"
                     : "=m"(reference) : : "memory");
    *(uint16_t *)reference = id == 1 ? 0x077f : 0x0b7f;
    *(uint32_t *)(reference + 24) = id == 1 ? 0x3f80 : 0x5f80;
    for (uint32_t i = 160; i < 416; ++i)
        reference[i] = (uint8_t)(i * 37 + id * 11);

    for (uint32_t i = 0; i < 300; ++i) {
        __asm__ volatile("fxrstor64 %0" : : "m"(reference) : "memory");
        sleep_ms(10);
        __asm__ volatile("fxsave64 %0" : "=m"(actual) : : "memory");
        if (!matches()) {
            report(id == 1 ? "ONE FAIL context\n" : "TWO FAIL context\n");
            return 2;
        }
    }
    report(id == 1 ? "ONE PASS 300 switches\n" : "TWO PASS 300 switches\n");
    return 0;
}
