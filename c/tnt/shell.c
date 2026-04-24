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

void shell_banner(TcpSocket_T *sock) {
    sock_str(sock, (const uint8_t *)"\r\ntnt - rou2exOS telnet service\r\n"
                                    "Type 'help' for commands.\r\n\r\n");
}

void shell_prompt(TcpSocket_T *sock) { sock_str(sock, (const uint8_t *)"r2sh> "); }

static void cmd_help(TcpSocket_T *sock) {
    sock_str(sock, (const uint8_t *)"Commands:\r\n"
                                    "  help          show this message\r\n"
                                    "  ls            list root directory\r\n"
                                    "  run <name>    execute ELF binary\r\n"
                                    "  sysinfo       show system information\r\n"
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
    sock_str(sock, (const uint8_t *)")");
    sock_str(sock, (const uint8_t *)"\r\n");
    sock_str(sock, (const uint8_t *)"Version: ");
    sock_str(sock, si.system_version);
    sock_str(sock, (const uint8_t *)"\r\n");
    sock_str(sock, (const uint8_t *)"Uptime:  ");
    sock_u32(sock, si.system_uptime);
    sock_str(sock, (const uint8_t *)" ticks\r\n");
}

static void cmd_ls(TcpSocket_T *sock) {
    SysInfo_T si = {0};

    if (!read_sysinfo(&si)) {
        sock_str(sock, (const uint8_t *)"ls: failed\r\n");
        return;
    }

    Entry_T entries[32] = {0};

    if (!list_dir(si.system_path_cluster, entries)) {
        sock_str(sock, (const uint8_t *)"ls: failed\r\n");
        return;
    }

    for (uint8_t i = 0; i < 32; i++) {
        if (entries[i].name[0] == 0x00)
            break;
        if (entries[i].name[0] == 0xe5)
            continue;

        entries[i].name[7] = '\0';
        sock_str(sock, (const uint8_t *)"  ");
        sock_str(sock, entries[i].name);

        if (entries[i].attr & 0x10) {
            entries[i].attr = 0;

            sock_str(sock, (const uint8_t *)"  <DIR>\r\n");
        } else {
            entries[i].attr = 0;

            sock_str(sock, (const uint8_t *)"  ");
            sock_u32(sock, entries[i].file_size);
            sock_str(sock, (const uint8_t *)" bytes\r\n");
        }
    }
}

static void cmd_run(TcpSocket_T *sock, const uint8_t *name) {
    uint8_t pid = 0;
    if (!run_elf(name, &pid)) {
        sock_str(sock, (const uint8_t *)"run: failed to launch '");
        sock_str(sock, name);
        sock_str(sock, (const uint8_t *)"'\r\n");
    } else {
        sock_str(sock, (const uint8_t *)"run: launched pid=");
        sock_u32(sock, pid);
        sock_str(sock, (const uint8_t *)"\r\n");
    }
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

int shell_dispatch(TcpSocket_T *sock, uint8_t *line, uint8_t len) {
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r'))
        len--;
    line[len] = '\0';

    if (len == 0)
        return 0;

    if (str_eq(line, (const uint8_t *)"help")) {
        cmd_help(sock);
    } else if (str_eq(line, (const uint8_t *)"ls")) {
        cmd_ls(sock);
    } else if (str_eq(line, (const uint8_t *)"sysinfo")) {
        cmd_sysinfo(sock);
    } else if (str_eq(line, (const uint8_t *)"exit") || str_eq(line, (const uint8_t *)"quit")) {
        sock_str(sock, (const uint8_t *)"Goodbye.\r\n");

        return 1;
    } else {
        const uint8_t *arg = str_after(line, (const uint8_t *)"run ");
        if (arg) {
            cmd_run(sock, arg);
        } else {
            sock_str(sock, (const uint8_t *)"Unknown command: ");
            sock_str(sock, line);
            sock_str(sock, (const uint8_t *)"\r\n");
        }
    }

    return 0;
}
