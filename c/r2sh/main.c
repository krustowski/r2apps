#include "printf.h"
#include "syscall.h"
#include "types.h"

#define PIPE_CAP 17
#define LINE_CAP 64

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

/* Returns pointer past the prefix if s starts with prefix, else 0. */
static const uint8_t *str_skip(const uint8_t *s, const uint8_t *prefix) {
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return s;
}

static void show_prompt(void) { print((const uint8_t *)"r2sh> "); }

static void cmd_help(void) {
    print((const uint8_t *)"Commands:\n"
                           "  help          show this message\n"
                           "  clear         clear the screen\n"
                           "  ls            list root directory\n"
                           "  run <name>    execute ELF binary by filename\n"
                           "  sysinfo       show system information\n"
                           "  exit          exit the shell\n");
}

static void cmd_sysinfo(void) {
    SysInfo_T si;
    if (read_sysinfo(&si)) {
        printf((const uint8_t *)"System:  %s\n", si.system_name);
        printf((const uint8_t *)"User:    %s\n", si.system_user);
        printf((const uint8_t *)"Path:    %s\n", si.system_path);
        printf((const uint8_t *)"Version: %s\n", si.system_version);
        printf((const uint8_t *)"Uptime:  %u ticks\n", si.system_uptime);
    } else {
        print((const uint8_t *)"sysinfo: syscall failed\n");
    }
}

static void cmd_ls(void) {
    Entry_T entries[32];

    if (list_dir(0, entries)) {
        for (uint8_t i = 0; i < 32; i++) {
            if (entries[i].name[0] == 0x00) {
                break;
            }

            print(" ");

            if (entries[i].attr & 0x10) {
                entries[i].attr = 0x00;
                printf(" %s   <DIR>\n", entries[i].name);
                continue;
            } else {
                entries[i].attr = 0x00;
                // print( entries[i].name );
            }

            printf(" %s   %d bytes\n", (const uint8_t *)(entries[i].name), entries[i].file_size);
        }
    }
}

static void cmd_run(const uint8_t *name) {
    uint8_t pid = 0;
    if (!run_elf(name, &pid)) {
        print((const uint8_t *)"run: failed to launch '");
        print(name);
        print((const uint8_t *)"'\n");
    }
}

static int dispatch(uint8_t *line, uint8_t len) {
    while (len > 0 && line[len - 1] == ' ')
        len--; /* rtrim */
    line[len] = 0;

    if (len == 0)
        return 0;

    if (str_eq(line, (const uint8_t *)"help")) {
        cmd_help();
    } else if (str_eq(line, (const uint8_t *)"clear")) {
        clear_screen();
    } else if (str_eq(line, (const uint8_t *)"ls")) {
        cmd_ls();
    } else if (str_eq(line, (const uint8_t *)"sysinfo")) {
        cmd_sysinfo();
    } else if (str_eq(line, (const uint8_t *)"exit")) {
        return 1;
    } else {
        const uint8_t *arg = str_skip(line, (const uint8_t *)"run ");
        if (arg) {
            cmd_run(arg);
        } else {
            print((const uint8_t *)"Unknown command: ");
            print(line);
            print((const uint8_t *)"\n");
        }
    }
    return 0;
}

int main(void) {
    uint8_t pipe[PIPE_CAP];
    uint8_t line[LINE_CAP];
    uint8_t llen = 0;
    uint8_t shift = 0;
    uint8_t halt = 0;

    for (uint8_t i = 0; i < PIPE_CAP; i++)
        pipe[i] = 0;

    if (!pipe_subscribe(pipe)) {
        print((const uint8_t *)"r2sh: pipe subscribe failed\n");
        return 1;
    }

    print((const uint8_t *)"r2sh - rou2ex userland shell\nType 'help' for commands.\n\n");
    show_prompt();

    while (!halt) {
        uint8_t sc = pipe[0];
        if (!sc)
            continue;
        pipe[0] = 0; /* ack — let the kernel write the next scancode */

        /* Key-release (break) codes have bit 7 set. */
        if (sc & 0x80) {
            uint8_t make = sc & 0x7f;
            if (make == 0x2a || make == 0x36)
                shift = 0; /* shift released */
            continue;
        }

        /* Shift press */
        if (sc == 0x2a || sc == 0x36) {
            shift = 1;
            continue;
        }

        /* Enter — execute line */
        if (sc == 0x1c) {
            print((const uint8_t *)"\n");
            halt = dispatch(line, llen);
            llen = 0;
            if (!halt)
                show_prompt();
            continue;
        }

        /* Backspace */
        if (sc == 0x0e) {
            if (llen > 0) {
                llen--;
                print((const uint8_t *)"\b \b");
            }
            continue;
        }

        /* Escape — discard current line */
        if (sc == 0x01) {
            while (llen > 0) {
                print((const uint8_t *)"\b \b");
                llen--;
            }
            continue;
        }

        /* Printable character */
        if (sc < 0x3a) {
            uint8_t ch = shift ? sc_shift[sc] : sc_normal[sc];
            if (ch && llen < LINE_CAP - 1) {
                line[llen++] = ch;
                printf((const uint8_t *)"%c", ch);
            }
        }
    }

    pipe_unsubscribe(pipe);
    print((const uint8_t *)"Goodbye.\n");
    return 0;
}
