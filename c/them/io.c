#include "io.h"
#include "printf.h"
#include "syscall.h"
#include "vga.h"
#include "winhost.h"

/*
 *  io.c
 *
 *  Emulated motherboard hardware.
 */

extern uint8_t debug;

/* --- 8259A interrupt controller ------------------------------------- */

static uint8_t pic_mask = 0xFC;  /* IRQ0 + IRQ1 enabled, rest masked */
static uint8_t pic_request = 0;  /* pending IRQ lines */
static uint8_t pic_icw_step = 0; /* remaining initialisation words */
static uint8_t pit0_pending = 0; /* queued timer ticks (bounded) */

/*
 *  Lines whose handler is still running: from the moment the IRQ is taken
 *  until the IRET that pops its frame (op_codes.c tells us which one that is).
 *
 *  This is not the 8259's in-service register -- a guest clears that with its
 *  EOI, and plenty of handlers send the EOI first thing.  It is what keeps the
 *  queued timer ticks from doing what a real timer never does: firing again
 *  before the previous tick's handler is done. A real 140 Hz tick is seven
 *  milliseconds away from the last one, so a handler that sends its EOI, keeps
 *  SS:SP in a fixed variable and lets a callback STI is perfectly safe there.
 *  Here, after any stall -- a frame blit, a file read -- the backlog came out
 *  back to back, the second tick landed in the first one's STI, and the saved
 *  stack pointer was overwritten with the handler's own private one. The game
 *  ran on a corrupted stack from then on.
 *
 *  Host time cannot tell us when that frame is gone: preemption or a syscall
 *  can pause an unfinished handler for any length of time. Unwinding its
 *  interrupt frame releases the line (IRET, or RETF with the flags discarded).
 *  EXEC also restores the parent's holds when it discards a child's CPU
 *  context. Abandoning a frame without either transition retains the hold.
 */
static uint8_t pic_busy = 0;

uint64_t hw_millis(void);

/* How many guest port accesses were passed through to the real hardware.
 * Each one is a syscall, so this is the figure that says whether the guest is
 * spending the run driving the speaker rather than drawing. */
static uint32_t port_calls = 0;

uint32_t hw_port_calls(void) { return port_calls; }
uint8_t hw_pit_pending(void) { return pit0_pending; }

void hw_irq_return(uint8_t irq) { pic_busy &= (uint8_t)~(1 << irq); }

uint8_t hw_irq_busy(void) { return pic_busy; }
void hw_irq_restore(uint8_t busy) { pic_busy = (uint8_t)(busy & 3); }

void hw_raise_irq(uint8_t irq) { pic_request |= (uint8_t)(1 << irq); }

uint8_t hw_pending_irq(void) {
    uint8_t live = (uint8_t)(pic_request & ~pic_mask & ~pic_busy);

    for (uint8_t i = 0; i < 8; i++)
        if ((live >> i) & 1)
            return i;

    return 0xFF;
}

void hw_take_irq(uint8_t irq) {
    pic_request &= (uint8_t)~(1 << irq);

    if (irq < 2)
        pic_busy |= (uint8_t)(1 << irq);

    if (irq == IRQ_TIMER && pit0_pending) {
        /* More ticks are waiting; re-assert the line for the next round. */
        pit0_pending--;
        if (pit0_pending)
            pic_request |= 1;
    }
}

/* --- 8253 timer ------------------------------------------------------ */

static uint16_t pit0_divisor = 0; /* 0 means 65536 */
static uint8_t pit0_latch_hi = 0;
static uint8_t pit0_access = 3; /* 1 = lo, 2 = hi, 3 = lo then hi */
static uint8_t pit0_write_hi = 0;

static uint16_t pit2_divisor = 0;
static uint8_t pit2_write_hi = 0;
static uint8_t speaker_gate = 0;

static uint64_t boot_ms = 0;
static uint64_t now_cache = 0;
static uint64_t pit_last_ms = 0;
static uint32_t pit_accum = 0; /* Hz * ms, carries the sub-tick remainder */
static uint32_t bios_tick_acc = 0;
static uint32_t bios_ticks = 0;

