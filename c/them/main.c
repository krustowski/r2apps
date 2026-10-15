#include "cpu.h"
#include "gfx.h"
#include "int.h"
#include "io.h"
#include "log.h"
#include "mem.h"
#include "mmu.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"
#include "vga.h"
#include "winhost.h"

/*
 *  theM
 *
 *  Simple 16-bit CPU eMulator (x86-16). The emulator also aims to run MS-DOS
 *  native programs and games.
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

uint8_t debug = 0;

/*
 *  Diagnostics, of two kinds.
 *
 *  Once the guest owns the screen, the emulator has to draw its own words on
 *  it, because console output makes a syscall wait on the kernel's screen lock
 *  and a program that waits there from inside the interpreter takes the machine
 *  with it. gfx_message() in gfx.c is that, for whenever it is needed.
 *
 *  Nothing below has started the guest yet, though. The shell's text console
 *  is still on the screen and still readable, and a syscall from here is an
 *  ordinary one -- so a line of text is the right answer: it always appears, it
 *  leaves the console as it found it, and it does not stop for eleven seconds
 *  to be admired. Painting a frame here showed nothing at all whenever the
 *  font or the backend was not up yet, and held for those eleven seconds
 *  regardless, which is what a freeze looks like from the outside.
 */
static void complain(const uint8_t *text) {
    /* In a window there is no console of ours: the window shows the reason. */
    if (winhost) {
        winhost_fail(text);
        return;
    }
    printf((const uint8_t *)"%s\n", text);
}

/* A decimal offset, or hexadecimal with a 0x prefix, within one segment. */
static uint8_t parse_org(const uint8_t *arg, uint16_t *org) {
    uint32_t value = 0;
    uint8_t base = 10;

    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
        arg += 2;
        base = 16;
    }
    if (!*arg)
        return 0;

    for (; *arg; arg++) {
        uint8_t digit;

        if (*arg >= '0' && *arg <= '9')
            digit = (uint8_t)(*arg - '0');
        else if (*arg >= 'a' && *arg <= 'f')
            digit = (uint8_t)(*arg - 'a' + 10);
        else if (*arg >= 'A' && *arg <= 'F')
            digit = (uint8_t)(*arg - 'A' + 10);
        else
            return 0;

        if (digit >= base)
            return 0;
        value = value * base + digit;
        if (value > 0xFFFF)
            return 0;
    }

    *org = (uint16_t)value;
    return 1;
}

#define PSP_SEG 0x1000
/* The environment sits in the paragraphs just below the PSP, which is where
 * DOS puts it and which nothing else here owns: the vectors and the BIOS
 * data area end well below, and allocation starts well above. */
#define ENV_SEG 0x0F00
#define BIOS_SEG 0xF000
#define STUB_BASE 0x1000 /* offset of the callback stubs inside BIOS_SEG */

/*
 *  Point every interrupt vector at its own three byte stub in the BIOS
 *  segment. Executing a stub traps back into the emulator, so a guest that
 *  hooks a vector takes over cleanly and can still chain to the stub.
 */
static void install_vectors(Memory_T *ram) {
    for (uint16_t n = 0; n < 256; n++) {
        uint16_t off = (uint16_t)(STUB_BASE + n * 3);

        mem_w8(ram, BIOS_SEG, off, 0xF1); /* emulator callback */
        mem_w8(ram, BIOS_SEG, (uint16_t)(off + 1), (uint8_t)n);
        mem_w8(ram, BIOS_SEG, (uint16_t)(off + 2), 0xCF); /* IRET */

        mem_w16(ram, 0x0000, (uint16_t)(n * 4), off);
        mem_w16(ram, 0x0000, (uint16_t)(n * 4 + 2), BIOS_SEG);
    }
}

/*
 *  The BIOS data area the guest reads for its equipment and keyboard state.
 */
