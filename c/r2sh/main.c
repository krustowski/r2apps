#include "args.h"
#include "printf.h"
#include "syscall.h"
#include "types.h"

#include "bsh.h"
#include "host.h"

/*
 *  r2sh --- the console shell, and the shell in Memento's Shell window
 *  (--host).  Its commands are bsh's (c/bsh), the same as tnt's; what is
 *  here is the keyboard, the window's rings and the line being typed.
 */

#define PIPE_CAP 17
#define LINE_CAP 96

/*
 *  PS/2 Set 1 make-code to ASCII — unshifted.
 *  Index is the scancode byte (0x00..0x39). Zero means non-printable.
 */
static const uint8_t sc_normal[0x3a] = {
    0,    0,   '1', '2',  '3', '4', '5', '6', /* 00-07 */
    '7',  '8', '9', '0',  '-', '=', 0,   0,   /* 08-0f  0x0e=BS 0x0f=Tab */
    'q',  'w', 'e', 'r',  't', 'y', 'u', 'i', /* 10-17 */
    'o',  'p', '[', ']',  0,   0,   'a', 's', /* 18-1f  0x1c=Enter 0x1d=LCtrl */
    'd',  'f', 'g', 'h',  'j', 'k', 'l', ';', /* 20-27 */
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', /* 28-2f  0x2a=LShift */
    'b',  'n', 'm', ',',  '.', '/', 0,   0,   /* 30-37  0x36=RShift */
    0,    ' ',                                /* 38-39  0x39=Space */
};

/*
 *  PS/2 Set 1 make-code to ASCII — shifted.
 */
static const uint8_t sc_shift[0x3a] = {
    0, 0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,   0,   'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0,
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   0,   0,   ' ',
};

static int str_eq(const uint8_t *a, const uint8_t *b) {
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/*
 *  Output.  Everything the shell says goes through sh_write(): to the console,
 *  or with --host into the host's ring (host.h).
 */
static ShHostBlock_T *host = 0;
static uint32_t host_seen_beat = 0;
static uint64_t host_seen_at = 0;

/* The host is still beating; false once it has stood still for ten seconds. */
static int host_alive(void) {
    uint32_t beat = host->hostBeat;
    uint64_t now = get_ticks();
    if (beat != host_seen_beat || !host_seen_at) {
        host_seen_beat = beat;
        host_seen_at = now;
        return 1;
    }
    return now - host_seen_at < 10000;
}

static void sh_write(const uint8_t *s, uint32_t len) {
    if (!host) {
        /* print() stops at a NUL, so it gets the bytes in NUL-ended pieces. */
        uint8_t buf[128];
        uint32_t at = 0;
        for (uint32_t i = 0; i < len; i++) {
            buf[at++] = s[i];
            if (at == sizeof(buf) - 1 || i + 1 == len) {
                buf[at] = 0;
                print(buf);
                at = 0;
            }
        }
        return;
    }

    uint32_t head = host->outHead;
    for (uint32_t i = 0; i < len; i++) {
        /* A full ring waits for the host to draw; a host that has gone gets
         * nothing more. */
        while (head - host->outTail >= SH_OUT_SIZE) {
            host->outHead = head;
            if (host->quit || !host_alive())
                return;
            sleep_ms(1);
        }
        host->out[head % SH_OUT_SIZE] = s[i];
        head++;
    }
    __asm__ volatile("" ::: "memory");
    host->outHead = head;
}

static void sh_print(const uint8_t *s) {
    uint32_t n = 0;
    while (s[n])
        n++;
    sh_write(s, n);
}

static void sh_putc(uint8_t c) { sh_write(&c, 1); }

static void sh_clear(void) {
    if (host)
        sh_write((const uint8_t *)"\f", 1);
    else
        clear_screen();
}

/*  The one session, on bsh.  */
static BshSession sess;

static void r2sh_write(BshSession *s, const uint8_t *text, uint32_t len) {
    (void)s;
    sh_write(text, len);
}

static void r2sh_clear(BshSession *s) {
    (void)s;
    sh_clear();
}

/* The line being typed, and what each key does to it. */
static uint8_t line[LINE_CAP];
static uint8_t llen = 0;

/* One typed character; returns 1 when the shell should leave. */
static int on_char(uint8_t ch) {
    if (ch == '\n') {
        sh_print((const uint8_t *)"\n");
        int halt = bsh_dispatch(&sess, line, llen) == BSH_EXIT;
        llen = 0;
        if (!halt)
            bsh_prompt(&sess);
        return halt;
    }
    if (ch == '\b') {
        if (llen > 0) {
            llen--;
            sh_print((const uint8_t *)"\b \b");
        }
        return 0;
    }
    if (ch == 0x1b) { /* Escape: discard the line */
        while (llen > 0) {
            sh_print((const uint8_t *)"\b \b");
            llen--;
        }
        return 0;
    }
    if (ch >= 0x20 && ch < 0x7f && llen < LINE_CAP - 1) {
        line[llen++] = ch;
        sh_putc(ch);
    }
    return 0;
}

/* "0xC1A2B0" (or without the 0x) to its value; 0 if it is not hex. */
static uint64_t parse_hex(const uint8_t *s) {
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (!*s)
        return 0;
    for (; *s; s++) {
        uint8_t c = *s, d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return 0;
        v = v * 16 + d;
    }
    return v;
}

/* The block named by --host, if it is one: on the user heap, whole, and with
 * the magic and version this build speaks. */
static ShHostBlock_T *host_from_args(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (!str_eq((const uint8_t *)argv[i], (const uint8_t *)"--host"))
            continue;
        uint64_t addr = parse_hex((const uint8_t *)argv[i + 1]);
        if (addr < 0xC00000 || addr + sizeof(ShHostBlock_T) > 0x1000000)
            return 0;
        ShHostBlock_T *b = (ShHostBlock_T *)addr;
        if (b->magic != SH_HOST_MAGIC || b->version != SH_HOST_VERSION)
            return 0;
        return b;
    }
    return 0;
}

