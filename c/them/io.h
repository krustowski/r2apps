#ifndef _THEM_IO_INCLUDED_
#define _THEM_IO_INCLUDED_

/*
 *  io.h
 *
 *  The PC hardware the guest talks to through IN/OUT: the 8259 interrupt
 *  controller, the 8253 timer, the 8042 keyboard controller and the PC
 *  speaker. Video registers are forwarded to vga.c.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IRQ_TIMER 0
#define IRQ_KEYBOARD 1

void io_init(void);

uint8_t io_in8(uint16_t port);
void io_out8(uint16_t port, uint8_t value);
uint16_t io_in16(uint16_t port);
void io_out16(uint16_t port, uint16_t value);

/* Advance the timer and drain host key events into the emulated keyboard. */
void hw_poll(void);

/* Guest port accesses passed through to the real hardware, and the timer
 * ticks still queued for the guest.  Both are for the log. */
uint32_t hw_port_calls(void);
uint8_t hw_pit_pending(void);
uint32_t hw_pit_rate(void);

/* Highest-priority pending and unmasked IRQ, or 0xFF when there is none. */
uint8_t hw_pending_irq(void);
void hw_take_irq(uint8_t irq);
void hw_raise_irq(uint8_t irq);

/* The handler of a taken IRQ has returned through its frame; until then the
 * line is not delivered again (io.c says why).  hw_irq_busy() is the mask of
 * the lines still waiting for that. */
void hw_irq_return(uint8_t irq);
uint8_t hw_irq_busy(void);
/* Restore the holds belonging to a saved guest CPU context. */
void hw_irq_restore(uint8_t busy);

/* Milliseconds since the emulator started. */
uint64_t hw_millis(void);

/* Number of 18.2 Hz BIOS ticks elapsed so far. */
uint32_t hw_bios_ticks(void);

/* The scancode the keyboard controller currently presents on port 0x60. */
uint8_t kbd_scancode(void);

/* 1 while the user asked the emulator itself to quit (host console only). */
uint8_t hw_quit_requested(void);

#ifdef __cplusplus
}
#endif

#endif
