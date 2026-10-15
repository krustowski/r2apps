#ifndef _THEM_WINHOST_INCLUDED_
#define _THEM_WINHOST_INCLUDED_

#ifndef __cplusplus
#include "types.h"
#endif

/*
 *  winhost.h
 *
 *  Running inside a window. Memento's THEM window (cpp/memento-hello/
 *  windows/them_window.cpp) allocates this block on the kernel's user heap
 *  and starts the emulator with
 *
 *      them.elf --host 0x<block address> /mnt/tar/games/DIR/GAME.EXE
 *
 *  From then on the emulator leaves the screen and the keyboard alone: it
 *  does not borrow the VGA, blit to the framebuffer, print to the console or
 *  read the keyboard pipe, any of which would be done over Memento's head.
 *  Instead it puts each picture in the block, already in the colours the
 *  window can show, and takes its keys from the block as the scancodes the
 *  keyboard sent. DOS console output goes into the guest's own text screen,
 *  so it shows up in the window like everything else.
 *
 *  Pictures are palette indices, one byte a pixel, at the size the guest is
 *  running (up to 640x480): the 16 EGA colours, or with `colours` 256 the
 *  same 16 followed by Memento's 6x6x6 cube and 24 greys. Two buffers, so
 *  that the one being shown is never the one being written; the rules are
 *  those of cpp/mpegplay/vidhost.hpp:
 *
 *      window:   f = front;  reading = f;  if (front != f) try again;
 *                copy pixels[f];  reading = NONE;
 *      emulator: writes the buffer that is neither front nor `reading`,
 *                then publishes it by setting `front` and bumping `frame`.
 *
 *  Keys are a ring: the window writes keys[keyHead % KEYS] and then bumps
 *  keyHead; the emulator reads while keyTail != keyHead. An extended key
 *  comes as 0xE0 and then its code, as from the keyboard.
 *
 *  Both sides beat. An emulator that hears nothing from its window for ten
 *  seconds quits; a window looks at the task table to see whether the
 *  emulator died.
 *
 *  Included from C (the eMulator) and C++ (the window): plain types only,
 *  and uint8_t and friends must already be defined by whoever includes it.
 */

#define THEMWIN_MAGIC 0x4E574854u /* "THWN" */
#define THEMWIN_VERSION 1
#define THEMWIN_MAX_W 640
#define THEMWIN_MAX_H 480
#define THEMWIN_KEYS 128
#define THEMWIN_NONE 0xFF

typedef struct {
    uint32_t magic;
    uint32_t version;

    /* ── Written by the emulator ─────────────────────────────────────────── */
    volatile uint32_t themBeat;
    volatile uint32_t frame; /* bumped each time a picture is published */
    volatile uint8_t front;  /* the buffer holding it */
    volatile uint8_t exited; /* the emulator has let go of the block */
    volatile uint8_t failed; /* it could not run the program; `message` says why */
    volatile uint8_t mode;   /* the guest's BIOS video mode, for the status line */
    volatile uint16_t width; /* of the picture in the buffers */
    volatile uint16_t height;
    volatile uint32_t keyTail; /* keys taken */
    volatile uint16_t exitCode;
    uint16_t pad0;
    char message[96];

    /* ── Written by the window ───────────────────────────────────────────── */
    volatile uint32_t hostBeat;
    volatile uint8_t reading; /* the buffer being copied, or THEMWIN_NONE */
    volatile uint8_t quit;
    uint16_t colours;          /* 16 or 256, set before the emulator starts */
    volatile uint32_t keyHead; /* keys written */
    volatile uint8_t keys[THEMWIN_KEYS];

    uint8_t pixels[2][THEMWIN_MAX_W * THEMWIN_MAX_H];
} ThemWinBlock;

/* The emulator's side. THEMWIN_BLOCK_ONLY leaves it out, for a C host that
 * only needs the layout. */
#if !defined(__cplusplus) && !defined(THEMWIN_BLOCK_ONLY)
#include "mmu.h"

/* The block, when the emulator runs in a window; null otherwise. */
extern ThemWinBlock *winhost;

/* Takes "0x..." from the command line. 1 when it names a block. */
uint8_t winhost_attach(const uint8_t *arg);

/* Keeps in touch: beats, and notices a window that has gone quiet. */
void winhost_beat(void);

/* 1 once the window asked the emulator to go, or stopped answering. */
uint8_t winhost_quit(void);

/* The next scancode the window sent, or 0 when there is none. */
uint8_t winhost_key(void);

/* Publish the guest's picture: graphics as the adapter shows it, text mode
 * drawn with the kernel font. 0 when the window is still reading the only
 * buffer that could be written, in which case the frame is to be tried
 * again. */
uint8_t winhost_publish(Memory_T *mem);

/* Publish a 320x200 picture of EGA colour indices (the emulator's own
 * messages). */
void winhost_publish_indexed(const uint8_t *frame, uint16_t w, uint16_t h);

/* Why the run could not go ahead, for the window to show. */
void winhost_fail(const uint8_t *why);

/* Last thing the emulator does: let go of the block. */
void winhost_detach(uint16_t exit_code);

/* Console output (INT 21h, INT 10h teletype): printed on the kernel console
 * normally, written into the guest's text screen in a window. */
void con_print(Memory_T *mem, const uint8_t *text);
#endif

#endif
