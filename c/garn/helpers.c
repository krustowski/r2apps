#include "helpers.h"

/* Returns the part after the last '.', or "" if no dot. */
const uint8_t *ext_of(const uint8_t *name) {
    const uint8_t *dot = 0;
    for (const uint8_t *p = name; *p; p++)
        if (*p == '.')
            dot = p + 1;
    return dot ? dot : (const uint8_t *)"";
}

const uint8_t *content_type_for(const uint8_t *ext) {
    if (memcmp(ext, (const uint8_t *)"txt", 4) == 0 || memcmp(ext, (const uint8_t *)"TXT", 4) == 0)
        return (const uint8_t *)"text/plain; charset=utf-8";
    if (memcmp(ext, (const uint8_t *)"htm", 4) == 0 || memcmp(ext, (const uint8_t *)"HTM", 4) == 0 || memcmp(ext, (const uint8_t *)"html", 5) == 0 || memcmp(ext, (const uint8_t *)"HTML", 5) == 0)
        return (const uint8_t *)"text/html; charset=utf-8";
    return (const uint8_t *)"application/octet-stream";
}

/* Match a null-terminated FAT 8.3 name like "DEBUG.TXT" against an entry.
 * FAT stores names space-padded and uppercase; comparison is case-insensitive. */
int fat_name_eq(const Entry_T *e, const uint8_t *name) {
    uint8_t ni = 0, ei = 0;
    while (name[ni] && name[ni] != '.') {
        if (ei >= 8)
            return 0;
        uint8_t c = name[ni] >= 'a' ? name[ni] - 32 : name[ni];
        if (e->name[ei] != c)
            return 0;
        ni++;
        ei++;
    }
    while (ei < 8) {
        if (e->name[ei++] != ' ')
            return 0;
    }
    if (name[ni] == '.') {
        ni++;
        uint8_t xi = 0;
        while (name[ni] && xi < 3) {
            uint8_t c = name[ni] >= 'a' ? name[ni] - 32 : name[ni];
            if (e->ext[xi] != c)
                return 0;
            ni++;
            xi++;
        }
        while (xi < 3) {
            if (e->ext[xi++] != ' ')
                return 0;
        }
    } else {
        for (uint8_t xi = 0; xi < 3; xi++) {
            if (e->ext[xi] != ' ')
                return 0;
        }
    }
    return name[ni] == 0;
}

/*
 *  respond()
 *
 *  Sends a complete HTTP/1.0 response: status line, headers, blank line, body.
 */
void respond(TcpSocket_T *client, uint16_t status, const uint8_t *reason, const uint8_t *content_type, const uint8_t *body, uint32_t body_len) {
    uint8_t hdr[256];
    uint8_t num[12];
    uint32_t n = 0;

    n = str_append(hdr, n, (const uint8_t *)"HTTP/1.0 ");
    u32_to_str(status, num);
    n = str_append(hdr, n, num);
    hdr[n++] = ' ';
    n = str_append(hdr, n, reason);
    n = str_append(hdr, n, (const uint8_t *)"\r\nContent-Type: ");
    n = str_append(hdr, n, content_type);
    n = str_append(hdr, n, (const uint8_t *)"\r\nContent-Length: ");
    u32_to_str(body_len, num);
    n = str_append(hdr, n, num);
    n = str_append(hdr, n, (const uint8_t *)"\r\nConnection: close\r\n\r\n");

    write(client, hdr, n);
    if (body_len && body)
        write(client, body, body_len);
}