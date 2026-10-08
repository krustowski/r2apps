#include "mem.h"
#include "net.h"
#include "string.h"
#include "syscall.h"

#include "shell.h"
#include "xfer.h"

/*
 *  Output is gathered here and goes out in segments of up to OUT_CAP bytes
 *  when the command is done (shell_flush), instead of one TCP segment per
 *  string: `mem` alone was fifty.  The TCP layer does not retransmit, so a
 *  segment the NIC could not take in a burst was lost for good and the
 *  session hung on the gap; fewer, fuller segments make that much rarer.
 */
#define OUT_CAP 1024
static uint8_t out_buf[OUT_CAP];
static uint32_t out_len = 0;
static TcpSocket_T *out_sock = 0;

void shell_flush(void) {
    if (out_sock && out_len)
        write(out_sock, out_buf, out_len);
    out_len = 0;
}

static void out(TcpSocket_T *sock, const uint8_t *s, uint32_t n) {
    if (sock != out_sock) {
        shell_flush();
        out_sock = sock;
    }
    while (n) {
        if (out_len == OUT_CAP)
            shell_flush();
        uint32_t room = OUT_CAP - out_len;
        uint32_t chunk = n < room ? n : room;
        memcpy(out_buf + out_len, s, (uint16_t)chunk);
        out_len += chunk;
        s += chunk;
        n -= chunk;
    }
}

static void sock_str(TcpSocket_T *sock, const uint8_t *s) { out(sock, s, strlen(s)); }

static int peer_telnet = 0;
void shell_set_telnet(int on) { peer_telnet = on; }



static void sock_u32(TcpSocket_T *sock, uint32_t v) {
    uint8_t buf[12];
    u32_to_str(v, buf);
    sock_str(sock, buf);
}

/* Write a dotted-decimal IPv4 address in one TCP segment. */
static void sock_ip(TcpSocket_T *sock, const uint8_t ip[4]) {
    uint8_t buf[16]; /* "255.255.255.255" max */
    uint8_t n = 0;
    uint8_t tmp[12];
    for (uint8_t i = 0; i < 4; i++) {
        u32_to_str(ip[i], tmp);
        for (uint8_t j = 0; tmp[j]; j++)
            buf[n++] = tmp[j];
        if (i < 3)
            buf[n++] = '.';
    }
    out(sock, buf, n);
}

/* Write a colon-separated MAC address (xx:xx:xx:xx:xx:xx) in one TCP segment. */
static void sock_mac(TcpSocket_T *sock, const uint8_t mac[6]) {
    static const uint8_t hex[] = "0123456789abcdef";
    uint8_t buf[17]; /* "xx:xx:xx:xx:xx:xx" */
    for (uint8_t i = 0; i < 6; i++) {
        buf[i * 3] = hex[mac[i] >> 4];
        buf[i * 3 + 1] = hex[mac[i] & 0xf];
        if (i < 5)
            buf[i * 3 + 2] = ':';
    }
    out(sock, buf, 17);
}

/* Per-session current working directory (absolute VFS path). */
static uint8_t cwd[64] = "/mnt/fat";

