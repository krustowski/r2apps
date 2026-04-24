#include "router.h"

/* Route: GET /<filename> — read from the FAT filesystem and serve in chunks.
 * buf/cap is a caller-supplied scratch buffer (reuse main's temp_buf to avoid
 * a large stack allocation here — BSS/static is not available in this env). */
void route_file(TcpSocket_T *client, const uint8_t *name, uint8_t *buf, uint32_t cap) {
    /* Step 1: scan directory to get the real file_size.
     * The read_file syscall always copies a full 512-byte sector, so strlen would
     * stop early at any null byte in the padding — we need the FAT entry's size. */
    uint32_t file_size = 0;
    Entry_T *entries = (Entry_T *)buf; /* borrow buf before read_file overwrites it */
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

    /* Use FAT file_size when available; fall back to strlen only if lookup failed. */
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

/* Route: GET / */
void route_root(TcpSocket_T *client) {
    const uint8_t body[] = "<html><head><title>garn</title></head><body>"
                           "<h1>rou2exOS / garn</h1>"
                           "<p>HTTP/1.0 server running on the r2 kernel.</p>"
                           "<p>Time: <b id=\"up\">connecting...</b></p>"
                           "<ul>"
                           "<li><a href=\"/info\">/info</a> &mdash; system information</li>"
                           "<li><a href=\"/events\">/events</a> &mdash; SSE uptime stream</li>"
                           "<li><a href=\"/HELLO.TXT\">/HELLO.TXT</a> &mdash; example file</li>"
                           "</ul>"
                           "<script>"
                           "var es=new EventSource('/events');"
                           "es.addEventListener('time',function(e){"
                           "document.getElementById('up').textContent=e.data;});"
                           "</script>"
                           "</body></html>";

    respond(client, 200, (const uint8_t *)"OK", (const uint8_t *)"text/html; charset=utf-8", body, strlen(body));
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