/* Reading the clock is a syscall, so it happens once per hw_poll() and
 * everything else in the emulator reads the cached value. */
uint64_t hw_millis(void) { return now_cache; }
uint32_t hw_bios_ticks(void) { return bios_ticks; }

static uint32_t pit0_rate(void);

uint32_t hw_pit_rate(void) { return pit0_rate(); }

static uint32_t pit0_rate(void) {
    uint32_t div = pit0_divisor ? pit0_divisor : 65536;
    uint32_t rate = 1193182u / div;

    if (!rate)
        rate = 1;
    /* Guests sometimes ask for rates the host cannot service; the emulated
     * clock still has to stay sane. */
    if (rate > 2000)
        rate = 2000;

    return rate;
}

/* --- keyboard -------------------------------------------------------- */

#define KBD_QUEUE_CAP 64

static uint8_t kbd_queue[KBD_QUEUE_CAP];
static uint8_t kbd_head = 0, kbd_tail = 0;
static uint8_t kbd_port60 = 0;
static uint8_t kbd_full = 0; /* a scancode is waiting to be read at 0x60 */
static uint64_t kbd_last_ms = 0;
static uint8_t quit_flag = 0;

uint8_t kbd_scancode(void) { return kbd_port60; }
uint8_t hw_quit_requested(void) { return quit_flag || winhost_quit(); }

static void kbd_enqueue(uint8_t sc) {
    uint8_t next = (uint8_t)((kbd_tail + 1) % KBD_QUEUE_CAP);

    /* F12 is reserved: it quits the emulator rather than reaching the guest. */
    if (sc == 0x58) {
        quit_flag = 1;
        return;
    }

    if (next == kbd_head)
        return; /* drop, the guest is not keeping up */

    kbd_queue[kbd_tail] = sc;
    kbd_tail = next;
}

/* --- port dispatch --------------------------------------------------- */

void io_init(void) {
    boot_ms = get_ticks();
    now_cache = 0;
    pit_last_ms = 0;
    pit_accum = 0;
    pic_mask = 0xFC;
    pic_request = 0;
    pic_busy = 0;
    kbd_head = kbd_tail = 0;
    kbd_full = 0;
}

uint8_t io_in8(uint16_t port) {
    switch (port) {
    case 0x20:
        return pic_request;
    case 0x21:
        return pic_mask;
    case 0x40: {
        /* Counter 0 read-back: derive the current count from elapsed time. */
        uint32_t div = pit0_divisor ? pit0_divisor : 65536;
        uint32_t pos = (uint32_t)((hw_millis() * 1193u) % div);
        uint16_t count = (uint16_t)(div - pos);

        if (pit0_latch_hi) {
            pit0_latch_hi = 0;
            return (uint8_t)(count >> 8);
        }
        pit0_latch_hi = 1;
        return (uint8_t)(count & 0xFF);
    }
    case 0x60: {
        kbd_full = 0;
        return kbd_port60;
    }
    case 0x61:
        /* Bit 4 toggles with the refresh clock; some timing loops watch it. */
        return (uint8_t)(speaker_gate | ((hw_millis() & 1) ? 0x10 : 0x00));
    case 0x64:
        return (uint8_t)(0x14 | (kbd_full ? 0x01 : 0x00));
    case 0x201:
        return 0xF0; /* no game port */
    default:
        if (port >= 0x3B0 && port <= 0x3DF)
            return vga_port_read(port);
        return 0xFF;
    }
}

