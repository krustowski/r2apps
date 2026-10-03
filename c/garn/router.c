#include "router.h"

/* Directory listings get storage of their own, sized to the kernel contract.
 * Syscall 0x2D writes up to 64 entries of 38 bytes (2432 bytes) and takes no
 * capacity argument, so it must never share a smaller buffer: overrunning one
 * is what corrupted the socket pool, which the linker places immediately
 * above the process stack. */
#define VFS_MAX_ENTRIES 64
#define FAT_MAX_ENTRIES 32

static VfsDirEntry_T vfs_entries[VFS_MAX_ENTRIES];
static Entry_T fat_entries[FAT_MAX_ENTRIES];

/* File contents live in BSS rather than on the stack, and a guard band follows
 * them.  read_file() (syscall 0x20) also takes no capacity argument: it writes
 * the whole file wherever it is pointed.  We refuse any file whose size we
 * know exceeds the buffer, and the guard catches the case where the size could
 * not be determined up front. */
#define FILE_BUF_CAP 16384
#define FILE_GUARD 256
#define GUARD_BYTE 0xa5

static uint8_t file_buf[FILE_BUF_CAP + FILE_GUARD];

static void guard_arm(void) {
    for (uint32_t i = 0; i < FILE_GUARD; i++)
        file_buf[FILE_BUF_CAP + i] = GUARD_BYTE;
}

static uint8_t guard_intact(void) {
    for (uint32_t i = 0; i < FILE_GUARD; i++)
        if (file_buf[FILE_BUF_CAP + i] != GUARD_BYTE)
            return 0;

    return 1;
}

static void respond_status(TcpSocket_T *client, uint16_t code, const uint8_t *reason) {
    respond(client, code, reason, (const uint8_t *)"text/plain", reason, strlen(reason));
}

/* Route: GET /<filename> --- read from the filesystem and serve in chunks.
 * base_path: if non-empty, files are read from that VFS directory; otherwise
 * the kernel cwd (set by chdir at startup) is used. */
void route_file(TcpSocket_T *client, const uint8_t *name, const uint8_t *base_path) {
    uint32_t file_size = 0;
    uint8_t size_known = 0;

    if (base_path && base_path[0]) {
        /* Path-based serving: look the size up with list_dir_path, then chdir
         * to base_path, read_file, and restore the kernel cwd. */
        int64_t count = list_dir_path(base_path, vfs_entries);

        /* The kernel signals errors with -1 and never reports more than 64;
         * anything else means we cannot trust the buffer. */
        if (count > VFS_MAX_ENTRIES)
            count = VFS_MAX_ENTRIES;

        if (count > 0) {
            uint32_t nlen = strlen(name);
            for (int64_t i = 0; i < count; i++) {
                if ((uint32_t)vfs_entries[i].name_len != nlen)
                    continue;
                uint8_t match = 1;
                for (uint32_t k = 0; k < nlen; k++) {
                    uint8_t a = vfs_entries[i].name[k] >= 'a' ? vfs_entries[i].name[k] - 32 : vfs_entries[i].name[k];
                    uint8_t b2 = name[k]               >= 'a' ? name[k]               - 32 : name[k];
                    if (a != b2) { match = 0; break; }
                }
                if (match) {
                    file_size = vfs_entries[i].size;
                    size_known = 1;
                    break;
                }
            }
        }
    } else {
        /* Default: root-cluster FAT listing for the size, then read_file. */
        if (list_dir(0, fat_entries)) {
            for (uint8_t i = 0; i < FAT_MAX_ENTRIES; i++) {
                if (fat_entries[i].name[0] == 0x00)
                    break;
                if (fat_entries[i].name[0] == 0xe5)
                    continue;
                if (fat_name_eq(&fat_entries[i], name)) {
                    file_size = fat_entries[i].file_size;
                    size_known = 1;
                    break;
                }
            }
        }
    }

    /* Refuse before reading: once read_file() has run there is no way to
     * undo an overrun. */
    if (size_known && file_size > FILE_BUF_CAP) {
        respond_status(client, 413, (const uint8_t *)"Payload Too Large");
        return;
    }

    guard_arm();

    file_buf[0] = '\0';

    int64_t ok;
    if (base_path && base_path[0]) {
        /* Snapshot cwd, chdir to base_path, read, restore. */
        SysInfo_T si;
        si.system_path[0]  = '\0';
        si.system_path[31] = '\0';
        read_sysinfo(&si);
        si.system_path[31] = '\0';

        chdir(base_path);
        ok = read_file(name, file_buf);
        if (si.system_path[0])
            chdir(si.system_path);
    } else {
        ok = read_file(name, file_buf);
    }

    if (!ok) {
        respond_status(client, 404, (const uint8_t *)"Not Found");
        return;
    }

    if (!guard_intact()) {
        /* The file was bigger than the buffer and its size was not known in
         * advance, so the write has already run past the end.  Say so rather
         * than serving whatever survived. */
        print((const uint8_t *)"-> route_file: buffer overrun, file too large\n");
        respond_status(client, 500, (const uint8_t *)"Internal Server Error");
        return;
    }

    /* Use the filesystem size when we have it; fall back to strlen otherwise. */
    uint32_t len = size_known ? file_size : strlen(file_buf);
    if (len > FILE_BUF_CAP)
        len = FILE_BUF_CAP;

    /* Send headers only (body=0 so respond() skips the body write). */
    respond(client, 200, (const uint8_t *)"OK", content_type_for(ext_of(name)), 0, len);

    /* Send body in CHUNK_SIZE pieces --- kernel packet buffer is limited. */
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > CHUNK_SIZE)
            chunk = CHUNK_SIZE;
        write(client, file_buf + sent, chunk);
        sent += chunk;
    }
}

