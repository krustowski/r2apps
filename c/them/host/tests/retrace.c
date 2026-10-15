#include "printf.h"
#include "vga.h"

uint8_t debug = 0;
static uint64_t now_ms;
uint64_t __wrap_hw_millis(void) { return now_ms; }

#define CHECK(expr) do { if (!(expr)) { \
    printf((const uint8_t *)"retrace regression FAIL at line %u\n", (unsigned int)__LINE__); \
    return 1; } } while (0)

/* A guest spins on IN between housekeeping passes, which refresh a cached
 * clock. Polling coarsely must still observe both ends of a retrace pulse. */
static int polling(uint16_t step) {
    uint32_t frames = 0;
    uint64_t last_rise = 0;
    uint8_t high = 0;
    now_ms = 0;
    vga_set_mode(0x0D);
    for (now_ms = 0; now_ms <= 14000; now_ms += step) {
        for (uint8_t reads = 0; reads < 64; reads++) {
            uint8_t value = vga_port_read(0x3DA);
            if ((value & 8) && !high) {
                CHECK(!frames || now_ms - last_rise >= 14);
                CHECK(!frames || now_ms - last_rise <= 14 + step);
                last_rise = now_ms;
                frames++;
            }
            high = (uint8_t)((value >> 3) & 1);
        }
    }
    CHECK(frames >= 14000u / (14 + step));
    CHECK(frames <= 1001); /* repeated INs cannot accelerate the clock */
    return 0;
}

int main(void) {
    CHECK(vga_init());
    CHECK(polling(1) == 0);
    CHECK(polling(2) == 0); /* even ms values used to miss every pulse */
    CHECK(polling(7) == 0);
    CHECK(polling(14) == 0);
    CHECK(polling(20) == 0);

    now_ms = 0;
    vga_set_mode(0x13);
    now_ms = 12;
    CHECK(!(vga_port_read(0x3DA) & 8));
    now_ms = 13;
    CHECK(vga_port_read(0x3DA) & 8);
    CHECK(vga_port_read(0x3BA) & 8); /* both status ports share the pulse */
    now_ms = 14;
    CHECK(!(vga_port_read(0x3DA) & 8));
    now_ms = 27;
    CHECK(vga_port_read(0x3DA) & 8);

    /* After a long pause expose the end of the old pulse, then one current
     * pulse. Hundreds of missed frames must not burst out back to back. */
    now_ms = 5000;
    CHECK(!(vga_port_read(0x3DA) & 8));
    CHECK(vga_port_read(0x3DA) & 8);
    for (uint8_t i = 0; i < 64; i++) CHECK(vga_port_read(0x3DA) & 8);
    now_ms = 5001;
    for (uint8_t i = 0; i < 64; i++) CHECK(!(vga_port_read(0x3DA) & 8));
    now_ms = 5014;
    CHECK(vga_port_read(0x3DA) & 8);

    /* A mode set starts a fresh retrace period and clears any held pulse. */
    vga_set_mode(0x0D);
    CHECK(!(vga_port_read(0x3DA) & 8));
    now_ms += 13;
    CHECK(vga_port_read(0x3DA) & 8);
    vga_free();
    printf((const uint8_t *)"retrace regression PASS\n");
    return 0;
}
