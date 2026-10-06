/* Hosted fixture producer; the production encoder and r2 memcpy are linked. */
#define TJE_IMPLEMENTATION
#include "tiny_jpeg.h"
#include <stdio.h>
static uint8_t rgb[640 * 480 * 4];
static uint8_t jpeg[1024 * 1024];
static uint32_t written;
static int overflow;
static void output(void *context, void *data, int size) {
    (void)context;
    if (size <= 0 || written + (uint32_t)size > sizeof(jpeg)) { overflow = 1; return; }
    for (int i = 0; i < size; ++i) jpeg[written + i] = ((uint8_t *)data)[i];
    written += (uint32_t)size;
}
int main(int argc, char **argv) {
    const int widths[] = {1, 17, 31, 32, 640, 17};
    const int heights[] = {1, 19, 9, 32, 480, 19};
    uint32_t random = 0x6c0874af;
    if (argc != 2) return 1;
    for (int test = 0; test < 6; ++test) {
        int w = widths[test], h = heights[test], components = test == 5 ? 4 : 3;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint32_t p = (y * w + x) * components;
            rgb[p] = 93; rgb[p + 1] = 157; rgb[p + 2] = 211;
            if (test == 3) {
                rgb[p] = (x < 16 && y < 16) || (x >= 16 && y >= 16) ? 255 : 0;
                rgb[p + 1] = x >= 16 ? 255 : 0;
                rgb[p + 2] = y >= 16 ? 255 : 0;
            }
            if (test == 4) for (int c = 0; c < 3; ++c) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                rgb[p + c] = random;
            }
            if (components == 4) rgb[p + 3] = (uint8_t)(x + y);
        }
        written = 0; overflow = 0;
        if (!tje_encode_with_func(output, 0, 2, w, h, components, rgb) || overflow) return 2;
        char path[512];
        snprintf(path, sizeof(path), "%s/%d.jpg", argv[1], test);
        FILE *file = fopen(path, "wb");
        if (!file) return 3;
        if (fwrite(jpeg, 1, written, file) != written) return 4;
        fclose(file);
    }
    written = 0;
    if (tje_encode_with_func(output, 0, 2, 0, 19, 3, rgb) || written) return 5;
    puts("JPEG fixtures: solid colours, padded edges, quadrants, noise and RGBA encoded");
    return 0;
}
