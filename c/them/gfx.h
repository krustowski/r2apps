#ifndef _THEM_GFX_INCLUDED_
#define _THEM_GFX_INCLUDED_

/*
 *  gfx.h
 *
 *  Painting the emulated screen onto the r2 display.
 *
 *  krusty@vxn.dev / Nov 24, 2025
 */

#include "mmu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Present the guest screen if it changed and enough time has passed. */
void gfx_frame(Memory_T *mem);

/* Present the guest screen now, whatever the rate limit says.  For the
 * moment the guest ends, so whatever it said last is seen. */
void gfx_flush(Memory_T *mem);

/* Force the next gfx_frame() to repaint. */
void gfx_mark_dirty(void);

/* Leave graphics mode and give the console back to r2. */
void gfx_shutdown(void);

/* What the last presented frame cost in milliseconds. A display that has
 * started eating the run shows up here before it shows up as a freeze. */
uint64_t gfx_frame_cost(void);

/* Palette reloads and the milliseconds they have cost, for the log. */
uint32_t gfx_dac_loads(void);
uint64_t gfx_dac_ms(void);

/* Called when the guest programs a new video mode. */
void gfx_mode_changed(uint8_t mode);

/* Show a line of the emulator's own text on the screen for a moment. */
void gfx_message(const uint8_t *text);

#ifdef __cplusplus
}
#endif

#endif
