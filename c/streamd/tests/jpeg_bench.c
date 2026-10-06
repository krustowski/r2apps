#include <stdio.h>
#include <time.h>
#define TJE_IMPLEMENTATION
#include "tiny_jpeg.h"
static uint8_t rgb[640 * 480 * 3];
static uint8_t jpeg[1024 * 1024];
static uint32_t jpeg_size;
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
}
static void output(void *context, void *data, int size) {
    (void)context;
    if (size < 0 || jpeg_size + (uint32_t)size > sizeof(jpeg)) return;
    for (int i = 0; i < size; i++) jpeg[jpeg_size + i] = ((uint8_t *)data)[i];
    jpeg_size += (uint32_t)size;
}
static void make_scene(int noisy) {
    uint32_t random = 0x6c0874af;
    for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) {
        int i = (y * 640 + x) * 3;
        if (noisy) {
            for (int c = 0; c < 3; c++) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                rgb[i + c] = random;
            }
        } else {
            int window = x >= 60 && x < 580 && y >= 50 && y < 430;
            int title = window && y < 76;
            int text = window && !title && y % 16 < 9 && x % 10 < 5;
            rgb[i] = title ? 30 : text ? 24 : window ? 224 : (uint8_t)(x / 4);
            rgb[i+1] = title ? 50 : text ? 24 : window ? 224 : (uint8_t)(y / 3);
            rgb[i+2] = title ? 110 : text ? 24 : window ? 224 : 96;
        }
    }
}
int main(void) {
    for (int scene = 0; scene < 2; scene++) {
        make_scene(scene);
        for (int quality = 1; quality <= 2; quality++) {
            jpeg_size = 0;
            if (!tje_encode_with_func(output, 0, quality, 640, 480, 3, rgb)) return 1;
            double best = 1e30;
            for (int round = 0; round < 3; round++) {
                double start = now_ms();
                for (int n = 0; n < 12; n++) {
                    jpeg_size = 0;
                    if (!tje_encode_with_func(output, 0, quality, 640, 480, 3, rgb)) return 2;
                }
                double elapsed = (now_ms() - start) / 12;
                if (elapsed < best) best = elapsed;
            }
            uint64_t hash = 14695981039346656037UL;
            for (uint32_t n = 0; n < jpeg_size; n++) { hash ^= jpeg[n]; hash *= 1099511628211UL; }
            printf("scene=%s quality=%d encode_ms=%.3f bytes=%u hash=%016lx\n",
                scene ? "noise" : "synthetic-desktop", quality, best, jpeg_size, hash);
        }
    }
    return 0;
}
