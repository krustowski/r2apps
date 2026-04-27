#include "mem.h"
#include "net.h"
#include "string.h"
#include "syscall.h"

#include "shell.h"

static void sock_str(TcpSocket_T *sock, const uint8_t *s) { write(sock, s, strlen(s)); }

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
    write(sock, buf, n);
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
    write(sock, buf, 17);
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

void shell_prompt(TcpSocket_T *sock) {
    sock_str(sock, (const uint8_t *)"[");
    sock_str(sock, cwd);
    sock_str(sock, (const uint8_t *)"]> ");
}

static void cmd_help(TcpSocket_T *sock) {
    sock_str(sock, (const uint8_t *)"Commands:\r\n"
                                    "  help          show this message\r\n"
                                    "  ls [path]     list directory (default: cwd)\r\n"
                                    "  cd <path>     change directory\r\n"
                                    "  read <path>   print file contents\r\n"
                                    "  bg <name> [args...]  run ELF in background\r\n"
                                    "  ts            list running tasks\r\n"
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

    write(sock, buf, n);
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

    VfsDirEntry_T entries[32] = {0};
    int64_t count = list_dir_path(abs, entries);

    /* Kernel returns u64::MAX (-1 as int64_t) on error; valid range is 0–32. */
    if (count < 0 || count > 32) {
        sock_str(sock, (const uint8_t *)"ls: no such directory: ");
        sock_str(sock, abs);
        sock_str(sock, (const uint8_t *)"\r\n");
        return;
    }

    for (int64_t i = 0; i < count; i++) {
        VfsDirEntry_T *e = &entries[i];
        sock_str(sock, (const uint8_t *)"  ");
        write(sock, e->name, e->name_len);
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
       valid counts are 0–32.  Reject anything outside that range. */
    VfsDirEntry_T tmp[32];
    int64_t r = list_dir_path(abs, tmp);
    if (r < 0 || r > 32) {
        sock_str(sock, (const uint8_t *)"cd: no such directory: ");
        sock_str(sock, abs);
        sock_str(sock, (const uint8_t *)"\r\n");
        return;
    }

    str_copy(cwd, abs, 64);
    chdir(abs);
}

static void cmd_read(TcpSocket_T *sock, const uint8_t *path_arg) {
    if (!path_arg || !path_arg[0]) {
        sock_str(sock, (const uint8_t *)"read: usage: read <file>\r\n");
        return;
    }

    uint8_t abs[64];
    build_abs_path(abs, path_arg);

    static uint8_t file_buf[4096];
    uint16_t i;
    for (i = 0; i < (uint16_t)sizeof(file_buf); i++)
        file_buf[i] = 0;

    /* Try the full absolute path first — ISO9660 resolves subdirectories
     * natively, and FAT12 root files work this way too. */
    int64_t r = read_file(abs, file_buf);

    if (!r) {
        /* Full-path lookup failed: likely a FAT12 subdirectory file.
         * ScReadFile only matches a single filename component, so chdir to the
         * parent directory (which uses resolve_path_from to walk subdirs) and
         * call read_file with just the bare filename, then restore cwd. */
        uint8_t parent[64];
        const uint8_t *fname = abs;
        uint8_t last = 0;
        for (uint8_t k = 0; abs[k]; k++)
            if (abs[k] == '/')
                last = k;
        if (last > 0) {
            str_copy(parent, abs, last + 1);
            parent[last] = '\0';
            fname = abs + last + 1;
        } else {
            str_copy(parent, cwd, 64);
        }
        chdir(parent);
        for (i = 0; i < (uint16_t)sizeof(file_buf); i++)
            file_buf[i] = 0;
        r = read_file(fname, file_buf);
        chdir(cwd);
    }

    if (!r) {
        sock_str(sock, (const uint8_t *)"read: failed to open '");
        sock_str(sock, abs);
        sock_str(sock, (const uint8_t *)"'\r\n");
        return;
    }

    /* Find content length (NUL-terminated or buffer end). */
    uint16_t len = 0;
    while (len < sizeof(file_buf) && file_buf[len])
        len++;

    if (len == 0) {
        sock_str(sock, (const uint8_t *)"(empty file)\r\n");
        return;
    }

    /* Send in chunks, replacing bare \n with \r\n for telnet. */
    for (uint16_t j = 0; j < len; j++) {
        if (file_buf[j] == '\n') {
            write(sock, (const uint8_t *)"\r\n", 2);
        } else {
            write(sock, &file_buf[j], 1);
        }
    }
    sock_str(sock, (const uint8_t *)"\r\n");
}

static void cmd_mount(TcpSocket_T *sock) {
    MountInfo_T mounts[8];
    int64_t count = list_mounts(mounts);
    if (count <= 0) {
        sock_str(sock, (const uint8_t *)"No mounts.\r\n");
        return;
    }
    for (int64_t i = 0; i < count; i++) {
        write(sock, mounts[i].path, mounts[i].path_len);
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
    int64_t r = play_midi_file(name);
    if (r < 0) {
        sock_str(sock, (const uint8_t *)"play: failed to open '");
        sock_str(sock, name);
        sock_str(sock, (const uint8_t *)"'\r\n");
    } else {
        sock_str(sock, (const uint8_t *)"play: started '");
        sock_str(sock, name);
        sock_str(sock, (const uint8_t *)"'\r\n");
    }
}

static void cmd_stop(TcpSocket_T *sock) {
    stop_speaker();
    sock_str(sock, (const uint8_t *)"play: stopped\r\n");
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

    sock_str(sock, (const uint8_t *)"PID  M  STATUS    NAME\r\n");
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
        write(sock, t->name, nlen);
        sock_str(sock, (const uint8_t *)"\r\n");
    }
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
        cmd_read(sock, arg);
    } else if ((arg = str_after(line, (const uint8_t *)"bg "))) {
        cmd_bg(sock, arg);
    } else if ((arg = str_after(line, (const uint8_t *)"play "))) {
        cmd_play(sock, arg);
    } else if (str_eq(line, (const uint8_t *)"ts")) {
        cmd_ts(sock);
    } else if (str_eq(line, (const uint8_t *)"stop")) {
        cmd_stop(sock);
    } else {
        sock_str(sock, (const uint8_t *)"Unknown command: ");
        sock_str(sock, line);
        sock_str(sock, (const uint8_t *)"\r\n");
    }

    return 0;
}
