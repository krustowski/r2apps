#ifndef RNTP_TEST_FIXTURE_H
#define RNTP_TEST_FIXTURE_H
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static void fixture_put(uint8_t *p, uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) { p[i] = v; v >>= 8; }
}
static uint8_t *fixture(unsigned mib) {
    uint32_t sectors = mib * 2048, fats = mib == 2 ? 12 : mib == 64 ? 256 : 128;
    uint8_t *image = calloc(sectors, 512); assert(image);
    image[0] = 0xeb; image[1] = 0x3c; image[2] = 0x90;
    memcpy(image + 3, "RNTPTEST", 8); image[21] = 0xf8; image[38] = 0x29;
    fixture_put(image + 39, 123, 4); memcpy(image + 43, "NO NAME    ", 11);
    memcpy(image + 54, mib == 2 ? "FAT12   " : "FAT16   ", 8);
    fixture_put(image + 11, 512, 2); image[13] = mib == 64 ? 2 : 1;
    fixture_put(image + 14, 1, 2); image[16] = 2;
    fixture_put(image + 17, mib == 2 ? 224 : 512, 2);
    fixture_put(image + (sectors <= 65535 ? 19 : 32), sectors, sectors <= 65535 ? 2 : 4);
    fixture_put(image + 22, fats, 2); image[510] = 0x55; image[511] = 0xaa;
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *f = image + (1 + i * fats) * 512;
        f[0] = 0xf8; f[1] = f[2] = 0xff; if (mib != 2) f[3] = 0xff;
    }
    return image;
}
#endif
