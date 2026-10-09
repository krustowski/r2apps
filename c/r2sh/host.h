#ifndef R2SH_HOST_H
#define R2SH_HOST_H

/*
 *  host.h --- the shell in a window.
 *
 *      sh.elf --host 0xC1A2B0
 *
 *  With --host the shell neither prints to the console nor reads the keyboard.
 *  Its output goes into a ring in a block of memory that the host --- Memento's
 *  Shell window --- allocated on r2's user heap (0xC00_000 up, or the extension
 *  the kernel adds once that is full; mapped the same way in every process)
 *  and named on the command line, and its keys come out
 *  of a second ring in the same block.  The host draws the one as a terminal
 *  and fills the other with what is typed.
 *
 *  The output is a byte stream: printable characters, '\n', '\b' (the cursor
 *  one back), and '\f', which clears the screen.  The input is ASCII: '\n' for
 *  Enter, '\b' for Backspace, 0x1B for Escape.
 *
 *  One writer per field.  Each side bumps its beat while it is there; the
 *  shell leaves when the host's has stood still for ten seconds.  The shell
 *  also bumps its own once when it has taken the block and once after each of
 *  the next two steps of its start (bsh's mount table, the kernel's working
 *  directory): a host whose shell has said nothing can tell how far it got.
 *
 *  Memento's windows/shell_window.cpp has the same layout, and so has the
 *  Terminal of Turbo C++ 23 on r2 (tcpp/r2/src/terminal.cpp): change one,
 *  change all three, and bump the version.
 */

#include "types.h"

#define SH_HOST_MAGIC 0x42535232u /* "2RSB" */
#define SH_HOST_VERSION 1u
#define SH_OUT_SIZE 8192u
#define SH_IN_SIZE 128u

typedef struct {
    uint32_t magic;
    uint32_t version;

    /* The shell's. */
    volatile uint32_t outHead;   /* bytes written into out, ever */
    volatile uint32_t inTail;    /* bytes taken out of in, ever */
    volatile uint32_t shellBeat;
    volatile uint8_t exited;     /* the shell has left and will not touch this again */
    uint8_t pad0[3];
    uint8_t out[SH_OUT_SIZE];

    /* The host's. */
    volatile uint32_t outTail;   /* bytes the host has drawn, ever */
    volatile uint32_t inHead;    /* bytes put into in, ever */
    volatile uint32_t hostBeat;
    volatile uint8_t quit;       /* leave now */
    uint8_t pad1[3];
    uint8_t in[SH_IN_SIZE];
} ShHostBlock_T;

#endif
