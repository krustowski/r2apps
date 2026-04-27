#include "router.h"

/* Route: GET /<filename> — read from the FAT filesystem and serve in chunks.
 * buf/cap is a caller-supplied scratch buffer (reuse main's temp_buf to avoid
 * a large stack allocation here — BSS/static is not available in this env).
 * base_path: if non-empty, files are read from that VFS directory; otherwise
 * the kernel cwd (set by chdir at startup) is used. */
void route_file(TcpSocket_T *client, const uint8_t *name, uint8_t *buf, uint32_t cap, const uint8_t *base_path) {
    uint32_t file_size = 0;

    if (base_path && base_path[0]) {
        /* Path-based serving: use list_dir_path for the size, then chdir to
         * base_path, read_file, and restore the kernel cwd. */
        VfsDirEntry_T *vfs = (VfsDirEntry_T *)buf;
        int64_t count = list_dir_path(base_path, vfs);
        if (count > 0) {
            uint32_t nlen = strlen(name);
            for (int64_t i = 0; i < count; i++) {
                if ((uint32_t)vfs[i].name_len != nlen)
                    continue;
                uint8_t match = 1;
                for (uint32_t k = 0; k < nlen; k++) {
                    uint8_t a = vfs[i].name[k] >= 'a' ? vfs[i].name[k] - 32 : vfs[i].name[k];
                    uint8_t b2 = name[k]       >= 'a' ? name[k]       - 32 : name[k];
                    if (a != b2) { match = 0; break; }
                }
                if (match) { file_size = vfs[i].size; break; }
            }
        }

        /* Snapshot cwd, chdir to base_path, read, restore. */
        SysInfo_T si;
        si.system_path[0]  = '\0';
        si.system_path[31] = '\0';
        read_sysinfo(&si);
        si.system_path[31] = '\0';

        buf[0] = '\0';
        chdir(base_path);
        int64_t ok = read_file(name, buf);
        if (si.system_path[0])
            chdir(si.system_path);

        if (!ok) {
            const uint8_t b[] = "Not Found";
            respond(client, 404, (const uint8_t *)"Not Found", (const uint8_t *)"text/plain", b, strlen(b));
            return;
        }
    } else {
        /* Default: root-cluster FAT listing for file_size, then read_file. */
        Entry_T *entries = (Entry_T *)buf;
        if (list_dir(0, entries)) {
            for (uint8_t i = 0; i < 32; i++) {
                if (entries[i].name[0] == 0x00)
                    break;
                if (entries[i].name[0] == 0xe5)
                    continue;
                if (fat_name_eq(&entries[i], name)) {
                    file_size = entries[i].file_size;
                    break;
                }
            }
        }

        buf[0] = '\0';
        if (!read_file(name, buf)) {
            const uint8_t b[] = "Not Found";
            respond(client, 404, (const uint8_t *)"Not Found", (const uint8_t *)"text/plain", b, strlen(b));
            return;
        }
    }

    /* Use FAT/VFS file_size when available; fall back to strlen otherwise. */
    uint32_t len = file_size ? file_size : strlen(buf);
    if (len > cap)
        len = cap;

    /* Send headers only (body=0 so respond() skips the body write). */
    respond(client, 200, (const uint8_t *)"OK", content_type_for(ext_of(name)), 0, len);

    /* Send body in CHUNK_SIZE pieces — kernel packet buffer is limited. */
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > CHUNK_SIZE)
            chunk = CHUNK_SIZE;
        write(client, buf + sent, chunk);
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