/* Route: GET /events — open an SSE stream.
 * Sends headers only; the main loop pushes events and never calls close(). */
void route_events(TcpSocket_T *client) {
    const uint8_t hdr[] = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream\r\n"
                          "Cache-Control: no-cache\r\n"
                          "Connection: keep-alive\r\n"
                          "\r\n";
    write(client, hdr, sizeof(hdr) - 1);
}

/* Route: GET /info — kernel sysinfo */
void route_info(TcpSocket_T *client) {
    uint8_t body[256];
    uint8_t num[12];
    uint8_t cluster[12];
    uint32_t n = 0;
    SysInfo_T info = {0};

    n = str_append(body, n, (const uint8_t *)"<html><body><pre>");

    if (read_sysinfo(&info)) {
        info.system_name[31] = '\0';
        info.system_user[31] = '\0';
        info.system_path[31] = '\0';
        info.system_version[7] = '\0';
        n = str_append(body, n, (const uint8_t *)"System:  ");
        n = str_append(body, n, info.system_name);
        body[n++] = '\n';
        n = str_append(body, n, (const uint8_t *)"User:    ");
        n = str_append(body, n, info.system_user);
        body[n++] = '\n';
        n = str_append(body, n, (const uint8_t *)"Version: ");
        n = str_append(body, n, info.system_version);
        body[n++] = '\n';
        n = str_append(body, n, (const uint8_t *)"System path: ");
        n = str_append(body, n, info.system_path);
        n = str_append(body, n, (const uint8_t *)" (cluster: ");
        u32_to_str(info.system_path_cluster, cluster);
        n = str_append(body, n, cluster);
        n = str_append(body, n, (const uint8_t *)")\n");
        n = str_append(body, n, (const uint8_t *)"Uptime:  ");
        u32_to_str(info.system_uptime, num);
        n = str_append(body, n, num);
        n = str_append(body, n, (const uint8_t *)" s\n");
    } else {
        n = str_append(body, n, (const uint8_t *)"sysinfo unavailable\n");
    }

    n = str_append(body, n, (const uint8_t *)"</pre></body></html>");

    respond(client, 200, (const uint8_t *)"OK", (const uint8_t *)"text/html; charset=utf-8", body, n);
}