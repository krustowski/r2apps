#ifndef _THEM_LOG_INCLUDED_
#define _THEM_LOG_INCLUDED_

/*
 *  log.h
 *
 *  A record of the run, left on the floppy as /mnt/fat/THEM.LOG.
 *
 *  A run that goes wrong minutes in cannot report itself to the screen: the
 *  guest owns the screen by then, and a stall shows nothing at all.  So the
 *  events worth knowing about are staged in memory and appended to the floppy
 *  every few seconds, where the telnet shell can read them while the emulator
 *  is still stuck, and where the next boot can read them if it is not.
 *
 *  Off unless the emulator was started with the word `log`, because the
 *  writing is not free.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Start logging. Picks up after whatever the last run left in the file, so
 *  several runs can be compared, and starts the file over once it has grown
 *  past the cap rather than filling the floppy.
 */
void log_open(uint8_t enabled);

/* 1 when the log is on, for callers that would rather not build a line for
 * nothing. */
uint8_t log_on(void);

/* Characters dropped because the buffer was full, cleared by the call. */
uint16_t log_dropped(void);

/*
 *  Append one line. Understands %s (NUL-terminated), %u, %d, %x and %c, and
 *  %l for a 64-bit unsigned. The timestamp is added here.
 */
void log_line(const uint8_t *fmt, ...);

/*
 *  Write out what has been staged, but no more often than every few seconds.
 *  Meant for the emulator's periodic point, where a syscall is already safe.
 */
void log_tick(void);

/* Write out whatever is left, whatever the interval says. */
void log_close(void);

#ifdef __cplusplus
}
#endif

#endif
