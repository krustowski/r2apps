#ifndef _THEM_VGA_INCLUDED_
#define _THEM_VGA_INCLUDED_

/*
 *  vga.h
 *
 *  Emulation of the EGA/VGA display adapter: planar video memory, the
 *  Sequencer/Graphics Controller/Attribute Controller register files and
 *  the DAC. The emulated adapter state lives here; painting the result
 *  onto the r2 screen is the job of gfx.c.
 */

#include "mmu.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VGA_PLANE_SIZE 0x10000

/* The largest picture the emulator will present at its own size. Every mode
 * it supports fits inside this, so nothing real is sampled down any more. */
#define VGA_MAX_W 640
#define VGA_MAX_H 480

/* Called once before the CPU starts; allocates the four bit planes. */
uint8_t vga_init(void);

/* BIOS mode number currently programmed (0x03, 0x0D, 0x10, 0x12, 0x13, ...) */
uint8_t vga_mode(void);
void vga_set_mode(uint8_t mode);

/* 1 when the adapter is in a 16-colour planar mode (0x0D/0x0E/0x10/0x12) */
uint8_t vga_planar(void);
/* 1 when video memory is unchained 256-colour ("Mode X"): one byte a pixel,
 * pixel x in plane x & 3. Set up by the guest from 13h or a planar mode, so
 * it does not follow from the BIOS mode number. */
uint8_t vga_unchained(void);
/* 1 when the 0xA0000 window is the adapter's logic rather than plain RAM */
uint8_t vga_mapped(void);
/* 1 when the adapter is in any graphics mode */
uint8_t vga_graphics(void);

/* Pitch, visible width and visible height as read back from the CRTC — what
 * vga_frame() will actually decode with. */
void vga_geometry(uint16_t *pitch, uint16_t *width, uint16_t *height);

/* Running totals of guest flips of GC[5] bit 6 and SEQ[4] bit 3. */
void vga_flip_counts(uint16_t *shift256, uint16_t *chain4);

/* Video memory window 0xA0000-0xAFFFF; goes through the planar logic. */
uint8_t vga_mem_read(uint32_t addr);
void vga_mem_write(uint32_t addr, uint8_t value);

/* Adapter register files, ports 0x3C0-0x3DF. */
uint8_t vga_port_read(uint16_t port);
void vga_port_write(uint16_t port, uint8_t value);

/* Palette helpers used by the INT 10h services. */
void vga_set_dac(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void vga_get_dac(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b);
void vga_set_ac_palette(uint8_t index, uint8_t value);
uint8_t vga_get_ac_palette(uint8_t index);

/* Flatten the current video memory into a 320x200 chunky buffer of palette
 * indices. Returns the buffer; *w and *h get its size. For the mode 13h
 * fallback, which is that size and cannot be anything else. */
const uint8_t *vga_frame(Memory_T *mem, uint16_t *w, uint16_t *h);

/* The native-size picture as one byte per pixel, into max_w * max_h bytes.
 * map is a 256-entry DAC-to-output palette map, or NULL for DAC indices.
 * AC colour banking and PEL masking are resolved before applying map. */
void vga_frame_indexed(Memory_T *mem, uint8_t *dst, const uint8_t *map, uint16_t max_w, uint16_t max_h, uint16_t *w, uint16_t *h);

/* The same picture at the size the guest is running, as 32bpp 0x00RRGGBB,
 * into a buffer of at least max_w * max_h pixels. *w and *h get the size
 * actually produced: the guest's own, or max_w/max_h if it asks for more.
 * This is the path that keeps a 640x480 mode's text readable. */
void vga_frame_rgb(Memory_T *mem, uint32_t *dst, uint16_t max_w, uint16_t max_h, uint16_t *w, uint16_t *h);

/* 256 x RGB triplets matching the current palette state. */
const uint8_t *vga_palette(void);

/* One plane of the guest picture in a host planar mode's layout: dst_h rows
 * of dst_pitch bytes, to be written with that plane selected in the map mask.
 * Nothing is sampled down, so a 640x480 guest goes out at 640x480. */
void vga_plane_rows(uint8_t plane, uint8_t *dst, uint16_t dst_pitch, uint16_t dst_h);

/* The 16 colours a planar mode is showing, as RGB triplets: the attribute
 * controller's map followed by the DAC, which is what the pixel values mean. */
void vga_palette_16(uint8_t *out);

/* The 320x200 buffer vga_frame() builds into. The emulator borrows it to
 * draw its own messages when it has no guest picture to show. */
uint8_t *vga_frame_buffer(void);
uint16_t vga_frame_width(void);
uint16_t vga_frame_height(void);

/* Release the plane buffer. vga_init() allocates it again if called after. */
void vga_free(void);

/* Set when the guest touched video memory since the last frame. */
uint8_t vga_dirty(void);
void vga_clear_dirty(void);

#ifdef __cplusplus
}
#endif

#endif
