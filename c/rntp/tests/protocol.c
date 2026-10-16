#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "protocol.h"

static void little16(uint8_t *p, uint16_t value) { p[0] = value; p[1] = value >> 8; }

static void stat_test(uint32_t mib) {
    uint32_t sectors = mib * 2048;
    uint32_t fat_sectors = mib == 2 ? 12 : mib == 64 ? 256 : 128;
    uint32_t roots = mib == 2 ? 224 : 512;
    uint32_t spc = mib == 64 ? 2 : 1;
    uint32_t clusters = (sectors - 1 - 2 * fat_sectors - roots / 16) / spc;
    uint8_t *image = calloc(sectors, 512);
    assert(image);
    little16(image + 11, 512); image[13] = spc; little16(image + 14, 1);
    image[16] = 2; little16(image + 17, roots);
    if (sectors <= 65535) little16(image + 19, sectors);
    else { little16(image + 32, sectors); little16(image + 34, sectors >> 16); }
    little16(image + 22, fat_sectors); image[510] = 0x55; image[511] = 0xaa;
    uint8_t input[RNTP_FRAME] = {0}, output[RNTP_FRAME];
    RntpHeader h = { RNTP_STAT, 0, 0, 11, 0, 9 }, reply;
    rntp_encode(input, &h);
    assert(rntp_handle(image, sectors, 9, 1, input, output) == RNTP_HEADER + RNTP_STAT_BYTES);
    assert(rntp_decode(output, &reply) && reply.op == (RNTP_STAT | RNTP_RESPONSE) && reply.status == RNTP_OK);
    assert(rntp_decode_free_space(output + RNTP_HEADER) == (uint64_t)clusters * spc * 512);
    // Allocate two entries, including a FAT12 entry spanning sector 0/1.
    for (uint32_t cluster = 340; cluster <= 341; ++cluster) {
        if (mib == 2) {
            uint32_t offset = cluster * 3 / 2;
            uint16_t pair = image[512 + offset] | ((uint16_t)image[512 + offset + 1] << 8);
            pair = cluster & 1 ? (pair & 15) | 0xfff0 : (pair & 0xf000) | 0xfff;
            little16(image + 512 + offset, pair);
        } else little16(image + 512 + cluster * 2, 0xffff);
    }
    rntp_handle(image, sectors, 9, 1, input, output);
    assert(rntp_decode_free_space(output + RNTP_HEADER) == (uint64_t)(clusters - 2) * spc * 512);
    memset(image + 512, 0, fat_sectors * 512);
    rntp_handle(image, sectors, 9, 1, input, output);
    assert(rntp_decode_free_space(output + RNTP_HEADER) == (uint64_t)clusters * spc * 512);
    image[13] = 0;
    rntp_handle(image, sectors, 9, 1, input, output);
    assert(rntp_decode(output, &reply) && reply.status == RNTP_INVALID && reply.length == 0);
    free(image);
}

