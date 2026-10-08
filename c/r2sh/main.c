#include "args.h"
#include "printf.h"
#include "syscall.h"
#include "types.h"

#include "host.h"

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

/* Copy src into dst, NUL-terminate, return length written (max dst_cap-1). */
static uint8_t str_copy(uint8_t *dst, const uint8_t *src, uint8_t dst_cap) {
    uint8_t i = 0;
    while (src[i] && i < dst_cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

/*
 *  Output.  Everything the shell says goes through sh_write(): to the console,
 *  or with --host into the host's ring (host.h).  printf() cannot be pointed
 *  anywhere but the console, so the shell formats with sh_printf() instead.
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

/* The part of printf() the shell uses: %s %c %d %u %x and %%. */
static void sh_printf(const uint8_t *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    uint8_t num[24];

    for (; *fmt; fmt++) {
        if (*fmt != '%' || !fmt[1]) {
            sh_putc(*fmt);
            continue;
        }
        fmt++;
        switch (*fmt) {
            case 's': {
                const uint8_t *str = va_arg(args, const uint8_t *);
                sh_print(str ? str : (const uint8_t *)"(null)");
                break;
            }
            case 'c':
                sh_putc((uint8_t)va_arg(args, int));
                break;
            case 'd':
            case 'u':
            case 'x': {
                uint32_t base = *fmt == 'x' ? 16 : 10;
                int64_t v = *fmt == 'd' ? (int64_t)va_arg(args, int) : (int64_t)va_arg(args, unsigned int);
                uint8_t n = 0;
                if (v < 0) {
                    sh_putc('-');
                    v = -v;
                }
                do {
                    num[n++] = (uint8_t)"0123456789abcdef"[(uint64_t)v % base];
                    v = (int64_t)((uint64_t)v / base);
                } while (v);
                while (n)
                    sh_putc(num[--n]);
                break;
            }
            default:
                sh_putc(*fmt);
                break;
        }
    }

    va_end(args);
}

static void sh_clear(void) {
    if (host)
        sh_putc('\f');
    else
        clear_screen();
}

/* Per-session current working directory (absolute VFS path). */
static uint8_t cwd[64] = "/mnt/fat";

/* Build an absolute path from cwd + arg.  If arg is already absolute, use it
   directly.  Result is always NUL-terminated and stored in out[64]. */
static void build_abs_path(uint8_t out[64], const uint8_t *arg) {
    if (!arg || !arg[0]) {
        str_copy(out, cwd, 64);
        return;
    }
    if (arg[0] == '/') {
        str_copy(out, arg, 64);
        return;
    }
    /* relative: cwd + "/" + arg */
    uint8_t i = str_copy(out, cwd, 64);
    if (i < 63) { out[i++] = '/'; out[i] = '\0'; }
    while (*arg && i < 63) { out[i++] = *arg++; }
    out[i] = '\0';
}

static void show_prompt(void) {
    sh_print((const uint8_t *)"[");
    sh_print(cwd);
    sh_print((const uint8_t *)"]> ");
}

static void cmd_help(void) {
    sh_print((const uint8_t *)"Commands:\n"
                           "  help            show this message\n"
                           "  clear           clear the screen\n"
                           "  ls [path]       list directory (default: cwd)\n"
                           "  cd <path>       change directory\n"
                           "  read <path>     print file contents\n"
                           "  rm <path>       delete a file\n"
                           "  mkdir <name>    create subdirectory in cwd\n"
                           "  mount           show VFS mount table\n"
                           "  run <name>      execute ELF binary by filename\n"
                           "  sysinfo         show system information\n"
                           "  exit            exit the shell\n");
}

static void cmd_mount(void) {
    MountInfo_T mounts[8];
    int64_t count = list_mounts(mounts);
    if (count <= 0) {
        sh_print((const uint8_t *)"No mounts.\n");
        return;
    }
    for (int64_t i = 0; i < count; i++) {
        uint8_t len = mounts[i].path_len;
        for (uint8_t j = 0; j < len; j++)
            sh_printf((const uint8_t *)"%c", mounts[i].path[j]);
        const uint8_t *fsname;
        switch (mounts[i].fs_type) {
            case 1:  fsname = (const uint8_t *)"rootfs";  break;
            case 2:  fsname = (const uint8_t *)"fat12";   break;
            case 3:  fsname = (const uint8_t *)"iso9660"; break;
            case 4:  fsname = (const uint8_t *)"tar";     break;
            case 5:  fsname = (const uint8_t *)"memdisk"; break; /* FAT12 in RAM */
            default: fsname = (const uint8_t *)"unknown"; break;
        }
        sh_printf((const uint8_t *)" (%s)\n", fsname);
    }
}

static void cmd_sysinfo(void) {
    SysInfo_T si;
    if (read_sysinfo(&si)) {
        uint32_t up = si.system_uptime;
        uint32_t up_h = up / 3600;
        uint32_t up_m = (up % 3600) / 60;
        uint32_t up_s = up % 60;
        sh_printf((const uint8_t *)"System:  %s\n", si.system_name);
        sh_printf((const uint8_t *)"User:    %s\n", si.system_user);
        sh_printf((const uint8_t *)"Path:    %s\n", si.system_path);
        sh_printf((const uint8_t *)"Version: %s\n", si.system_version);
        sh_printf((const uint8_t *)"Uptime:  %uh %um %us\n", up_h, up_m, up_s);
    } else {
        sh_print((const uint8_t *)"sysinfo: syscall failed\n");
    }
}

static void cmd_ls(const uint8_t *path_arg) {
    uint8_t abs[64];
    build_abs_path(abs, path_arg);

    /* 64 entries: syscall 0x2D takes no capacity argument and writes one per
     * directory member, up to 64.  A 32-entry buffer is overrun silently. */
    VfsDirEntry_T entries[64];
    int64_t count = list_dir_path(abs, entries);

    if (count < 0 || count > 64) {
        sh_print((const uint8_t *)"ls: no such directory: ");
        sh_print(abs);
        sh_print((const uint8_t *)"\n");
        return;
    }

    for (int64_t i = 0; i < count; i++) {
        VfsDirEntry_T *e = &entries[i];
        sh_print((const uint8_t *)"  ");

        /* name is not NUL-terminated; print byte-by-byte */
        for (uint8_t j = 0; j < e->name_len; j++)
            sh_printf((const uint8_t *)"%c", e->name[j]);

        if (e->is_dir) {
            sh_print((const uint8_t *)"/  <DIR>\n");
        } else {
            sh_printf((const uint8_t *)"  %d bytes\n", e->size);
        }
    }

    if (count == 0)
        sh_print((const uint8_t *)"  (empty)\n");
}

static void cmd_cd(const uint8_t *path_arg) {
    if (!path_arg || !path_arg[0]) {
        sh_print((const uint8_t *)"cd: usage: cd <path>\n");
        return;
    }

    /* Handle "cd .." — trim last path component then sync kernel. */
    if (path_arg[0] == '.' && path_arg[1] == '.' && path_arg[2] == '\0') {
        uint8_t i = 0;
        while (cwd[i]) i++;
        if (i == 0) return;
        uint8_t last_slash = 0;
        for (uint8_t j = 0; j < i; j++) if (cwd[j] == '/') last_slash = j;
        if (last_slash == 0) {
            cwd[0] = '/'; cwd[1] = '\0';
        } else {
            cwd[last_slash] = '\0';
        }
        chdir(cwd);
        return;
    }

    uint8_t abs[64];
    build_abs_path(abs, path_arg);

    /* Verify the target exists as a directory via the kernel. */
    VfsDirEntry_T tmp[64];
    int64_t r = list_dir_path(abs, tmp);
    if (r < 0 || r > 64) {
        sh_print((const uint8_t *)"cd: no such directory: ");
        sh_print(abs);
        sh_print((const uint8_t *)"\n");
        return;
    }

    str_copy(cwd, abs, 64);
    chdir(abs);
}

static void cmd_read(const uint8_t *path_arg) {
    if (!path_arg || !path_arg[0]) {
        sh_print((const uint8_t *)"read: usage: read <file>\n");
        return;
    }

    uint8_t abs[64];
    build_abs_path(abs, path_arg);

    static uint8_t file_buf[4096];
    for (uint16_t i = 0; i < (uint16_t)sizeof(file_buf); i++) file_buf[i] = 0;

    int64_t r = read_file(abs, file_buf);
    if (!r) {
        sh_print((const uint8_t *)"read: failed to open '");
        sh_print(abs);
        sh_print((const uint8_t *)"'\n");
        return;
    }

    uint16_t len = 0;
    while (len < (uint16_t)sizeof(file_buf) && file_buf[len]) len++;

    if (len == 0) {
        sh_print((const uint8_t *)"(empty file)\n");
        return;
    }

    for (uint16_t j = 0; j < len; j++)
        sh_printf((const uint8_t *)"%c", file_buf[j]);
    sh_print((const uint8_t *)"\n");
}

static void cmd_rm(const uint8_t *path_arg) {
    if (!path_arg || !path_arg[0]) {
        sh_print((const uint8_t *)"rm: usage: rm <file>\n");
        return;
    }

    uint8_t abs[64];
    build_abs_path(abs, path_arg);

    int64_t r = delete_file(abs);
    if (!r) {
        sh_print((const uint8_t *)"rm: failed to delete '");
        sh_print(abs);
        sh_print((const uint8_t *)"'\n");
    }
}

static void cmd_mkdir(const uint8_t *name) {
    if (!name || !name[0]) {
        sh_print((const uint8_t *)"mkdir: usage: mkdir <name>\n");
        return;
    }
    int64_t r = write_subdir(cwd, name);
    if (!r) {
        sh_print((const uint8_t *)"mkdir: failed to create '");
        sh_print(name);
        sh_print((const uint8_t *)"'\n");
    }
}

static void cmd_run(const uint8_t *name) {
    uint8_t pid = 0;
    if (!run_elf(name, name, &pid)) {
        sh_print((const uint8_t *)"run: failed to launch '");
        sh_print(name);
        sh_print((const uint8_t *)"'\n");
        return;
    }
    /* A program's own output goes to the console; only the shell's comes
     * through the window. */
    if (host)
        sh_printf((const uint8_t *)"run: started %s as PID %u (its output goes to the console)\n", name,
                  (unsigned int)pid);
}

static int dispatch(uint8_t *line, uint8_t len) {
    while (len > 0 && line[len - 1] == ' ')
        len--; /* rtrim */
    line[len] = 0;

    if (len == 0)
        return 0;

    const uint8_t *arg;

    if (str_eq(line, (const uint8_t *)"help")) {
        cmd_help();
    } else if (str_eq(line, (const uint8_t *)"clear")) {
        sh_clear();
    } else if (str_eq(line, (const uint8_t *)"ls")) {
        cmd_ls((const uint8_t *)0);
    } else if ((arg = str_skip(line, (const uint8_t *)"ls "))) {
        cmd_ls(arg);
    } else if ((arg = str_skip(line, (const uint8_t *)"cd "))) {
        cmd_cd(arg);
    } else if ((arg = str_skip(line, (const uint8_t *)"read "))) {
        cmd_read(arg);
    } else if ((arg = str_skip(line, (const uint8_t *)"rm "))) {
        cmd_rm(arg);
    } else if ((arg = str_skip(line, (const uint8_t *)"mkdir "))) {
        cmd_mkdir(arg);
    } else if (str_eq(line, (const uint8_t *)"mount")) {
        cmd_mount();
    } else if (str_eq(line, (const uint8_t *)"sysinfo")) {
        cmd_sysinfo();
    } else if (str_eq(line, (const uint8_t *)"exit")) {
        return 1;
    } else {
        arg = str_skip(line, (const uint8_t *)"run ");
        if (arg) {
            cmd_run(arg);
        } else {
            sh_print((const uint8_t *)"Unknown command: ");
            sh_print(line);
            sh_print((const uint8_t *)"\n");
        }
    }
    return 0;
}

/* The line being typed, and what each key does to it. */
static uint8_t line[LINE_CAP];
static uint8_t llen = 0;

/* One typed character; returns 1 when the shell should leave. */
static int on_char(uint8_t ch) {
    if (ch == '\n') {
        sh_print((const uint8_t *)"\n");
        int halt = dispatch(line, llen);
        llen = 0;
        if (!halt)
            show_prompt();
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
        sh_printf((const uint8_t *)"r2sh: started with args:");
        for (int i = 1; i < argc; i++)
            sh_printf((const uint8_t *)" %s", (uint8_t *)argv[i]);
        sh_print((const uint8_t *)"\n");
    }

    /* Sync cwd from kernel SYSTEM_CONFIG. */
    {
        SysInfo_T si;
        if (read_sysinfo(&si) && si.system_path[0]) {
            str_copy(cwd, si.system_path, 64);
            /* strip trailing CR/LF/space the kernel may leave in system_path */
            uint8_t i = 0;
            while (cwd[i]) i++;
            while (i > 0 && (cwd[i-1] == '\n' || cwd[i-1] == '\r' || cwd[i-1] == ' '))
                cwd[--i] = '\0';
        }
    }

    sh_print((const uint8_t *)"r2sh - rou2ex userland shell\nType 'help' for commands.\n");
    show_prompt();

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
