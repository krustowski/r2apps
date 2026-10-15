#include "cpu.h"
#include "gfx.h"
#include "io.h"
#include "printf.h"
#include "syscall.h"
#include "vga.h"
#include "winhost.h"

uint8_t debug = 0;
static Memory_T ram;
static ThemWinBlock block;
static uint64_t wall_ms = 1000;
static uint64_t frame_delay;
static uint32_t clock_calls, blits, passes;
static uint8_t interpreter;

#define CHECK(expr) do { if (!(expr)) { \
    printf((const uint8_t *)"pacing regression FAIL at line %u\n", (unsigned int)__LINE__); \
    return 1; } } while (0)

uint64_t __wrap_get_ticks(void) { clock_calls++; return wall_ms; }
uint64_t __wrap_hw_millis(void) { return wall_ms - 1000; }
uint8_t __real_winhost_publish(Memory_T *mem);
uint8_t __wrap_winhost_publish(Memory_T *mem) {
    /* Elapsed wall time includes time spent scheduled in another process. */
    wall_ms += frame_delay;
    return __real_winhost_publish(mem);
}
int64_t __wrap_blit_buffer_scaled(const uint32_t *pixels, uint32_t w, uint32_t h) {
    (void)pixels; (void)w; (void)h;
    wall_ms += frame_delay;
    blits++;
    return 1;
}
uint8_t __real_hw_quit_requested(void);
uint8_t __wrap_hw_quit_requested(void) {
    return interpreter ? ++passes == 3 : __real_hw_quit_requested();
}

int main(int argc, char **argv) {
    CHECK(vga_init());
    io_init();
    if (argc > 1 && argv[1][0] == 'c') {
        CPU_T cpu = {0};
        /* A running guest with no interrupts or changing screen. Diagnostics
         * are off: polling should not make their extra clock syscalls. */
        ram.bytes[0] = 0xEB; ram.bytes[1] = 0xFE; /* JMP $ */
        interpreter = 1;
        clock_calls = 0;
        switch_opcode(&cpu, &ram);
        CHECK(passes == 3);
        CHECK(clock_calls <= passes * 2);
    } else if (argc > 1) {
        /* Expensive full-screen blits still need the 25% display budget. */
        vga_set_mode(0x0D);
        frame_delay = 40;
        wall_ms = 1020;
        gfx_frame(&ram);
        CHECK(blits == 1 && wall_ms == 1060 && gfx_frame_cost() == 40);
        gfx_mark_dirty();
        frame_delay = 0;
        wall_ms = 1179;
        gfx_frame(&ram);
        CHECK(blits == 1);
        wall_ms = 1180;
        gfx_frame(&ram);
        CHECK(blits == 2);
    } else {
        block.reading = THEMWIN_NONE;
        block.colours = 16;
        winhost = &block;
        vga_set_mode(0x0D);
        wall_ms = 1020;
        gfx_frame(&ram);
        CHECK(block.frame == 1);
        gfx_mark_dirty();
        wall_ms = 1039;
        gfx_frame(&ram);
        CHECK(block.frame == 1); /* at most one start every 20 ms */
        frame_delay = 25;
        wall_ms = 1040;
        gfx_frame(&ram);
        CHECK(block.frame == 2 && wall_ms == 1065 && gfx_frame_cost() == 25);
        frame_delay = 0;
        gfx_mark_dirty();
        wall_ms = 1085;
        gfx_frame(&ram);
        CHECK(block.frame == 3); /* no extra 75 ms penalty after preemption */
        gfx_mark_dirty();
        wall_ms = 1104;
        gfx_frame(&ram);
        CHECK(block.frame == 3);
        wall_ms = 1105;
        gfx_frame(&ram);
        CHECK(block.frame == 4);
        block.reading = (uint8_t)(block.front ^ 1);
        gfx_mark_dirty();
        wall_ms = 1125;
        gfx_frame(&ram);
        CHECK(block.frame == 4);
        block.reading = THEMWIN_NONE;
        wall_ms = 1145;
        gfx_frame(&ram);
        CHECK(block.frame == 5); /* a busy reader leaves the frame pending */
        winhost = (ThemWinBlock *)0;
    }
    vga_free();
    printf((const uint8_t *)"pacing regression PASS\n");
    return 0;
}