/* Copy src into dst, NUL-terminate, return length written (max dst_cap-1). */
static uint8_t str_copy(uint8_t *dst, const uint8_t *src, uint8_t dst_cap) {
    uint8_t i = 0;
    while (src[i] && i < dst_cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

void shell_banner(TcpSocket_T *sock) {
    sock_str(sock, (const uint8_t *)"\r\ntnt - rou2exOS telnet service\r\n"
                                    "Type 'help' for commands.\r\n\r\n");
}

/*
 *  "user@host:/mnt/fat> ", as the kernel's own prompt has it.  The user is the
 *  system user, which Memento's login sets (and the login a new connection
 *  gives, see auth.h, is that one); "root" before anyone has logged in.
 */
void shell_prompt(TcpSocket_T *sock) {
    SysInfo_T si = {0};
    if (read_sysinfo(&si)) {
        si.system_user[31] = '\0';
        si.system_name[31] = '\0';
        /*  The kernel pads the host name with spaces.  */
        for (int i = 30; i >= 0 && (si.system_name[i] == ' ' || !si.system_name[i]); i--)
            si.system_name[i] = '\0';
        sock_str(sock, si.system_user);
        sock_str(sock, (const uint8_t *)"@");
        sock_str(sock, si.system_name);
        sock_str(sock, (const uint8_t *)":");
    }
    sock_str(sock, cwd);
    sock_str(sock, (const uint8_t *)"> ");
}

static void cmd_help(TcpSocket_T *sock) {
    sock_str(sock, (const uint8_t *)"Commands:\r\n"
                                    "  help          show this message\r\n"
                                    "  ls [path]     list directory (default: cwd)\r\n"
                                    "  cd <path>     change directory\r\n"
                                    "  read <path>   print file contents (a key stops it)\r\n"
                                    "  get <path> [port]  send a file to curl/wget/nc (port 8023)\r\n"
                                    "  bg <name> [args...]  run ELF in background\r\n"
                                    "  ts            list running tasks\r\n"
                                    "  kill <pid>    end a task by its PID\r\n"
                                    "  meminfo       RAM, process frames and the user heap\r\n"
                                    "  heap          the user heap in detail, and who holds it\r\n"
                                    "  play <name>   play MIDI file\r\n"
                                    "  stop          stop playback\r\n"
                                    "  sysinfo       show system information\r\n"
                                    "  mount         list mounted filesystems\r\n"
                                    "  net           show interface and active connections\r\n"
                                    "  exit          close connection\r\n");
}

static void cmd_sysinfo(TcpSocket_T *sock) {
    SysInfo_T si = {0};

    if (!read_sysinfo(&si)) {
        sock_str(sock, (const uint8_t *)"sysinfo: failed\r\n");
        return;
    }

    si.system_name[31] = '\0';
    si.system_user[31] = '\0';
    si.system_path[31] = '\0';
    si.system_version[7] = '\0';

    sock_str(sock, (const uint8_t *)"System:  ");
    sock_str(sock, si.system_name);
    sock_str(sock, (const uint8_t *)"\r\n");
    sock_str(sock, (const uint8_t *)"User:    ");
    sock_str(sock, si.system_user);
    sock_str(sock, (const uint8_t *)"\r\n");
    sock_str(sock, (const uint8_t *)"Path:    ");
    sock_str(sock, si.system_path);
    sock_str(sock, (const uint8_t *)" (cluster: ");
    sock_u32(sock, si.system_path_cluster);
    sock_str(sock, (const uint8_t *)")\r\n");
    sock_str(sock, (const uint8_t *)"Version: ");
    sock_str(sock, si.system_version);
    sock_str(sock, (const uint8_t *)"\r\n");
    sock_str(sock, (const uint8_t *)"Uptime:  ");
    sock_u32(sock, si.system_uptime / 3600);
    sock_str(sock, (const uint8_t *)"h ");
    sock_u32(sock, (si.system_uptime % 3600) / 60);
    sock_str(sock, (const uint8_t *)"m ");
    sock_u32(sock, si.system_uptime % 60);
    sock_str(sock, (const uint8_t *)"s\r\n");
}

/*
 *  Decode and write a FAT16 date+time pair.
 *  date bits: [15:9] year-1980  [8:5] month  [4:0] day
 *  time bits: [15:11] hours  [10:5] minutes  [4:0] seconds/2
 *  Sends one TCP segment: "  YYYY/MM/DD HH:MM"
 */
static void sock_fat_datetime(TcpSocket_T *sock, uint16_t date, uint16_t time) {
    uint8_t buf[20];
    uint8_t tmp[12];
    uint8_t n = 0;

    if (!date) {
        sock_str(sock, (const uint8_t *)"  ----/--/-- --:--");
        return;
    }

    uint16_t year = ((date >> 9) & 0x7f) + 1980;
    uint8_t month = (date >> 5) & 0x0f;
    uint8_t day = date & 0x1f;
    uint8_t hours = (time >> 11) & 0x1f;
    uint8_t minutes = (time >> 5) & 0x3f;

    buf[n++] = ' ';
    buf[n++] = ' ';

    u32_to_str(year, tmp);
    for (uint8_t j = 0; tmp[j]; j++)
        buf[n++] = tmp[j];
    buf[n++] = '/';

    if (month < 10)
        buf[n++] = '0';
    u32_to_str(month, tmp);
    for (uint8_t j = 0; tmp[j]; j++)
        buf[n++] = tmp[j];
    buf[n++] = '/';

    if (day < 10)
        buf[n++] = '0';
    u32_to_str(day, tmp);
    for (uint8_t j = 0; tmp[j]; j++)
        buf[n++] = tmp[j];
    buf[n++] = ' ';

    if (hours < 10)
        buf[n++] = '0';
    u32_to_str(hours, tmp);
    for (uint8_t j = 0; tmp[j]; j++)
        buf[n++] = tmp[j];
    buf[n++] = ':';

    if (minutes < 10)
        buf[n++] = '0';
    u32_to_str(minutes, tmp);
    for (uint8_t j = 0; tmp[j]; j++)
        buf[n++] = tmp[j];

    out(sock, buf, n);
}

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
    /* relative: cwd + "/" + arg (don't add '/' if cwd already ends with one) */
    uint8_t i = str_copy(out, cwd, 64);
    if (i > 0 && out[i - 1] != '/' && i < 63) {
        out[i++] = '/';
        out[i] = '\0';
    }
    while (*arg && i < 63) {
        out[i++] = *arg++;
    }
    out[i] = '\0';
}

static void cmd_ls(TcpSocket_T *sock, const uint8_t *path_arg) {
    uint8_t abs[64];
    build_abs_path(abs, path_arg);

    /* 64 entries: syscall 0x2D takes no capacity argument and writes one per
     * directory member, up to 64.  A 32-entry buffer is overrun silently. */
    VfsDirEntry_T entries[64] = {0};
    int64_t count = list_dir_path(abs, entries);

    /* Kernel returns u64::MAX (-1 as int64_t) on error; valid range is 0–64. */
    if (count < 0 || count > 64) {
        sock_str(sock, (const uint8_t *)"ls: no such directory: ");
        sock_str(sock, abs);
        sock_str(sock, (const uint8_t *)"\r\n");
        return;
    }

    for (int64_t i = 0; i < count; i++) {
        VfsDirEntry_T *e = &entries[i];
        sock_str(sock, (const uint8_t *)"  ");
        out(sock, e->name, e->name_len);
        if (e->is_dir) {
            sock_str(sock, (const uint8_t *)"/  <DIR>\r\n");
        } else {
            sock_str(sock, (const uint8_t *)"  ");
            sock_u32(sock, e->size);
            sock_str(sock, (const uint8_t *)" bytes\r\n");
        }
    }

    if (count == 0)
        sock_str(sock, (const uint8_t *)"  (empty)\r\n");
}

static void cmd_cd(TcpSocket_T *sock, const uint8_t *path_arg) {
    if (!path_arg || !path_arg[0]) {
        sock_str(sock, (const uint8_t *)"cd: usage: cd <path>\r\n");
        return;
    }

    /* Handle "cd .." — trim last path component then sync kernel. */
    if (path_arg[0] == '.' && path_arg[1] == '.' && path_arg[2] == '\0') {
        uint8_t i = 0;
        while (cwd[i])
            i++;
        if (i == 0)
            return;
        /* Find the last '/' */
        uint8_t last_slash = 0;
        for (uint8_t j = 0; j < i; j++)
            if (cwd[j] == '/')
                last_slash = j;
        if (last_slash == 0) {
            /* Already at root or one level below root — go to "/" */
            cwd[0] = '/';
            cwd[1] = '\0';
        } else {
            cwd[last_slash] = '\0';
        }
        chdir(cwd);
        return;
    }

    uint8_t abs[64];
    build_abs_path(abs, path_arg);

    /* Verify the directory exists.  Kernel returns u64::MAX on any error;
       valid counts are 0–64.  Reject anything outside that range. */
    VfsDirEntry_T tmp[64];
    int64_t r = list_dir_path(abs, tmp);
    if (r < 0 || r > 64) {
        sock_str(sock, (const uint8_t *)"cd: no such directory: ");
        sock_str(sock, abs);
        sock_str(sock, (const uint8_t *)"\r\n");
        return;
    }

    str_copy(cwd, abs, 64);
    chdir(abs);
}

/*
 *  `read`: the whole file, read into a block of the user heap first (syscall
 *  0x0a; it grows past its first 4 MiB when it has to), then sent down the
 *  session the way `get` sends a download --- following the client's ACKs and
 *  sending again what went missing --- since libcr2's TCP does not, and a
 *  file of any size is far more than one burst of segments.  A key pressed
 *  meanwhile stops it.  It used to be read with read_file() into 4 KiB on the
 *  stack's doorstep, and a bigger file wrote on past it.
 */
#define READ_MAX (8u * 1024 * 1024) /* past this, `get` is the way */
#define READ_PIECE (32u * 1024)     /* read_file_at() at a time */

static void sock_size(TcpSocket_T *sock, uint32_t bytes) {
    sock_u32(sock, bytes);
    sock_str(sock, (const uint8_t *)" bytes");
}

static void cmd_read(TcpSocket_T *sock, TcpSocket_T sockets[MAX_SOCKETS], const uint8_t *path_arg) {
    if (!path_arg || !path_arg[0]) {
        sock_str(sock, (const uint8_t *)"read: usage: read <file>\r\n");
        return;
    }

    uint8_t abs[64];
    build_abs_path(abs, path_arg);

    int64_t size = xfer_file_size(abs);
    if (size == -1) {
        sock_str(sock, (const uint8_t *)"read: failed to open '");
        sock_str(sock, abs);
        sock_str(sock, (const uint8_t *)"'\r\n");
        return;
    }
    if (size > (int64_t)READ_MAX) {
        sock_str(sock, (const uint8_t *)"read: ");
        sock_size(sock, (uint32_t)size);
        sock_str(sock, (const uint8_t *)" is too much to show here; use 'get'\r\n");
        return;
    }

    /*  Into the heap.  Its size from the directory when it says, else grown
     *  as the file turns out to go on (XFER_SIZE_UNKNOWN).  */
    uint32_t cap = size >= 0 ? (uint32_t)size : 64u * 1024;
    uint8_t *buf = malloc(cap ? cap : 1);
    uint32_t len = 0;
    int err = 0;
    while (buf) {
        if (len == cap) {
            if (size >= 0 || cap >= READ_MAX)
                break;
            uint32_t more = cap * 2 > READ_MAX ? READ_MAX : cap * 2;
            uint8_t *grown = realloc(buf, more);
            if (!grown) {
                free(buf);
                buf = 0;
                break;
            }
            buf = grown;
            cap = more;
        }
        uint32_t want = cap - len < READ_PIECE ? cap - len : READ_PIECE;
        int64_t got = read_file_at(abs, buf + len, len, want);
        if (got < 0) {
            err = 1;
            break;
        }
        len += (uint32_t)got;
        if ((uint32_t)got < want)
            break; /* the end of it */
    }
    if (!buf) {
        sock_str(sock, (const uint8_t *)"read: no memory for ");
        sock_size(sock, size >= 0 ? (uint32_t)size : cap);
        sock_str(sock, (const uint8_t *)"\r\n");
        return;
    }
    if (err) {
        free(buf);
        sock_str(sock, (const uint8_t *)"read: failed reading '");
        sock_str(sock, abs);
        sock_str(sock, (const uint8_t *)"'\r\n");
        return;
    }
    if (len == 0) {
        free(buf);
        sock_str(sock, (const uint8_t *)"(empty file)\r\n");
        return;
    }

    /*  For the terminal: a bare \n becomes \r\n, and for a telnet client a
     *  0xff byte is doubled, or it takes it for a TELNET command.  Done in
     *  place from the end, in the block grown by what that adds.  */
    uint8_t iac = peer_telnet ? 0xFF : '\n'; /* '\n': nothing more to double */
    uint32_t extra = 0;
    for (uint32_t j = 0; j < len; j++)
        if (buf[j] == '\n' || buf[j] == iac)
            extra++;
    if (extra) {
        uint8_t *grown = realloc(buf, len + extra);
        if (!grown) {
            free(buf);
            sock_str(sock, (const uint8_t *)"read: no memory for ");
            sock_size(sock, len + extra);
            sock_str(sock, (const uint8_t *)"\r\n");
            return;
        }
        buf = grown;
        uint32_t w = len + extra;
        for (uint32_t j = len; j-- > 0;) {
            uint8_t b = buf[j];
            buf[--w] = b;
            if (b == '\n')
                buf[--w] = '\r';
            else if (b == iac)
                buf[--w] = 0xFF;
        }
        len += extra;
    }

    shell_flush(); /* what came before goes first */
    XferResult_T res;
    XferStatus_T st = xfer_send_buffer(sockets, sock, buf, len, &res);
    free(buf);

    if (st == XFER_CANCELLED) {
        sock_str(sock, (const uint8_t *)"\r\n[stopped after ");
        sock_size(sock, (uint32_t)res.bytes);
        sock_str(sock, (const uint8_t *)"]\r\n");
    } else if (st == XFER_OK) {
        sock_str(sock, (const uint8_t *)"\r\n");
    }
    /* Stalled or reset: the session is gone, and there is nobody to tell. */
}

static void cmd_mount(TcpSocket_T *sock) {
    MountInfo_T mounts[8];
    int64_t count = list_mounts(mounts);
    if (count <= 0) {
        sock_str(sock, (const uint8_t *)"No mounts.\r\n");
        return;
    }
    for (int64_t i = 0; i < count; i++) {
        out(sock, mounts[i].path, mounts[i].path_len);
        const uint8_t *fsname;
        switch (mounts[i].fs_type) {
        case 1:
            fsname = (const uint8_t *)"rootfs";
            break;
        case 2:
            fsname = (const uint8_t *)"fat12";
            break;
        case 3:
            fsname = (const uint8_t *)"iso9660";
            break;
        case 4:
            fsname = (const uint8_t *)"tar";
            break;
        default:
            fsname = (const uint8_t *)"unknown";
            break;
        }
        sock_str(sock, (const uint8_t *)" (");
        sock_str(sock, fsname);
        sock_str(sock, (const uint8_t *)")\r\n");
    }
}

static void cmd_play(TcpSocket_T *sock, const uint8_t *name) {
    /* Blocks until the song is over; 0 when the file could not be read or
     * parsed.  A relative name is found in the cwd, on the floppy or on the
     * tar archive alike. */
    int64_t r = play_midi_file(name);
    if (!r) {
        sock_str(sock, (const uint8_t *)"play: failed to open '");
        sock_str(sock, name);
        sock_str(sock, (const uint8_t *)"'\r\n");
    } else {
        sock_str(sock, (const uint8_t *)"play: finished '");
        sock_str(sock, name);
        sock_str(sock, (const uint8_t *)"'\r\n");
    }
}

static void cmd_stop(TcpSocket_T *sock) {
    stop_speaker();
    sock_str(sock, (const uint8_t *)"play: stopped\r\n");
}

/* A 64-bit value as hex, for the saved instruction pointer. */
static void sock_hex64(TcpSocket_T *sock, uint64_t v) {
    const uint8_t *digits = (const uint8_t *)"0123456789abcdef";
    uint8_t buf[16];
    uint8_t i = 16;

    while (i--) {
        buf[i] = digits[v & 0xF];
        v >>= 4;
    }

    out(sock, buf, 16);
}

static const uint8_t *const task_status[] = {
    (const uint8_t *)"Ready   ", (const uint8_t *)"Running ", (const uint8_t *)"Idle    ", (const uint8_t *)"Blocked ", (const uint8_t *)"Crashed ", (const uint8_t *)"Dead    ",
};

static void cmd_ts(TcpSocket_T *sock) {
    const int MAX_TASKS = 10;
    TaskInfo_T tasks[MAX_TASKS];
    int64_t count = list_tasks(tasks, MAX_TASKS);

    if (count <= 0) {
        sock_str(sock, (const uint8_t *)"No tasks.\r\n");
        return;
    }

    sock_str(sock, (const uint8_t *)"PID  M  STATUS    NAME              RIP\r\n");
    for (int64_t i = 0; i < count; i++) {
        TaskInfo_T *t = &tasks[i];
        sock_u32(sock, t->id);
        sock_str(sock, (const uint8_t *)"    ");
        sock_str(sock, t->mode == 0 ? (const uint8_t *)"K  " : (const uint8_t *)"U  ");
        uint8_t s = t->status < 6 ? t->status : 0;
        sock_str(sock, task_status[s]);
        sock_str(sock, (const uint8_t *)"  ");
        uint8_t nlen = 0;
        while (nlen < 16 && t->name[nlen] && t->name[nlen] != ' ')
            nlen++;
        out(sock, t->name, nlen);

        /* Pad the name out so the addresses line up, then where the task is. */
        for (uint8_t p = nlen; p < 18; p++)
            sock_str(sock, (const uint8_t *)" ");

        sock_str(sock, (const uint8_t *)"0x");
        sock_hex64(sock, t->rip);
        sock_str(sock, (const uint8_t *)"\r\n");
    }
}

/*
 *  End a process by the PID `ts` printed.
 *
 *  The console is not always a way out: a program that stops answering its own
 *  keys cannot be quit from the machine it is running on, and until now there
 *  was nothing else to ask.  This is that something else.
 */
static void cmd_kill(TcpSocket_T *sock, const uint8_t *arg) {
    uint64_t pid = 0;
    uint8_t digits = 0;

    while (arg && arg[digits] >= '0' && arg[digits] <= '9') {
        pid = pid * 10 + (uint64_t)(arg[digits] - '0');
        digits++;
    }

    if (!digits) {
        sock_str(sock, (const uint8_t *)"kill: usage: kill <pid>\r\n");
        return;
    }

    if (kill_task(pid)) {
        sock_str(sock, (const uint8_t *)"kill: killed PID ");
        sock_u32(sock, (uint32_t)pid);
        sock_str(sock, (const uint8_t *)"\r\n");
    } else {
        sock_str(sock, (const uint8_t *)"kill: no such PID\r\n");
    }
}

/* An unsigned number of any size, optionally padded on the left to <width>. */
static void sock_u64w(TcpSocket_T *sock, uint64_t v, uint8_t width) {
    uint8_t buf[24];
    uint8_t n = 0;

    do {
        buf[n++] = (uint8_t)('0' + v % 10);
        v /= 10;
    } while (v);

    while (width > n) {
        sock_str(sock, (const uint8_t *)" ");
        width--;
    }
    while (n)
        out(sock, &buf[--n], 1);
}

/* An address, short: "0xC00000". */
static void sock_hex(TcpSocket_T *sock, uint64_t v) {
    const uint8_t *digits = (const uint8_t *)"0123456789ABCDEF";
    uint8_t buf[18];
    uint8_t n = 0;

    do {
        buf[n++] = digits[v & 0xF];
        v >>= 4;
    } while (v);

    sock_str(sock, (const uint8_t *)"0x");
    while (n)
        out(sock, &buf[--n], 1);
}

/* Bytes as KiB, one decimal, right-aligned in 9 columns: "   1024.0 KiB". */
static void sock_kib(TcpSocket_T *sock, uint64_t bytes) {
    uint64_t tenths = (bytes * 10 + 512) / 1024;
    sock_u64w(sock, tenths / 10, 7);
    sock_str(sock, (const uint8_t *)".");
    sock_u64w(sock, tenths % 10, 1);
    sock_str(sock, (const uint8_t *)" KiB");
}

/* Task <id>'s entry in a list_tasks() table, or 0. */
static const TaskInfo_T *find_task(const TaskInfo_T *tasks, int64_t count, uint8_t id) {
    for (int64_t i = 0; i < count; i++)
        if (tasks[i].id == id)
            return &tasks[i];
    return 0;
}

/* The name of task <id> from a list_tasks() table, trimmed; "?" when absent.
 * Returns how many characters it wrote. */
static uint8_t sock_task_name(TcpSocket_T *sock, const TaskInfo_T *tasks, int64_t count, uint8_t id) {
    for (int64_t i = 0; i < count; i++) {
        if (tasks[i].id != id)
            continue;
        uint8_t nlen = 0;
        while (nlen < 16 && tasks[i].name[nlen] && tasks[i].name[nlen] != ' ')
            nlen++;
        out(sock, tasks[i].name, nlen);
        return nlen;
    }
    sock_str(sock, (const uint8_t *)"?");
    return 1;
}

/*
 *  The kernel's figures (syscall 0x3c), read once for both commands.  The
 *  kernel answers busy when the heap or the scheduler is locked that instant,
 *  so it is asked a few times before giving up.
 */
static int read_mem(TcpSocket_T *sock, MemInfo_T *mi) {
    for (int tries = 0; tries < 20; tries++) {
        if (read_meminfo(mi))
            return 1;
        sleep_ms(2);
    }
    sock_str(sock, (const uint8_t *)"memory information not available (kernel busy, or older than syscall 0x3c)\r\n");
    return 0;
}

/*
 *  meminfo: the machine's memory at a glance --- the RAM, each process's
 *  private 2 MiB frame and who is in it, and the user heap in one line.
 */
static void cmd_meminfo(TcpSocket_T *sock) {
    MemInfo_T mi;
    if (!read_mem(sock, &mi))
        return;

    TaskInfo_T tasks[10];
    int64_t count = list_tasks(tasks, 10);
    if (count < 0)
        count = 0;

    sock_str(sock, (const uint8_t *)"RAM            ");
    sock_u64w(sock, mi.total_ram / (1024 * 1024), 0);
    sock_str(sock, (const uint8_t *)" MiB (");
    sock_u64w(sock, mi.total_ram, 0);
    sock_str(sock, (const uint8_t *)" bytes)\r\n");

    /* Kernel tasks take a slot too, but run on the kernel's own mappings:
     * only a user program's frame holds anything. */
    uint64_t slots = mi.slots < 16 ? mi.slots : 16;
    uint64_t in_use = 0;
    for (uint64_t s = 0; s < slots; s++) {
        const TaskInfo_T *t = mi.slot_task[s] == 0xFF ? 0 : find_task(tasks, count, mi.slot_task[s]);
        if (t && t->mode != 0)
            in_use++;
    }

    sock_str(sock, (const uint8_t *)"Process frames ");
    sock_u64w(sock, slots, 0);
    sock_str(sock, (const uint8_t *)" x ");
    sock_u64w(sock, mi.frame_size / 1024, 0);
    sock_str(sock, (const uint8_t *)" KiB, each seen at ");
    sock_hex(sock, mi.frame_virt);
    sock_str(sock, (const uint8_t *)"; ");
    sock_u64w(sock, in_use, 0);
    sock_str(sock, (const uint8_t *)" held by programs\r\n");

    sock_str(sock, (const uint8_t *)"  SLOT  PHYSICAL    PID  NAME\r\n");
    for (uint64_t s = 0; s < slots; s++) {
        sock_u64w(sock, s, 6);
        sock_str(sock, (const uint8_t *)"  ");
        sock_hex(sock, mi.frame_base + s * mi.frame_size);
        sock_str(sock, (const uint8_t *)"  ");
        if (mi.slot_task[s] == 0xFF) {
            sock_str(sock, (const uint8_t *)"   -  free\r\n");
            continue;
        }
        sock_u64w(sock, mi.slot_task[s], 4);
        sock_str(sock, (const uint8_t *)"  ");
        uint8_t nlen = sock_task_name(sock, tasks, count, mi.slot_task[s]);
        const TaskInfo_T *t = find_task(tasks, count, mi.slot_task[s]);
        if (t && t->mode == 0) {
            for (uint8_t p = nlen; p < 10; p++)
                sock_str(sock, (const uint8_t *)" ");
            sock_str(sock, (const uint8_t *)"(kernel task: frame unused)");
        }
        sock_str(sock, (const uint8_t *)"\r\n");
    }

    sock_str(sock, (const uint8_t *)"User heap      ");
    sock_u64w(sock, mi.heap_size / 1024, 0);
    sock_str(sock, (const uint8_t *)" KiB at ");
    sock_hex(sock, mi.heap_start);
    sock_str(sock, (const uint8_t *)": ");
    sock_u64w(sock, (mi.heap_used + 512) / 1024, 0);
    sock_str(sock, (const uint8_t *)" KiB used, ");
    sock_u64w(sock, (mi.heap_free + 512) / 1024, 0);
    sock_str(sock, (const uint8_t *)" KiB free, largest ");
    sock_u64w(sock, (mi.heap_largest_free + 512) / 1024, 0);
    sock_str(sock, (const uint8_t *)" KiB ('heap' for more)\r\n");
}

/*
 *  heap: the user heap (malloc, syscall 0x0a) in detail --- what is used, what
 *  is free and how broken up it is, and which task holds how much.
 */
static void cmd_heap(TcpSocket_T *sock) {
    MemInfo_T mi;
    if (!read_mem(sock, &mi))
        return;

    TaskInfo_T tasks[10];
    int64_t count = list_tasks(tasks, 10);
    if (count < 0)
        count = 0;

    uint64_t headers = mi.heap_size - mi.heap_used - mi.heap_free;

    sock_str(sock, (const uint8_t *)"User heap ");
    sock_hex(sock, mi.heap_start);
    sock_str(sock, (const uint8_t *)"-");
    sock_hex(sock, mi.heap_start + mi.heap_size - 1);
    sock_str(sock, (const uint8_t *)", shared by every process\r\n");

    sock_str(sock, (const uint8_t *)"  size         ");
    sock_kib(sock, mi.heap_size);
    sock_str(sock, (const uint8_t *)"\r\n  used         ");
    sock_kib(sock, mi.heap_used);
    sock_str(sock, (const uint8_t *)"  ");
    sock_u64w(sock, mi.heap_size ? mi.heap_used * 100 / mi.heap_size : 0, 3);
    sock_str(sock, (const uint8_t *)"%\r\n  free         ");
    sock_kib(sock, mi.heap_free);
    sock_str(sock, (const uint8_t *)"  ");
    sock_u64w(sock, mi.heap_size ? mi.heap_free * 100 / mi.heap_size : 0, 3);
    sock_str(sock, (const uint8_t *)"%\r\n  largest free ");
    sock_kib(sock, mi.heap_largest_free);
    sock_str(sock, (const uint8_t *)"  (the biggest one malloc can get)\r\n  headers      ");
    sock_kib(sock, headers);
    sock_str(sock, (const uint8_t *)"\r\n  blocks       ");
    sock_u64w(sock, mi.heap_blocks, 9);
    sock_str(sock, (const uint8_t *)"      (");
    sock_u64w(sock, mi.heap_free_blocks, 0);
    sock_str(sock, (const uint8_t *)" free)\r\n");

    /* Free space in many pieces is space a big request cannot have. */
    if (mi.heap_free) {
        sock_str(sock, (const uint8_t *)"  fragmented   ");
        sock_u64w(sock, 100 - mi.heap_largest_free * 100 / mi.heap_free, 9);
        sock_str(sock, (const uint8_t *)"%     of the free space is outside the largest block\r\n");
    }

    sock_str(sock, (const uint8_t *)"\r\nHeld by\r\n  SLOT   PID  NAME                 USED\r\n");
    int any = 0;
    for (int s = 0; s < 17; s++) {
        uint64_t bytes = mi.heap_by_slot[s];
        if (!bytes)
            continue;
        any = 1;
        if (s == 16) {
            sock_str(sock, (const uint8_t *)"     -     -  (no owner)      ");
        } else {
            sock_u64w(sock, (uint64_t)s, 6);
            uint8_t id = mi.slot_task[s];
            if (id == 0xFF) {
                /* Bytes tagged to a slot with nobody in it: a leak, or a sweep
                 * that has not run yet. */
                sock_str(sock, (const uint8_t *)"     -  (exited, not freed) ");
            } else {
                sock_u64w(sock, id, 6);
                sock_str(sock, (const uint8_t *)"  ");
                uint8_t nlen = sock_task_name(sock, tasks, count, id);
                for (uint8_t p = nlen; p < 14; p++)
                    sock_str(sock, (const uint8_t *)" ");
            }
        }
        sock_kib(sock, bytes);
        sock_str(sock, (const uint8_t *)"\r\n");
    }
    if (!any)
        sock_str(sock, (const uint8_t *)"  nothing is allocated\r\n");
}

static void cmd_bg(TcpSocket_T *sock, const uint8_t *arg) {
    if (!arg || !arg[0]) {
        sock_str(sock, (const uint8_t *)"bg: usage: bg <name> [args...]\r\n");
        return;
    }

    /* Extract the binary name (up to first space) for the file lookup. */
    uint8_t name[13];
    uint8_t i = 0;
    while (i < 12 && arg[i] && arg[i] != ' ') {
        name[i] = arg[i];
        i++;
    }
    name[i] = '\0';

    if (i == 0) {
        sock_str(sock, (const uint8_t *)"bg: usage: bg <name> [args...]\r\n");
        return;
    }

    /* Pass the full arg string (name + optional args) as the argv array so
     * push_user_args tokenises it: argv[0]=name, argv[1]=first_arg, ...  */
    uint8_t pid = 0;
    if (!run_elf(name, arg, &pid)) {
        sock_str(sock, (const uint8_t *)"bg: failed to launch '");
        sock_str(sock, name);
        sock_str(sock, (const uint8_t *)"'\r\n");
    } else {
        sock_str(sock, (const uint8_t *)"bg: launched '");
        sock_str(sock, name);
        sock_str(sock, (const uint8_t *)"' pid=");
        sock_u32(sock, pid);
        sock_str(sock, (const uint8_t *)"\r\n");
    }
}

/*
 *  get: hand a file to another computer over a data connection of its own,
 *  since the telnet one cannot carry it (see xfer.h).  The instructions go
 *  out before the wait starts, so the user knows what to run.
 */
static void cmd_get(TcpSocket_T *sock, TcpSocket_T sockets[MAX_SOCKETS], const uint8_t *arg) {
    uint8_t path[64];
    uint8_t i = 0;

    while (arg[i] && arg[i] != ' ' && i < 63) {
        path[i] = arg[i];
        i++;
    }
    path[i] = '\0';

    uint32_t port = 0;
    const uint8_t *p = arg + i;
    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9' && port < 65536)
        port = port * 10 + (uint32_t)(*p++ - '0');
    if (!port)
        port = XFER_PORT;

    if (!i || *p || port > 65535 || port == 23) {
        sock_str(sock, (const uint8_t *)"get: usage: get <path> [port]\r\n");
        return;
    }

    uint8_t abs[64];
    build_abs_path(abs, path);

    const uint8_t *name = abs;
    for (uint8_t k = 0; abs[k]; k++)
        if (abs[k] == '/')
            name = abs + k + 1;

    int64_t size = xfer_file_size(abs);
    if (size == -1) {
        sock_str(sock, (const uint8_t *)"get: cannot read '");
        sock_str(sock, abs);
        sock_str(sock, (const uint8_t *)"'\r\n");
        return;
    }

    uint8_t ip[4];
    net_get_local_ip(ip);

    sock_str(sock, (const uint8_t *)"get: ");
    sock_str(sock, abs);
    if (size >= 0) {
        sock_str(sock, (const uint8_t *)", ");
        sock_u32(sock, (uint32_t)size);
        sock_str(sock, (const uint8_t *)" bytes");
    }
    sock_str(sock, (const uint8_t *)"\r\nWaiting 60 s for a connection to port ");
    sock_u32(sock, port);
    sock_str(sock, (const uint8_t *)" (Enter cancels), e.g.\r\n  curl -o ");
    sock_str(sock, name);
    sock_str(sock, (const uint8_t *)" http://");
    sock_ip(sock, ip);
    sock_str(sock, (const uint8_t *)":");
    sock_u32(sock, port);
    sock_str(sock, (const uint8_t *)"/\r\n  nc ");
    sock_ip(sock, ip);
    sock_str(sock, (const uint8_t *)" ");
    sock_u32(sock, port);
    sock_str(sock, (const uint8_t *)" > ");
    sock_str(sock, name);
    sock_str(sock, (const uint8_t *)"\r\n");
    shell_flush();

    XferResult_T res;
    XferStatus_T st = xfer_send(sockets, sock, abs, name, size, (uint16_t)port, &res);

    switch (st) {
    case XFER_OK:
        sock_str(sock, (const uint8_t *)"get: sent ");
        sock_u64w(sock, res.bytes, 0);
        sock_str(sock, (const uint8_t *)" bytes to ");
        sock_ip(sock, res.peer_ip);
        sock_str(sock, res.http ? (const uint8_t *)" (http) in " : (const uint8_t *)" (raw) in ");
        sock_u64w(sock, res.ms / 1000, 0);
        sock_str(sock, (const uint8_t *)".");
        sock_u64w(sock, (res.ms % 1000) / 100, 0);
        sock_str(sock, (const uint8_t *)" s, crc32 ");
        {
            static const uint8_t hex[] = "0123456789abcdef";
            uint8_t h[8];
            for (int k = 0; k < 8; k++)
                h[k] = hex[(res.crc32 >> (28 - 4 * k)) & 0xF];
            out(sock, h, 8);
        }
        if (res.resent) {
            sock_str(sock, (const uint8_t *)", ");
            sock_u32(sock, res.resent);
            sock_str(sock, (const uint8_t *)" segments resent");
        }
        sock_str(sock, (const uint8_t *)"\r\n");
        break;
    case XFER_NO_SOCKET:
        sock_str(sock, (const uint8_t *)"get: no free socket\r\n");
        break;
    case XFER_NO_CLIENT:
        sock_str(sock, (const uint8_t *)"get: nobody connected\r\n");
        break;
    case XFER_CANCELLED:
        sock_str(sock, (const uint8_t *)"get: cancelled\r\n");
        break;
    case XFER_STALLED:
        sock_str(sock, (const uint8_t *)"get: the receiver stopped answering\r\n");
        break;
    case XFER_RESET:
        sock_str(sock, (const uint8_t *)"get: the receiver closed the connection\r\n");
        break;
    case XFER_READ_ERROR:
        sock_str(sock, (const uint8_t *)"get: read error\r\n");
        break;
    }
}

static const uint8_t *tcp_state_name(SocketState s) {
    switch (s) {
    case SOCKET_CLOSED:
        return (const uint8_t *)"CLOSED     ";
    case SOCKET_LISTENING:
        return (const uint8_t *)"LISTEN     ";
    case SOCKET_SYN_SENT:
        return (const uint8_t *)"SYN_SENT   ";
    case SOCKET_ESTABLISHED:
        return (const uint8_t *)"ESTABLISHED";
    case SOCKET_FIN_WAIT:
        return (const uint8_t *)"FIN_WAIT   ";
    case SOCKET_CLOSE_WAIT:
        return (const uint8_t *)"CLOSE_WAIT ";
    default:
        return (const uint8_t *)"UNKNOWN    ";
    }
}

static void cmd_net(TcpSocket_T *sock, TcpSocket_T sockets[MAX_SOCKETS]) {
    uint8_t ip[4];
    uint8_t mac[6];
    net_get_local_ip(ip);
    net_get_local_mac(mac);

    sock_str(sock, (const uint8_t *)"Interface:\r\n  ip   ");
    sock_ip(sock, ip);
    sock_str(sock, (const uint8_t *)"\r\n  mac  ");
    sock_mac(sock, mac);

    /* The rest as the eth driver published it (syscall 0x3d). */
    NetConfig_T cfg;
    for (uint32_t i = 0; i < sizeof(cfg); i++)
        ((uint8_t *)&cfg)[i] = 0;
    if (get_net_config(&cfg) == 0) {
        static const char *const sources[] = {"not set", "static", "DHCP", "fallback, no DHCP answer yet"};
        sock_str(sock, (const uint8_t *)"\r\n  mask ");
        sock_ip(sock, cfg.netmask);
        sock_str(sock, (const uint8_t *)"\r\n  gw   ");
        sock_ip(sock, cfg.gateway);
        if (cfg.gateway_mac[0] | cfg.gateway_mac[1] | cfg.gateway_mac[2] | cfg.gateway_mac[3] |
            cfg.gateway_mac[4] | cfg.gateway_mac[5]) {
            sock_str(sock, (const uint8_t *)" at ");
            sock_mac(sock, cfg.gateway_mac);
        }
        sock_str(sock, (const uint8_t *)"\r\n  dns  ");
        sock_ip(sock, cfg.dns);
        sock_str(sock, (const uint8_t *)"\r\n  from ");
        sock_str(sock, (const uint8_t *)sources[cfg.source <= 3 ? cfg.source : 0]);
    }
    sock_str(sock, (const uint8_t *)"\r\n\r\nConnections:\r\n");

    uint8_t found = 0;
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        TcpSocket_T *s = &sockets[i];
        if (!s->used)
            continue;
        found = 1;

        sock_str(sock, (const uint8_t *)"  ");
        sock_str(sock, tcp_state_name(s->state));
        sock_str(sock, (const uint8_t *)"  ");

        if (s->state == SOCKET_LISTENING) {
            sock_str(sock, (const uint8_t *)"*:");
            sock_u32(sock, s->local_port);
        } else {
            sock_ip(sock, s->remote_ip);
            sock_str(sock, (const uint8_t *)":");
            sock_u32(sock, s->remote_port);
            sock_str(sock, (const uint8_t *)"  ->  ");
            sock_ip(sock, s->local_ip);
            sock_str(sock, (const uint8_t *)":");
            sock_u32(sock, s->local_port);
        }
        sock_str(sock, (const uint8_t *)"\r\n");
    }

    if (!found)
        sock_str(sock, (const uint8_t *)"  (none)\r\n");
}