/* Keys from the host's ring until the shell leaves or the host goes. */
static void run_hosted(void) {
    while (!host->quit && host_alive()) {
        host->shellBeat = host->shellBeat + 1;
        uint32_t tail = host->inTail;
        int any = 0;
        while (tail != host->inHead) {
            uint8_t ch = host->in[tail % SH_IN_SIZE];
            tail++;
            host->inTail = tail;
            any = 1;
            if (on_char(ch))
                return;
        }
        if (!any)
            sleep_ms(10);
    }
}

/* Keys from the keyboard: scancodes through the kernel's pipe. */
static int run_console(void) {
    uint8_t pipe[PIPE_CAP];
    uint8_t shift = 0;
    uint8_t drain[16];

    for (uint8_t i = 0; i < PIPE_CAP; i++)
        pipe[i] = 0;

    if (!pipe_subscribe(pipe)) {
        sh_print((const uint8_t *)"r2sh: pipe subscribe failed\n");
        return 1;
    }

    for (;;) {
        uint8_t i;
        for (i = 0; i < 16; i++)
            drain[i] = 0;
        pipe_read(drain);
        if (!drain[0])
            continue;

        for (i = 0; i < 16 && drain[i]; i++) {
            uint8_t sc = drain[i];
            uint8_t ch = 0;

            /* Key-release (break) codes have bit 7 set. */
            if (sc & 0x80) {
                uint8_t make = sc & 0x7f;
                if (make == 0x2a || make == 0x36)
                    shift = 0; /* shift released */
                continue;
            }

            if (sc == 0x2a || sc == 0x36) /* Shift press */
                shift = 1;
            else if (sc == 0x1c)
                ch = '\n';
            else if (sc == 0x0e)
                ch = '\b';
            else if (sc == 0x01)
                ch = 0x1b;
            else if (sc < 0x3a)
                ch = shift ? sc_shift[sc] : sc_normal[sc];

            if (ch && on_char(ch)) {
                pipe_unsubscribe(pipe);
                return 0;
            }
        }
    }
}

int main(int argc, char **argv) {
    host = host_from_args(argc, argv);

    if (!host && argc > 1) {
        sh_print((const uint8_t *)"r2sh: started with args:");
        for (int i = 1; i < argc; i++) {
            sh_print((const uint8_t *)" ");
            sh_print((const uint8_t *)argv[i]);
        }
        sh_print((const uint8_t *)"\n");
    }

    bsh_init(&sess, r2sh_write, 0, 0);
    sess.clear = r2sh_clear;

    /*  Where the kernel's working directory is, when that is a directory: the
     *  kernel shell's `cd` before `fg sh`.  */
    {
        SysInfo_T si = {0};
        if (read_sysinfo(&si) && si.system_path[0]) {
            uint8_t path[BSH_PATH];
            uint8_t i = 0;
            while (i < 31 && i < BSH_PATH - 1 && si.system_path[i]) {
                path[i] = si.system_path[i];
                i++;
            }
            /* the kernel may leave CR/LF/space after it */
            while (i > 0 && (path[i - 1] == '\n' || path[i - 1] == '\r' || path[i - 1] == ' '))
                i--;
            path[i] = '\0';
            if (i && bsh_cd(&sess, path))
                for (uint8_t k = 0; k < BSH_PATH; k++)
                    sess.start[k] = sess.cwd[k];
        }
    }

    sh_print((const uint8_t *)"r2sh - rou2ex userland shell\nType 'help' for commands.\n");
    bsh_prompt(&sess);

    int rc = 0;
    if (host)
        run_hosted();
    else
        rc = run_console();

    sh_print((const uint8_t *)"Goodbye.\n");
    if (host) {
        __asm__ volatile("" ::: "memory");
        host->exited = 1;
    }
    return rc;
}
