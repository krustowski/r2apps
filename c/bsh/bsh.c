#include "mem.h"
#include "string.h"
#include "syscall.h"

#include "bsh.h"
#include "bsh_int.h"

/*
 *  bsh's core: output, paths, the mount table, sessions, the command table
 *  and the dispatch by it.  The commands themselves are in bsh_fs.c (files
 *  and directories), bsh_sys.c (tasks, memory, the system) and bsh_script.c
 *  (scripts).
 */

/* ------------------------------------------------------------------------ *
 *  Output
 * ------------------------------------------------------------------------ */

void bsh_out(BshSession *s, const uint8_t *text, uint32_t len) {
    if (len)
        s->write(s, text, len);
}

void bsh_ustr(BshSession *s, const uint8_t *text) { bsh_out(s, text, strlen(text)); }
void bsh_str(BshSession *s, const char *text) { bsh_ustr(s, (const uint8_t *)text); }

void bsh_u64(BshSession *s, uint64_t v, uint8_t width) {
    uint8_t buf[24];
    uint8_t n = 0;
    do {
        buf[n++] = (uint8_t)('0' + v % 10);
        v /= 10;
    } while (v);
    uint8_t out[48];
    uint8_t o = 0;
    while (width > n && o < 24) {
        out[o++] = ' ';
        width--;
    }
    while (n)
        out[o++] = buf[--n];
    bsh_out(s, out, o);
}

void bsh_hex(BshSession *s, uint64_t v, uint8_t digits) {
    static const char hex[] = "0123456789abcdef";
    uint8_t buf[18];
    uint8_t n = 0;
    do {
        buf[n++] = (uint8_t)hex[v & 0xF];
        v >>= 4;
    } while ((v || n < digits) && n < 16);
    uint8_t out[18] = {'0', 'x'};
    uint8_t o = 2;
    while (n)
        out[o++] = buf[--n];
    bsh_out(s, out, o);
}

void bsh_color(BshSession *s, const char *sgr) {
    if (!s->color)
        return;
    bsh_str(s, "\x1b[");
    bsh_str(s, sgr);
    bsh_str(s, "m");
}

void bsh_label(BshSession *s, const char *sgr, const char *text) {
    bsh_color(s, sgr);
    bsh_str(s, text);
    bsh_color(s, BSH_RESET);
}

void bsh_ip(BshSession *s, const uint8_t ip[4]) {
    uint8_t buf[16];
    uint8_t n = 0;
    uint8_t tmp[12];
    for (uint8_t i = 0; i < 4; i++) {
        u32_to_str(ip[i], tmp);
        for (uint8_t j = 0; tmp[j]; j++)
            buf[n++] = tmp[j];
        if (i < 3)
            buf[n++] = '.';
    }
    bsh_out(s, buf, n);
}

void bsh_mac(BshSession *s, const uint8_t mac[6]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t buf[17];
    for (uint8_t i = 0; i < 6; i++) {
        buf[i * 3] = (uint8_t)hex[mac[i] >> 4];
        buf[i * 3 + 1] = (uint8_t)hex[mac[i] & 0xf];
        if (i < 5)
            buf[i * 3 + 2] = ':';
    }
    bsh_out(s, buf, 17);
}

/* ------------------------------------------------------------------------ *
 *  Strings
 * ------------------------------------------------------------------------ */