static int str_eq(const uint8_t *a, const uint8_t *b) {
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }

    return *a == *b;
}

static const uint8_t *str_after(const uint8_t *s, const uint8_t *prefix) {
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }

    return s;
}

int shell_dispatch(TcpSocket_T *sock, TcpSocket_T sockets[MAX_SOCKETS], uint8_t *line, uint8_t len) {
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r'))
        len--;
    line[len] = '\0';

    if (len == 0)
        return 0;

    const uint8_t *arg;

    if (str_eq(line, (const uint8_t *)"help")) {
        cmd_help(sock);
    } else if (str_eq(line, (const uint8_t *)"sysinfo")) {
        cmd_sysinfo(sock);
    } else if (str_eq(line, (const uint8_t *)"mount")) {
        cmd_mount(sock);
    } else if (str_eq(line, (const uint8_t *)"net")) {
        cmd_net(sock, sockets);
    } else if (str_eq(line, (const uint8_t *)"exit") || str_eq(line, (const uint8_t *)"quit")) {
        sock_str(sock, (const uint8_t *)"Goodbye.\r\n");
        return 1;
    } else if (str_eq(line, (const uint8_t *)"ls")) {
        cmd_ls(sock, (const uint8_t *)0);
    } else if ((arg = str_after(line, (const uint8_t *)"ls "))) {
        cmd_ls(sock, arg);
    } else if ((arg = str_after(line, (const uint8_t *)"cd "))) {
        cmd_cd(sock, arg);
    } else if ((arg = str_after(line, (const uint8_t *)"read "))) {
        cmd_read(sock, sockets, arg);
    } else if ((arg = str_after(line, (const uint8_t *)"get "))) {
        cmd_get(sock, sockets, arg);
    } else if ((arg = str_after(line, (const uint8_t *)"bg "))) {
        cmd_bg(sock, arg);
    } else if ((arg = str_after(line, (const uint8_t *)"kill "))) {
        cmd_kill(sock, arg);
    } else if ((arg = str_after(line, (const uint8_t *)"play "))) {
        cmd_play(sock, arg);
    } else if (str_eq(line, (const uint8_t *)"ts")) {
        cmd_ts(sock);
    } else if (str_eq(line, (const uint8_t *)"stop")) {
        cmd_stop(sock);
    } else if (str_eq(line, (const uint8_t *)"meminfo") || str_eq(line, (const uint8_t *)"mem")) {
        cmd_meminfo(sock);
    } else if (str_eq(line, (const uint8_t *)"heap")) {
        cmd_heap(sock);
    } else {
        sock_str(sock, (const uint8_t *)"Unknown command: ");
        sock_str(sock, line);
        sock_str(sock, (const uint8_t *)"\r\n");
    }

    return 0;
}
