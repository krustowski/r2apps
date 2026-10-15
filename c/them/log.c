#include "log.h"

#include "args.h"
#include "syscall.h"

/*
 *  log.c
 *
 *  See log.h for what this is for.
 */

/* The floppy on r2; a file beside the guest when built for the workstation
 * harness, which has no /mnt/fat to write to. */
#ifdef THEM_HOST
#define LOG_PATH ((const uint8_t *)"THEM.LOG")
#else
#define LOG_PATH ((const uint8_t *)"/mnt/fat/THEM.LOG")
#endif

/* Lines are staged here and written out together. A syscall per line would
 * cost far more than the run can afford, and the floppy is written a whole
 * sector at a time regardless. */
#define LOG_STAGE 4096

/*
 *  Often enough to be current while the emulator is wedged, rare enough not to
 *  be the thing that wedges it.
 *
 *  One per heartbeat, so a run that stops dead has its last second on the
 *  floppy rather than in a buffer that goes down with it. Allocating a
 *  cluster used to cost thousands of sector reads, which made a flush cost
 *  seconds; now that it is nine reads, a write a second is affordable.
 */
#define LOG_FLUSH_MS 1000

static uint8_t stage[LOG_STAGE];
static uint16_t staged = 0;
static uint64_t file_off = 0;
static uint64_t started = 0;
static uint64_t next_flush = 0;
static uint8_t on = 0;
static uint16_t dropped = 0;
static uint16_t wraps = 0;

/* The trace can outrun the floppy, so the file is a ring: on reaching the cap
 * it starts again from the beginning. What a freeze needs is the end of the
 * run, so it is the beginning that gives way. Each pass writes a "wrap" line,
 * and the highest number marks where the newest content begins. */
#define LOG_MAX_BYTES (160 * 1024)

uint8_t log_on(void) { return on; }

/* Characters lost to a full buffer since this was last asked, so a gap in the
 * log reads as a gap rather than as something that never happened. */
uint16_t log_dropped(void) {
    uint16_t n = dropped;

    dropped = 0;
    return n;
}

static void flush(void) {
    int64_t written;

    if (!staged)
        return;

    written = write_file_at(LOG_PATH, stage, file_off, staged);
    if (written > 0)
        file_off += (uint64_t)written;

    /*
     *  A terminator straight after, overwritten by the next flush.
     *
     *  The file is not truncated when a run starts -- the kernel's delete
     *  reports success without always removing the entry, so the previous
     *  run's tail survives past wherever this one has reached, and reads as
     *  though it belonged to it. This line says where the run actually ends;
     *  anything after it is left over from before.
     */
    {
        static const uint8_t end[] = "\n--- END OF THIS RUN ---\n";
        uint8_t n = 0;

        while (end[n])
            n++;

        write_file_at(LOG_PATH, end, file_off, n);
    }

    /* A write that failed is dropped rather than retried: the log is not worth
     * stalling the emulator over, and the gap is itself worth seeing. */
    staged = 0;
}

/*
 *  Never writes.
 *
 *  log_line() is reached from inside the interpreter -- a guest OUT lands in
 *  vga_port_write(), an INT 10h in the BIOS -- and a floppy write from there
 *  stalls the emulator inside a syscall for as long as the drive takes, with
 *  F12 dead because hw_poll() is where the key is noticed and it is never
 *  reached again. So a full buffer drops characters and says how many; only
 *  log_tick() and log_close() do I/O, and both are called from the emulator's
 *  periodic point where a syscall is already safe.
 */
static void put(uint8_t c) {
    if (staged >= LOG_STAGE) {
        if (dropped < 0xFFFF)
            dropped++;
        return;
    }

    stage[staged++] = c;
}

static void put_s(const uint8_t *s) {
    if (!s) {
        s = (const uint8_t *)"(null)";
    }

    while (*s)
        put(*s++);
}

static void put_u(uint64_t v) {
    uint8_t buf[20];
    uint8_t i = 0;

    do {
        buf[i++] = (uint8_t)('0' + (v % 10));
        v /= 10;
    } while (v && i < sizeof(buf));

    while (i--)
        put(buf[i]);
}

static void put_d(int32_t v) {
    if (v < 0) {
        put('-');
        put_u((uint64_t)(-(int64_t)v));
        return;
    }

    put_u((uint64_t)v);
}

static void put_x(uint64_t v) {
    const uint8_t *digits = (const uint8_t *)"0123456789abcdef";
    uint8_t buf[16];
    uint8_t i = 0;

    do {
        buf[i++] = digits[v & 0xF];
        v >>= 4;
    } while (v && i < sizeof(buf));

    while (i--)
        put(buf[i]);
}

void log_open(uint8_t enabled) {
    on = enabled;
    if (!on)
        return;

    started = get_ticks();
    next_flush = started + LOG_FLUSH_MS;
    staged = 0;
    dropped = 0;

    /*
     *  Each run starts the file again.
     *
     *  Carrying on after the last run meant asking the directory for the size
     *  to append at, and appending into an existing chain -- and a run started
     *  with a log already on the floppy died before it drew anything. One run
     *  per file is what the thing is for anyway: a session that misbehaves is
     *  the session being read.
     */
    {
        int64_t gone = delete_file(LOG_PATH);

        file_off = 0;
        log_line((const uint8_t *)"=== theM start === (old log removed: %u)", (unsigned int)gone);
    }
}

void log_line(const uint8_t *fmt, ...) {
    va_list ap;

    if (!on)
        return;

    put('[');
    put_u(get_ticks() - started);
    put(']');
    put(' ');

    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            put(*fmt++);
            continue;
        }

        fmt++;

        switch (*fmt) {
        case 's':
            put_s(va_arg(ap, const uint8_t *));
            break;
        case 'u':
            put_u((uint64_t)va_arg(ap, unsigned int));
            break;
        case 'l':
            put_u(va_arg(ap, uint64_t));
            break;
        case 'd':
            put_d(va_arg(ap, int));
            break;
        case 'x':
            put_x((uint64_t)va_arg(ap, unsigned int));
            break;
        case 'c':
            put((uint8_t)va_arg(ap, int));
            break;
        case '%':
            put('%');
            break;
        case 0:
            continue;
        default:
            put('%');
            put(*fmt);
            break;
        }

        fmt++;
    }

    va_end(ap);

    put('\n');
}

void log_tick(void) {
    uint64_t now;

    if (!on || !staged)
        return;

    now = get_ticks();

    /*
     *  A half-full buffer goes out at once, whatever the interval says. With
     *  the interrupt trace on, a second's worth is far more than the buffer
     *  holds, and what would be lost is the newest part -- the only part that
     *  says anything about a freeze.
     */
    if (staged < LOG_STAGE / 2 && now < next_flush)
        return;

    next_flush = now + LOG_FLUSH_MS;

    if (file_off >= LOG_MAX_BYTES) {
        file_off = 0;
        wraps++;
        log_line((const uint8_t *)"=== wrap %u ===", (unsigned int)wraps);
    }

    flush();
}

void log_close(void) {
    if (!on)
        return;

    log_line((const uint8_t *)"=== them end ===");
    flush();
    on = 0;
}