static void install_bda(Memory_T *ram) {
    mem_w16(ram, 0x0040, 0x0010, 0x0021); /* equipment: 80x25 colour, one drive */
    mem_w16(ram, 0x0040, 0x0013, 640);    /* conventional memory in KiB */
    mem_w8(ram, 0x0040, 0x0017, 0x00);    /* shift state */
    mem_w16(ram, 0x0040, 0x001A, 0x001E); /* keyboard buffer head */
    mem_w16(ram, 0x0040, 0x001C, 0x001E); /* keyboard buffer tail */
    mem_w16(ram, 0x0040, 0x0080, 0x001E); /* buffer start */
    mem_w16(ram, 0x0040, 0x0082, 0x003E); /* buffer end */
    mem_w8(ram, 0x0040, 0x0049, 0x03);    /* video mode */
    mem_w16(ram, 0x0040, 0x004A, 80);     /* columns */
    mem_w16(ram, 0x0040, 0x0063, 0x03D4); /* CRTC port */
    mem_w8(ram, 0x0040, 0x0084, 24);      /* rows - 1 */
    mem_w8(ram, 0x0040, 0x0087, 0x60);    /* EGA video control */
    mem_w8(ram, 0x0040, 0x0088, 0x09);    /* EGA switches */
}

/*
 *  A .COM program starts with a program segment prefix at offset 0 of its
 *  own segment and its image at offset 0x100.
 */
static void install_psp(Memory_T *ram, const uint8_t *tail) {
    uint8_t len = 0;

    mem_w8(ram, PSP_SEG, 0x0000, 0xCD); /* INT 20h */
    mem_w8(ram, PSP_SEG, 0x0001, 0x20);
    mem_w16(ram, PSP_SEG, 0x0002, 0xA000); /* first paragraph past the program */
    mem_w8(ram, PSP_SEG, 0x0005, 0xCD);    /* far call entry to the DOS dispatcher */
    mem_w8(ram, PSP_SEG, 0x0006, 0x21);
    mem_w8(ram, PSP_SEG, 0x0007, 0xCB);

    /* Set before loading: an origin below 0x100 may replace the PSP with
     * code, which install_env() must not overwrite afterwards. */
    mem_w16(ram, PSP_SEG, 0x002C, ENV_SEG);

    mem_w8(ram, PSP_SEG, 0x0050, 0xCD);
    mem_w8(ram, PSP_SEG, 0x0051, 0x21);
    mem_w8(ram, PSP_SEG, 0x0052, 0xCB);

    if (tail) {
        while (tail[len] && len < 0x7E) {
            mem_w8(ram, PSP_SEG, (uint16_t)(0x0081 + len), tail[len]);
            len++;
        }
    }

    mem_w8(ram, PSP_SEG, 0x0080, len);
    mem_w8(ram, PSP_SEG, (uint16_t)(0x0081 + len), 0x0D);
}

static void env_put(Memory_T *ram, uint16_t *off, const uint8_t *s) {
    while (*s)
        mem_w8(ram, ENV_SEG, (*off)++, *s++);

    mem_w8(ram, ENV_SEG, (*off)++, 0);
}

/*
 *  The environment block, and the path the program was started from.
 *
 *  A run of "NAME=VALUE" strings, a zero byte to end the run, a word, and
 *  then the program's own full path. That path is how a program finds the
 *  directory it lives in, and so how it finds the files it keeps beside
 *  itself -- the usual trick is to replace everything after the last
 *  backslash and open that.
 *
 *  Leaving the block out does not make a program skip the step. It reads
 *  segment zero as the environment instead and scans it for a variable that
 *  is not there, which is a loop with nothing to end it.
 *
 *  The path is spelled the DOS way, drive letter and backslashes, because
 *  that is what a program expects to take apart. int21.c turns it back
 *  into a kernel path on the way in, so the name a program builds from it
 *  still names a file we can find.
 */