void io_out8(uint16_t port, uint8_t value) {
    switch (port) {
    case 0x20:
        /* End-of-interrupt and other OCW2 commands need no state here. */
        break;
    case 0x21:
        if (pic_icw_step) {
            pic_icw_step--;
            break;
        }
        pic_mask = value;
        break;
    case 0x43: {
        uint8_t channel = (uint8_t)(value >> 6);
        uint8_t access = (uint8_t)((value >> 4) & 3);

        if (channel == 0) {
            if (access == 0) {
                pit0_latch_hi = 0; /* latch command */
            } else {
                pit0_access = access;
                pit0_write_hi = 0;
            }
        } else if (channel == 2) {
            pit2_write_hi = 0;
            /* Channel 2 drives the speaker; let the real one hear it. */
            port_calls++;
            write_port(0x43, value);
        }
        break;
    }
    case 0x40:
        if (pit0_access == 1) {
            pit0_divisor = value;
        } else if (pit0_access == 2) {
            pit0_divisor = (uint16_t)(value << 8);
        } else {
            if (!pit0_write_hi) {
                pit0_divisor = (uint16_t)((pit0_divisor & 0xFF00) | value);
                pit0_write_hi = 1;
            } else {
                pit0_divisor = (uint16_t)((pit0_divisor & 0x00FF) | (value << 8));
                pit0_write_hi = 0;
            }
        }
        break;
    case 0x42:
        if (!pit2_write_hi) {
            pit2_divisor = (uint16_t)((pit2_divisor & 0xFF00) | value);
            pit2_write_hi = 1;
        } else {
            pit2_divisor = (uint16_t)((pit2_divisor & 0x00FF) | (value << 8));
            pit2_write_hi = 0;
        }
        port_calls++;
        write_port(0x42, value);
        break;
    case 0x61: {
        uint8_t was = speaker_gate;
        speaker_gate = (uint8_t)(value & 0x03);

        if (was != speaker_gate) {
            /* Drive the host speaker the same way the guest drives its own. */
            uint32_t current = 0;
            port_calls += 2;
            read_port(0x61, &current);
            write_port(0x61, (uint32_t)((current & 0xFC) | speaker_gate));
        }
        break;
    }
    case 0x64:
        break;
    case 0xA0:
    case 0xA1:
        break;
    default:
        if (port >= 0x3B0 && port <= 0x3DF)
            vga_port_write(port, value);
        break;
    }
}

uint16_t io_in16(uint16_t port) { return (uint16_t)(io_in8(port) | (io_in8((uint16_t)(port + 1)) << 8)); }

void io_out16(uint16_t port, uint16_t value) {
    io_out8(port, (uint8_t)(value & 0xFF));
    io_out8((uint16_t)(port + 1), (uint8_t)(value >> 8));
}

/* --- periodic housekeeping ------------------------------------------- */

void hw_poll(void) {
    uint64_t now;
    uint64_t delta;

    now_cache = get_ticks() - boot_ms;
    now = now_cache;
    delta = now - pit_last_ms;

    if (delta) {
        pit_last_ms = now;

        if (delta > 250)
            delta = 250; /* a long stall must not flood the guest with ticks */

        pit_accum += (uint32_t)delta * pit0_rate();
        while (pit_accum >= 1000) {
            pit_accum -= 1000;
            if (pit0_pending < 8)
                pit0_pending++;
        }

        /* The BIOS counter at 0040:006C always runs at 18.2 Hz. */
        bios_tick_acc += (uint32_t)delta * 182;
        while (bios_tick_acc >= 10000) {
            bios_tick_acc -= 10000;
            bios_ticks++;
        }

        if (pit0_pending)
            hw_raise_irq(IRQ_TIMER);
    }

    /* Host key events -> emulated keyboard controller. Draining the pipe
     * is another syscall, so it runs a few times per millisecond at most:
     * far below anything a player can notice, and out of the way of the
     * interpreter. */
    if (now != kbd_last_ms) {
        uint8_t drain[16];

        kbd_last_ms = now;

        if (winhost) {
            /* In a window the keys come from it, and the pipe is left to
             * Memento, which is reading it. */
            uint8_t sc;

            winhost_beat();
            for (uint8_t i = 0; i < 16 && (sc = winhost_key()) != 0; i++)
                kbd_enqueue(sc);
        } else {
            for (uint8_t i = 0; i < 16; i++)
                drain[i] = 0;

            pipe_read(drain);

            for (uint8_t i = 0; i < 16 && drain[i]; i++)
                kbd_enqueue(drain[i]);
        }
    }

    if (!kbd_full && kbd_head != kbd_tail) {
        kbd_port60 = kbd_queue[kbd_head];
        kbd_head = (uint8_t)((kbd_head + 1) % KBD_QUEUE_CAP);
        kbd_full = 1;
        hw_raise_irq(IRQ_KEYBOARD);
    }
}
