/* Exercise the patched timer stub in user mode. The host adaptation only
 * synthesizes the interrupt frame and replaces IRETQ with stack cleanup/RET.
 * Build with -mgeneral-regs-only so the test driver cannot alter SIMD state.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void timer_interrupt_stub(void);
static unsigned char reference[512] __attribute__((aligned(16)));
static unsigned char actual[512] __attribute__((aligned(16)));
static unsigned char newborn[512] __attribute__((aligned(16)));
static int restore_newborn;

uintptr_t scheduler_schedule(uintptr_t frame) {
    /* Simulate another process overwriting all FP state during scheduling. */
    __asm__ volatile("fxrstor64 %0" : : "m"(newborn) : "memory");
    if (restore_newborn)
        *(uintptr_t *)(frame - 8) = (uintptr_t)newborn;
    return frame;
}

static int equal_state(const unsigned char *a, const unsigned char *b) {
    /* Control word, status, tags, MXCSR, x87/MMX and all sixteen XMMs. */
    return !memcmp(a, b, 5) && !memcmp(a + 24, b + 24, 4) &&
           !memcmp(a + 32, b + 32, 128) && !memcmp(a + 160, b + 160, 256);
}

int main(void) {
    /* Same reset state constructed for a new kernel process. */
    *(uint16_t *)newborn = 0x037f;
    *(uint32_t *)(newborn + 24) = 0x1f80;
    *(uint32_t *)(newborn + 28) = 0xffff;
    __asm__ volatile("fxrstor64 %0\n\tfld1\n\tfldpi\n\tfxsave64 %1"
                     : : "m"(newborn), "m"(reference) : "memory");
    *(uint16_t *)reference = 0x077f; // Non-default x87 rounding mode.
    *(uint32_t *)(reference + 24) = 0x3f80; // Non-default SSE rounding mode.
    for (unsigned i = 160; i < 416; ++i)
        reference[i] = (unsigned char)(i * 37 + 11);

    for (unsigned i = 0; i < 10000; ++i) {
        __asm__ volatile("fxrstor64 %0" : : "m"(reference) : "memory");
        timer_interrupt_stub();
        __asm__ volatile("fxsave64 %0" : "=m"(actual) : : "memory");
        if (!equal_state(reference, actual)) {
            puts("FAIL: scheduling clobbered FP/SIMD state");
            return 1;
        }
    }
    restore_newborn = 1;
    __asm__ volatile("fxrstor64 %0" : : "m"(reference) : "memory");
    timer_interrupt_stub();
    __asm__ volatile("fxsave64 %0" : "=m"(actual) : : "memory");
    if (!equal_state(newborn, actual)) {
        puts("FAIL: initial process FP state was not restored");
        return 2;
    }
    puts("FPU context: 10000 restores preserve x87, MXCSR and XMM0-XMM15; initial state passes");
    return 0;
}