static void install_env(Memory_T *ram, const uint8_t *prog_name) {
    uint16_t off = 0;
    uint8_t last = ':';
    uint8_t cwd[40];

    env_put(ram, &off, (const uint8_t *)"COMSPEC=C:\\COMMAND.COM");
    env_put(ram, &off, (const uint8_t *)"PATH=C:\\");
    env_put(ram, &off, (const uint8_t *)"TEMP=C:\\");

    mem_w8(ram, ENV_SEG, off++, 0);     /* no more strings */
    mem_w16(ram, ENV_SEG, off, 0x0001); /* one of what follows */
    off += 2;

    mem_w8(ram, ENV_SEG, off++, 'C');
    mem_w8(ram, ENV_SEG, off++, ':');

    /* The path of the file that was loaded. dos_load_program() made the
     * name absolute, so it carries its own directory. One that somehow did
     * not gets the working directory put in front -- asked for until the
     * kernel gives a real answer, since a program builds the names of its
     * own files out of this and the one thing it must not be is wrong. */
    if (prog_name[0] != '/') {
        dos_cwd(cwd, sizeof(cwd));

        for (uint16_t i = 0; cwd[i]; i++) {
            last = (uint8_t)(cwd[i] == '/' ? '\\' : cwd[i]);
            mem_w8(ram, ENV_SEG, off++, last);
        }

        if (last != '\\')
            mem_w8(ram, ENV_SEG, off++, '\\');
    }

    for (uint8_t i = 0; prog_name[i]; i++)
        mem_w8(ram, ENV_SEG, off++, (uint8_t)(prog_name[i] == '/' ? '\\' : prog_name[i]));

    mem_w8(ram, ENV_SEG, off++, 0);
}

/*
 *  Give back everything the run took.
 *
 *  The kernel heap is one region shared by every process and nothing
 *  reclaims a block when a process exits, so whatever is still held at the
 *  return is gone until the machine reboots. The emulator is the largest
 *  customer that heap has -- a megabyte of guest RAM, a quarter of one for
 *  the planes, and a file buffer for every handle the guest leaves open --
 *  and a few runs of it were enough to use all four megabytes up. The run
 *  after that could not allocate its planes and returned without a word.
 */
static void release(Memory_T *ram, uint8_t *pipe, uint8_t subscribed) {
    /* Last chance to get the record onto the floppy: after this the process
     * is gone and anything still staged goes with it. */
    log_close();

    /* Put the adapter back into text mode. gfx_message() leaves the screen
     * in graphics mode once it has held a message, and on the way out of a
     * failure that mode is what the kernel shell inherits: it keeps running
     * and keeps writing, but nothing it writes is ever shown again. */
    gfx_shutdown();

    if (subscribed)
        pipe_unsubscribe(pipe);

    dos_shutdown();
    vga_free();

    if (ram)
        free(ram);
}

