#include "io.h"

char *getenv(const char *name);
unsigned long strtoul(const char *value, char **end, int base);

/* Advance by ordinary polling work and inject a single host pause while
 * the DOS guest's timer handler owns its interrupt frame. */
static uint64_t clock_ms;
static uint8_t injected;
uint64_t __wrap_get_ticks(void) {
    clock_ms++;
    if (!injected && (hw_irq_busy() & 1) && hw_pit_rate() == 140) {
        const char *pause = getenv("THEM_IRQ_PAUSE_MS");
        clock_ms += pause ? strtoul(pause, (char **)0, 10) : 251;
        injected = 1;
    }
    return clock_ms;
}