static void batch_test(void) {
    uint8_t image[33 * 512] = {0}, input[RNTP_FRAME] = {0}, output[RNTP_FRAME];
    RntpHeader h = { RNTP_CAPS, 0, 0, 1, 0, 9 }, reply;
    rntp_encode(input, &h);
    assert(rntp_handle(image, 33, 9, 1, input, output) == RNTP_HEADER + 8);
    assert(rntp_get_number(output + RNTP_HEADER, 8) == RNTP_CAP_BATCH);
    h.op = RNTP_WRITE_BATCH; h.length = RNTP_BATCH_BYTES;
    rntp_encode(input, &h);
    rntp_put_number(input + RNTP_HEADER, RNTP_MAX_WRITES, 4);
    for (uint32_t i = 0; i < RNTP_MAX_WRITES; ++i) {
        uint8_t *record = input + RNTP_HEADER + 4 + i * RNTP_WRITE_RECORD;
        rntp_put_number(record, i + 1, 8);
        memset(record + 8, i + 17, 512);
    }
    assert(rntp_handle(image, 33, 9, 1, input, output) == RNTP_HEADER);
    assert(rntp_decode(output, &reply) && reply.status == RNTP_OK && reply.id == 1);
    for (uint32_t i = 0; i < RNTP_MAX_WRITES; ++i)
        for (uint32_t j = 0; j < 512; ++j) assert(image[(i + 1) * 512 + j] == i + 17);
    // A bad LAST record cannot partially apply the valid earlier ones.
    memset(image, 0, sizeof(image));
    uint8_t *last = input + RNTP_HEADER + 4 + (RNTP_MAX_WRITES - 1) * RNTP_WRITE_RECORD;
    rntp_put_number(last, 33, 8);
    rntp_handle(image, 33, 9, 1, input, output);
    assert(rntp_decode(output, &reply) && reply.status == RNTP_INVALID);
    for (uint32_t i = 0; i < sizeof(image); ++i) assert(image[i] == 0);
    rntp_put_number(last, 32, 8);
    h.generation = 8; rntp_encode(input, &h);
    rntp_handle(image, 33, 9, 1, input, output);
    assert(rntp_decode(output, &reply) && reply.status == RNTP_STALE);
    h.generation = 9; rntp_encode(input, &h);
    rntp_handle(image, 33, 9, 0, input, output);
    assert(rntp_decode(output, &reply) && reply.status == RNTP_BUSY);
    for (uint32_t count = 0; count <= 33; count += 33) {
        rntp_put_number(input + RNTP_HEADER, count, 4);
        rntp_handle(image, 33, 9, 1, input, output);
        assert(rntp_decode(output, &reply) && reply.status == RNTP_INVALID);
    }
    rntp_put_number(input + RNTP_HEADER, RNTP_MAX_WRITES, 4);
    h.length--; rntp_encode(input, &h);
    rntp_handle(image, 33, 9, 1, input, output);
    assert(rntp_decode(output, &reply) && reply.status == RNTP_INVALID);
    for (uint32_t i = 0; i < sizeof(image); ++i) assert(image[i] == 0);
}

int main(void) {
    batch_test();
    stat_test(2); stat_test(16); stat_test(64);
    uint8_t image[4096] = {0}, input[RNTP_FRAME] = {0}, output[RNTP_FRAME];
    RntpHeader h = { RNTP_HELLO, 0, 0, 0x123456789abcdef0ULL, 0, 0 }, reply;
    image[510] = 0x55; image[511] = 0xaa;
    rntp_encode(input, &h);
    assert(rntp_handle(image, 8, 9, 1, input, output) == RNTP_HEADER + RNTP_SECTOR);
    assert(rntp_decode(output, &reply) && reply.id == h.id && reply.lba == 8 && reply.generation == 9);
    assert(memcmp(output + RNTP_HEADER, image, 512) == 0);
    rntp_handle(image, 8, 9, 0, input, output);
    assert(rntp_decode(output, &reply) && reply.status == RNTP_BUSY && !reply.length);
    h.op = RNTP_WRITE; h.lba = 7; h.generation = 9; h.length = 512;
    rntp_encode(input, &h); memset(input + RNTP_HEADER, 42, 512);
    assert(rntp_handle(image, 8, 9, 1, input, output) == RNTP_HEADER);
    assert(image[7 * 512] == 42);
    h.op = RNTP_READ; h.length = 0; rntp_encode(input, &h);
    assert(rntp_handle(image, 8, 9, 1, input, output) == RNTP_HEADER + RNTP_SECTOR);
    for (int i = 0; i < 512; ++i) assert(output[RNTP_HEADER + i] == 42);
    h.lba = 8; rntp_encode(input, &h); rntp_handle(image, 8, 9, 1, input, output);
    assert(rntp_decode(output, &reply) && reply.status == RNTP_INVALID && !reply.length);
    h.op = RNTP_WRITE; h.lba = 7; h.length = 512; h.generation = 8;
    rntp_encode(input, &h); memset(input + RNTP_HEADER, 99, 512);
    rntp_handle(image, 8, 9, 1, input, output);
    assert(rntp_decode(output, &reply) && reply.status == RNTP_STALE && image[7 * 512] == 42);
    input[8] = 0xff; assert(!rntp_decode(input, &reply));
    rntp_encode(input, &h); input[12] = 1; assert(!rntp_decode(input, &reply));
    rntp_encode(input, &h); input[4] = 2; assert(!rntp_decode(input, &reply));
    return 0;
}
