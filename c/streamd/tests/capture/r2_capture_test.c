/* QEMU regression: bg captest writer; bg captest reader.
 * Present top and bottom halves 30 ms apart, then commit the full picture.
 * The reader must never accept a frame containing both colours. */
#include "syscall.h"
static uint8_t pixels[640 * 400];
static uint8_t palette[768];
static uint8_t rgb[640 * 480 * 3];
typedef struct __attribute__((packed)) {
    uint64_t pixels, palette;
    uint32_t width, height, first_row, rows;
} Frame;
static void report(const char *text) {
    for (; *text; ++text) write_port(0xe9, (uint8_t)*text);
}
static int blit(Frame *f, uint32_t command) {
    return (int)syscall(ScBlitIndexed, (int64_t)f, command, 0);
}
int main(int argc, uint8_t **argv) {
    if (argc > 1 && argv[1][0] == 'm') {
        Frame f = {(uint64_t)pixels, (uint64_t)palette, 640, 400, 0, 400};
        palette[3] = 255;
        for (uint32_t i = 0; i < sizeof(pixels); ++i) pixels[i] = 1;
        syscall(ScBlitIndexed, 0, 2, 0);
        blit(&f, 1); blit(&f, 3);
        FBCaptureInfo_T info = {0};
        if (capture_framebuffer_rgb24_scaled_if_new(rgb, 640, 480, &info) ||
            !(info.flags & FB_CAPTURE_INFO_SNAPSHOT) || !info.frame_id) {
            report("METADATA FAIL missing snapshot identity\n"); return 4;
        }
        uint64_t id = info.frame_id, stamp = info.timestamp_ms;
        for (uint32_t i = 0; i < sizeof(rgb); ++i) rgb[i] = 37;
        if (capture_framebuffer_rgb24_scaled_if_new(rgb, 640, 480, &info) != FB_CAPTURE_UNCHANGED ||
            info.frame_id != id || info.timestamp_ms != stamp) {
            report("METADATA FAIL unchanged result\n"); return 5;
        }
        for (uint32_t i = 0; i < sizeof(rgb); ++i) if (rgb[i] != 37) {
            report("METADATA FAIL copied unchanged picture\n"); return 6;
        }
        // A legacy caller may have a live value in RCX: no opt-in bit means
        // that value must not be interpreted as a metadata pointer.
        uint64_t dims = ((uint64_t)640 << 16) | 480;
        if (syscall(ScCaptureFBRGB24Scaled, (int64_t)rgb, dims, 1)) {
            report("METADATA FAIL legacy caller compatibility\n"); return 7;
        }
        report("METADATA PASS identity, timestamp, no-copy reuse and legacy ABI\n");
        return 0;
    }
    if (argc > 1 && (argv[1][0] == 'w' || argv[1][0] == 'f')) {
        int rate_test = argv[1][0] == 'f';
        Frame f = {(uint64_t)pixels, (uint64_t)palette, 640, 400, 0, 400};
        palette[3] = 255; palette[8] = 255; // 1 = red, 2 = blue.
        for (uint32_t frame = 0; frame < (rate_test ? 600U : 120U); ++frame) {
            for (uint32_t i = 0; i < sizeof(pixels); ++i)
                pixels[i] = (uint8_t)(frame % 2 + 1);
            syscall(ScBlitIndexed, 0, 2, 0);
            f.first_row = 0; f.rows = 200;
            blit(&f, frame == 0 ? 1 : 0);
            if (!rate_test) sleep_ms(30);
            f.first_row = 200; f.rows = 200;
            blit(&f, 0);
            blit(&f, 3);
            sleep_ms(rate_test ? 16 : 10);
        }
        if (rate_test) {
            int64_t length = read_file_at((const uint8_t *)"/mnt/tmp/STREAMD.LOG", rgb, 0, 4096);
            if (length > 0) for (int64_t i = 0; i < length; ++i) write_port(0xe9, rgb[i]);
        }
        report("WRITER PASS split-band presents\n");
        return 0;
    }
    uint32_t good = 0, busy = 0;
    sleep_ms(10);
    for (uint32_t frame = 0; frame < 500; ++frame) {
        int64_t status = capture_framebuffer_rgb24_scaled(rgb, 640, 480);
        if (status == FB_CAPTURE_BUSY) { ++busy; sleep_ms(1); continue; }
        if (status != 0) { report("READER FAIL capture syscall\n"); return 1; }
        // These rows lie inside the centred source at common GRUB sizes.
        uint32_t top = (120 * 640 + 320) * 3;
        uint32_t bottom = (360 * 640 + 320) * 3;
        if ((rgb[top] != 255 && rgb[top + 2] != 255) ||
            rgb[top] != rgb[bottom] || rgb[top + 2] != rgb[bottom + 2]) {
            report("READER FAIL torn/mixed frame\n");
            return 2;
        }
        ++good;
        sleep_ms(10);
    }
    if (good < 100) { report("READER FAIL capture starvation\n"); return 3; }
    report(busy ? "READER PASS complete frames, transient busy handled\n" :
                  "READER PASS complete frames without contention\n");
    return 0;
}