int main(int argc, char **argv) {
    uint8_t prog_name[65] = {0};
    uint8_t cmd_tail[80] = {0};
    uint8_t tail_len = 0;
    uint8_t pipe[64];
    uint8_t keyboard = 1;
    uint8_t logging = 0;
    uint16_t com_org = 0x0100;

    CPU_T cpu = {0};
    Memory_T *ram;

    for (int i = 1; i < argc; i++) {
        if (!prog_name[0] && memcmp((uint8_t *)argv[i], (uint8_t *)"--host", 7) == 0 && i + 1 < argc) {
            /* Run in a Memento window (winhost.h). */
            winhost_attach((const uint8_t *)argv[++i]);
        } else if (!prog_name[0] && memcmp((uint8_t *)argv[i], (uint8_t *)"--org", 6) == 0) {
            if (i + 1 >= argc || !parse_org((const uint8_t *)argv[++i], &com_org)) {
                complain((const uint8_t *)"them: --org requires an offset from 0 to 0xFFFF (decimal or 0x hex)");
                winhost_detach(1);
                return 1;
            }
        } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"debug", 6) == 0) {
            debug = 1;
        } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"log", 4) == 0) {
            logging = 1;
        } else if (!prog_name[0]) {
            uint8_t j = 0;
            while (argv[i][j] && j < 64) {
                prog_name[j] = (uint8_t)argv[i][j];
                j++;
            }
            prog_name[j] = 0;
        } else {
            /* Everything after the program name becomes its command tail. */
            uint8_t j = 0;

            if (tail_len && tail_len < 78)
                cmd_tail[tail_len++] = ' ';
            while (argv[i][j] && tail_len < 78)
                cmd_tail[tail_len++] = (uint8_t)argv[i][j++];
            cmd_tail[tail_len] = 0;
        }
    }

    log_open(logging);
    log_line((const uint8_t *)"argv prog=%s tail=%s org=%x", prog_name, cmd_tail, com_org);

    /* Bring the display and the keyboard up first: together they are how
     * the emulator reports anything that goes wrong from here on. */
    if (!vga_init()) {
        /* Nothing was allocated and nothing is on screen, so the log is the
         * only place this run can say what happened to it. */
        log_close();
        complain((const uint8_t *)"theM: no memory for the video planes");
        winhost_detach(1);
        return 1;
    }

    for (uint8_t i = 0; i < 64; i++)
        pipe[i] = 0;

    /* In a window the keys come from the window, and the pipe stays with
     * Memento, which reads it. */
    if (winhost || !pipe_subscribe(pipe))
        keyboard = 0;

    /* In a window a game starts in its own directory, as it would have been
     * started on DOS; the window passes the whole path. */
    if (winhost && prog_name[0] == '/') {
        uint8_t dir[65];
        uint8_t last = 0;

        for (uint8_t i = 0; prog_name[i]; i++) {
            dir[i] = prog_name[i];
            if (prog_name[i] == '/')
                last = i;
        }
        dir[last ? last : 1] = 0;
        dos_set_cwd(dir);
    }

    if (!prog_name[0]) {
        complain((const uint8_t *)"theM: usage: them [--host 0xBLOCK] [--org OFFSET] PROGRAM [args] [debug] [log]");
        release((Memory_T *)0, pipe, keyboard);
        winhost_detach(1);
        return 1;
    }

    ram = (Memory_T *)malloc(sizeof(Memory_T));
    log_line((const uint8_t *)"malloc ram %u -> %x", (unsigned int)sizeof(Memory_T), (unsigned int)(uint64_t)ram);

    if (!ram) {
        complain((const uint8_t *)"theM: cannot allocate the emulator RAM (1 MiB)");
        release((Memory_T *)0, pipe, keyboard);
        winhost_detach(1);
        return 1;
    }

    init_ram(ram);

    install_vectors(ram);
    install_bda(ram);
    bios_disk_init(ram);
    install_psp(ram, cmd_tail);

    io_init();
    dos_init(PSP_SEG);
    vga_set_mode(0x03);

    /* Load the program and let DOS decide how: a .COM image runs flat in
     * one segment, an .EXE is relocated into place. */
    if (!dos_load_program(ram, &cpu, PSP_SEG, com_org, prog_name)) {
        log_line((const uint8_t *)"load FAILED %s", prog_name);
        uint8_t text[96];
        uint8_t n = 0;
        const uint8_t *lead = (const uint8_t *)"theM: cannot load ";

        while (lead[n]) {
            text[n] = lead[n];
            n++;
        }
        for (uint8_t i = 0; prog_name[i] && n < sizeof(text) - 1; i++)
            text[n++] = prog_name[i];
        text[n] = 0;

        complain(text);
        release(ram, pipe, keyboard);
        winhost_detach(1);
        return 1;
    }

    /* After the load, so the path names the file that was actually found
     * rather than the name the user typed without its extension. */
    log_line((const uint8_t *)"load ok %s cs=%x ip=%x", prog_name, cpu.CS, cpu.IP);

    install_env(ram, prog_name);

    if (debug)
        dump_registers(&cpu);

    switch_opcode(&cpu, ram);

    /* In a window the guest's last words are on its own screen; on the
     * console they went to the console as they were written. */
    if (winhost)
        gfx_flush(ram);

#ifdef THEM_HOST
    /* Debug aid on the workstation build: leave the whole megabyte behind so
     * any segment can be disassembled. A guest that unpacks itself and jumps
     * somewhere else -- which is most of them -- is not in its own PSP segment
     * by the time it goes wrong, so dumping only that one leaves out the code
     * actually running. */
    {
        void host_dump(const void *data, unsigned long len);
        host_dump(ram->bytes, 0x100000);
    }
#endif

    stop_speaker();

    if (debug) {
        dump_registers(&cpu);
        printf((const uint8_t *)"theM: program exited with code %d\n", dos_exit_code());
    }

    log_line((const uint8_t *)"guest exited, code %u", (unsigned int)dos_exit_code());
    log_close();

    release(ram, pipe, keyboard);

    /* The very last thing: after this the window may free the block. */
    winhost_detach(dos_exit_code());

    return 0;
}