uint8_t bsh_copy(uint8_t *dst, const uint8_t *src, uint8_t cap) {
    uint8_t i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

int bsh_eq(const uint8_t *a, const char *b) {
    while (*a && *b && *a == (uint8_t)*b) {
        a++;
        b++;
    }
    return *a == (uint8_t)*b;
}

/* ------------------------------------------------------------------------ *
 *  The mount table
 *
 *  For what the kernel does not list itself: the root, and the directories
 *  on the way to the mount points ("/mnt").  The root is rootfs, which holds
 *  nothing but those; the kernel took "/" for the floppy's root, and with no
 *  floppy could not list it at all.
 * ------------------------------------------------------------------------ */

MountInfo_T bsh_mnt[BSH_MAX_MOUNTS];
int bsh_mnt_count;

void bsh_load_mounts(void) {
    int64_t n = list_mounts(bsh_mnt);
    bsh_mnt_count = n < 0 ? 0 : n > BSH_MAX_MOUNTS ? BSH_MAX_MOUNTS : (int)n;
}

const char *bsh_fs_name(uint8_t type) {
    switch (type) {
    case 1:
        return "rootfs";
    case 2:
        return "fat12";
    case 3:
        return "iso9660";
    case 4:
        return "tar";
    case 5:
        return "memdisk"; /* /mnt/tmp: the RAM disk, FAT16 (FAT12 when only 2 MiB) */
    default:
        return "unknown";
    }
}

const char *bsh_format_name(uint8_t format) {
    switch (format) {
    case FS_FORMAT_FAT12:
        return "fat12";
    case FS_FORMAT_FAT16:
        return "fat16";
    case FS_FORMAT_ISO9660:
        return "iso9660";
    case FS_FORMAT_TAR:
        return "tar";
    default:
        return "none";
    }
}

/*  Does mount <m> hold <path>: is its path all of <path>, or the start of it
 *  up to a '/'?  "/" holds everything.  */
static int mount_holds(const MountInfo_T *m, const uint8_t *path) {
    uint8_t ml = m->path_len;
    if (ml > 32)
        return 0;
    if (ml == 1 && m->path[0] == '/')
        return 1;
    for (uint8_t k = 0; k < ml; k++)
        if (path[k] != m->path[k])
            return 0;
    return path[ml] == '\0' || path[ml] == '/';
}

uint8_t bsh_mount_type_at(const uint8_t *path) {
    uint8_t best_len = 0, best_type = 0;
    for (int i = 0; i < bsh_mnt_count; i++) {
        if (bsh_mnt[i].path_len >= best_len && mount_holds(&bsh_mnt[i], path)) {
            best_len = bsh_mnt[i].path_len;
            best_type = bsh_mnt[i].fs_type;
        }
    }
    return best_type;
}

uint8_t bsh_mount_child(const MountInfo_T *m, const uint8_t *path, const uint8_t **name) {
    uint8_t pl = (uint8_t)strlen(path);
    uint8_t ml = m->path_len;
    if (ml > 32)
        return 0;
    if (pl == 1)
        pl = 0; /* "/": the children start right after the slash */
    if (ml <= pl + 1)
        return 0;
    for (uint8_t k = 0; k < pl; k++)
        if (m->path[k] != path[k])
            return 0;
    if (m->path[pl] != '/')
        return 0;
    const uint8_t *c = m->path + pl + 1;
    uint8_t n = 0;
    while (pl + 1 + n < ml && c[n] != '/')
        n++;
    *name = c;
    return n;
}

int bsh_mount_dir(const uint8_t *path) {
    uint8_t t = bsh_mount_type_at(path);
    if (t != 1 && t != 0)
        return 0; /* on a filesystem: the kernel lists it */
    if (path[0] == '/' && path[1] == '\0')
        return 1;
    const uint8_t *name;
    for (int i = 0; i < bsh_mnt_count; i++)
        if (bsh_mount_child(&bsh_mnt[i], path, &name))
            return 1;
    return 0;
}

int bsh_dir_exists(const uint8_t *path) {
    bsh_load_mounts();
    if (bsh_mount_dir(path))
        return 1;
    uint8_t t = bsh_mount_type_at(path);
    if (t == 0 || t == 1)
        return 0;
    static VfsDirEntry_T probe[64]; /* 0x2d writes up to 64, asked or not */
    int64_t r = list_dir_path(path, probe);
    return r >= 0 && r <= 64;
}

/* ------------------------------------------------------------------------ *
 *  Paths
 * ------------------------------------------------------------------------ */

void bsh_abs_path(BshSession *s, uint8_t out[BSH_PATH], const uint8_t *arg) {
    uint8_t raw[2 * BSH_PATH + 2];
    uint32_t i = 0;
    if (!arg || arg[0] != '/') {
        i = bsh_copy(raw, s->cwd, BSH_PATH);
        raw[i++] = '/';
    }
    while (arg && *arg && i < sizeof(raw) - 1)
        raw[i++] = *arg++;
    raw[i] = '\0';

    uint8_t n = 0;
    out[n++] = '/';
    for (uint32_t at = 0; raw[at];) {
        while (raw[at] == '/')
            at++;
        uint32_t len = 0;
        while (raw[at + len] && raw[at + len] != '/')
            len++;
        if (!len)
            break;
        if (len == 1 && raw[at] == '.') {
            /* this directory */
        } else if (len == 2 && raw[at] == '.' && raw[at + 1] == '.') {
            while (n > 1 && out[n - 1] != '/')
                n--;
            if (n > 1)
                n--; /* and the slash before it */
        } else {
            if (n > 1 && n < BSH_PATH - 1)
                out[n++] = '/';
            for (uint32_t k = 0; k < len && n < BSH_PATH - 1; k++)
                out[n++] = raw[at + k];
        }
        at += len;
    }
    out[n] = '\0';
}

/*  The kernel has one working directory for the process, and a host may
 *  serve several sessions: it is set to the session's before each command.
 *  It may not take "/" or "/mnt"; the session's own is cwd all the same.  */
void bsh_kernel_cwd(const uint8_t *path) { chdir(path); }
void bsh_kernel_cwd_for(BshSession *s) { chdir(s->cwd); }

/* ------------------------------------------------------------------------ *
 *  Sessions
 * ------------------------------------------------------------------------ */

void bsh_init(BshSession *s, void (*write)(BshSession *, const uint8_t *, uint32_t), const BshCommand *commands,
              void *host) {
    s->write = write;
    s->clear = 0;
    s->flush = 0;
    s->color = 0;
    s->color_ok = 0;
    s->commands = commands;
    s->host = host;
    if (bsh_dir_exists((const uint8_t *)"/mnt/fat"))
        bsh_copy(s->cwd, (const uint8_t *)"/mnt/fat", BSH_PATH);
    else
        bsh_copy(s->cwd, (const uint8_t *)"/", BSH_PATH);
    bsh_copy(s->start, s->cwd, BSH_PATH);
}

int bsh_cd(BshSession *s, const uint8_t *path) {
    uint8_t abs[BSH_PATH];
    bsh_abs_path(s, abs, path);
    if (!bsh_dir_exists(abs))
        return 0;
    bsh_copy(s->cwd, abs, BSH_PATH);
    bsh_kernel_cwd_for(s);
    return 1;
}

void bsh_prompt(BshSession *s) {
    SysInfo_T si = {0};
    if (read_sysinfo(&si)) {
        si.system_user[31] = '\0';
        si.system_name[31] = '\0';
        /*  The kernel pads the host name with spaces.  */
        for (int i = 30; i >= 0 && (si.system_name[i] == ' ' || !si.system_name[i]); i--)
            si.system_name[i] = '\0';
        bsh_color(s, BSH_GREEN);
        bsh_ustr(s, si.system_user);
        bsh_str(s, "@");
        bsh_ustr(s, si.system_name);
        bsh_color(s, BSH_RESET);
        bsh_str(s, ":");
    }
    bsh_color(s, BSH_BLUE);
    bsh_ustr(s, s->cwd);
    bsh_color(s, BSH_RESET);
    bsh_str(s, "> ");
}

/* ------------------------------------------------------------------------ *
 *  Commands
 * ------------------------------------------------------------------------ */

static int cmd_help(BshSession *s, const uint8_t *arg);

static int cmd_exit(BshSession *s, const uint8_t *arg) {
    (void)s;
    (void)arg;
    return BSH_EXIT;
}

/*  `color [on|off]`: where the host can show colour.  */
static int cmd_color(BshSession *s, const uint8_t *arg) {
    if (!s->color_ok) {
        bsh_str(s, "color: this terminal shows no colours\n");
        return 0;
    }
    if (bsh_eq(arg, "on"))
        s->color = 1;
    else if (bsh_eq(arg, "off"))
        s->color = 0;
    else if (arg[0]) {
        bsh_label(s, BSH_C_USAGE, "color: usage: color [on|off]\n");
        return 0;
    }
    bsh_str(s, "color: ");
    bsh_color(s, BSH_GREEN);
    bsh_str(s, s->color ? "on" : "off");
    bsh_color(s, BSH_RESET);
    bsh_str(s, "\n");
    return 0;
}

static int cmd_clear(BshSession *s, const uint8_t *arg) {
    (void)arg;
    if (s->clear)
        s->clear(s);
    else
        bsh_label(s, BSH_C_ERROR, "clear: this terminal cannot be cleared\n");
    return 0;
}

static const BshCommand base[] = {
    {"help", "", "show this message", cmd_help},
    {"ls", "[path]", "list a directory (/ shows the mounts)", bsh_cmd_ls},
    {"cd", "[path]", "change directory (alone: back to the start)", bsh_cmd_cd},
    {"mkdir", "<path>", "make a directory (8 chars; /mnt/fat or /mnt/tmp)", bsh_cmd_mkdir},
    {"rmdir", "<path>", "remove an empty directory", bsh_cmd_rmdir},
    {"rm", "<path>", "delete a file", bsh_cmd_rm},
    {"read", "<path>", "print a file", bsh_cmd_read},
    {"mount", "", "list mounted filesystems", bsh_cmd_mount},
    {"sysinfo", "", "show system information", bsh_cmd_sysinfo},
    {"bg", "<name> [args]", "run a program in the background", bsh_cmd_bg},
    {"run", "", 0, bsh_cmd_bg},
    {"ts", "", "list running tasks", bsh_cmd_ts},
    {"kill", "<pid>", "end a task by its PID", bsh_cmd_kill},
    {"meminfo", "", "RAM, process frames and the user heap", bsh_cmd_meminfo},
    {"mem", "", 0, bsh_cmd_meminfo},
    {"heap", "", "the user heap in detail, and who holds it", bsh_cmd_heap},
    {"play", "<name>", "play a MIDI file", bsh_cmd_play},
    {"stop", "", "stop playback", bsh_cmd_stop},
    {"bsh", "<file> [args]", "run a script (.BSH): a command a line", bsh_cmd_bsh},
    {"echo", "[text]", "print a line of text", bsh_cmd_echo},
    {"sleep", "<ms>", "wait so many milliseconds", bsh_cmd_sleep},
    {"clear", "", "clear the screen", cmd_clear},
    {"color", "[on|off]", "colour the prompt and listings", cmd_color},
    {"exit", "", "end the session", cmd_exit},
    {"quit", "", 0, cmd_exit},
    {0, 0, 0, 0},
};

static const BshCommand *find_in(const BshCommand *table, const uint8_t *name) {
    for (; table && table->name; table++)
        if (bsh_eq(name, table->name))
            return table;
    return 0;
}

static const BshCommand *find(BshSession *s, const uint8_t *name) {
    const BshCommand *c = find_in(s->commands, name);
    return c ? c : find_in(base, name);
}

static void help_line(BshSession *s, const BshCommand *c) {
    uint32_t col = 2 + strlen((const uint8_t *)c->name);
    bsh_str(s, "  ");
    bsh_label(s, BSH_GREEN, c->name);
    if (c->usage[0]) {
        bsh_str(s, " ");
        bsh_label(s, BSH_CYAN, c->usage);
        col += 1 + strlen((const uint8_t *)c->usage);
    }
    do
        bsh_str(s, " ");
    while (++col < 24);
    bsh_str(s, c->help);
    bsh_str(s, "\n");
}

/*  The base commands, each as the host has it if it has its own, then the
 *  host's that are not base ones.  */
static int cmd_help(BshSession *s, const uint8_t *arg) {
    (void)arg;
    bsh_label(s, BSH_C_HEAD, "Commands:");
    bsh_str(s, "\n");
    for (const BshCommand *b = base; b->name; b++) {
        const BshCommand *c = find(s, (const uint8_t *)b->name);
        if (c->help)
            help_line(s, c);
    }
    for (const BshCommand *h = s->commands; h && h->name; h++)
        if (h->help && !find_in(base, (const uint8_t *)h->name))
            help_line(s, h);
    return 0;
}

int bsh_dispatch(BshSession *s, uint8_t *line, uint32_t len) {
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\t'))
        len--;
    line[len] = '\0';
    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line)
        return 0;

    uint8_t name[16];
    uint8_t n = 0;
    while (line[n] && line[n] != ' ' && n < sizeof(name) - 1) {
        name[n] = line[n];
        n++;
    }
    name[n] = '\0';
    const uint8_t *arg = line + n;
    if (*arg && *arg != ' ')
        arg = 0; /* a name too long for any command */
    else
        while (*arg == ' ')
            arg++;

    const BshCommand *c = arg ? find(s, name) : 0;
    if (!c) {
        bsh_color(s, BSH_RED);
        bsh_str(s, "Unknown command: ");
        bsh_out(s, line, n);
        bsh_color(s, BSH_RESET);
        bsh_str(s, " ('help' lists them)\n");
        return 0;
    }

    bsh_kernel_cwd_for(s); /* this session's, for `play` and `bg` */
    return c->run(s, arg);
}
