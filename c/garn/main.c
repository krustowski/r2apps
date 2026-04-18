#include "mem.h"
#include "net.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

/*
 *  garn
 *
 *  Simple HTTP/1.0 server over TCP/SLIP for the rou2exOS kernel.
 *  Receives raw frames via SLIP over serial, handles IPv4/TCP state,
 *  and serves HTTP responses — analogous to icmpresp for ICMP.
 *
 *  krusty@vxn.dev / Aug 5, 2025
 */

#define BIND_PORT 80

/* Append a null-terminated string into buf at offset off, return new offset. */
static uint32_t str_append(uint8_t *buf, uint32_t off, const uint8_t *s) {
    while (*s)
        buf[off++] = *s++;
    return off;
}

/*
 *  respond()
 *
 *  Sends a complete HTTP/1.0 response: status line, headers, blank line, body.
 */
static void respond(TcpSocket_T *client, uint16_t status, const uint8_t *reason, const uint8_t *content_type, const uint8_t *body, uint32_t body_len) {
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
    if (body_len)
        write(client, body, body_len);
}

/* Route: GET / */
static void route_root(TcpSocket_T *client) {
    const uint8_t body[] = "<html><body>"
                           "<h1>rou2exOS / garn</h1>"
                           "<p>HTTP/1.0 server running on the r2 kernel.</p>"
                           "<ul><li><a href=\"/info\">/info</a> &mdash; system information</li></ul>"
                           "</body></html>";

    respond(client, 200, (const uint8_t *)"OK", (const uint8_t *)"text/html; charset=utf-8", body, strlen(body));
}

/* Route: GET /info — kernel sysinfo */
static void route_info(TcpSocket_T *client) {
    uint8_t body[256];
    uint8_t num[12];
    uint32_t n = 0;
    SysInfo_T info;

    n = str_append(body, n, (const uint8_t *)"<html><body><pre>");

    if (read_sysinfo(&info)) {
        n = str_append(body, n, (const uint8_t *)"System:  ");
        n = str_append(body, n, info.system_name);
        body[n++] = '\n';
        n = str_append(body, n, (const uint8_t *)"User:    ");
        n = str_append(body, n, info.system_user);
        body[n++] = '\n';
        n = str_append(body, n, (const uint8_t *)"Version: ");
        n = str_append(body, n, info.system_version);
        body[n++] = '\n';
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

/*
 *  parse_token()
 *
 *  Reads from buf[*off] up to delim (or \r), writes into out (null-terminated).
 *  Advances *off past the delimiter.
 */
static void parse_token(const uint8_t *buf, uint32_t len, uint32_t *off, uint8_t *out, uint32_t max, uint8_t delim) {
    uint32_t j = 0;
    while (*off < len && buf[*off] != delim && buf[*off] != '\r' && j < max - 1)
        out[j++] = buf[(*off)++];
    out[j] = '\0';
    if (*off < len && buf[*off] == delim)
        (*off)++;
}

int main(void) {
    uint32_t value = 0;

    uint8_t temp_buf[2048];
    uint8_t packet_buf[2048];
    uint8_t tcp_packet[1500];
    uint8_t req_buf[1024];
    TcpSocket_T sockets[MAX_SOCKETS];

    uint32_t temp_len = 0;
    int64_t decoded_len = 0;

    Ipv4Header_T ipv4_header;
    uint16_t ipv4_header_len = 0;

    TcpHeader_T tcp_header;

    TcpSocket_T *server = socket_tcp4(sockets);
    bind(server, BIND_PORT);
    listen(server);

    print((const uint8_t *)"-> garn HTTP/1.0 service starting on port 80\n");

    if (!serial_init()) {
        print((const uint8_t *)"-> serial port could not be initialized\n");
        return 1;
    }

    for (;;) {
        if (!serial_read(&value))
            continue;

        temp_buf[temp_len++] = (uint8_t)value;

        decoded_len = decode_slip(temp_buf, temp_len, packet_buf, sizeof(packet_buf));
        if (decoded_len <= 0)
            continue;
        temp_len = 0;

        ipv4_header_len = parse_ipv4_packet(packet_buf, &ipv4_header);
        if (!ipv4_header_len || ipv4_header.protocol != 6)
            continue;

        memcpy(tcp_packet, packet_buf + ipv4_header_len, decoded_len - ipv4_header_len);
        parse_tcp_packet(tcp_packet, &tcp_header);

        printf((const uint8_t *)">> TCP src=%u dst=%u seq=%u\n", tcp_header.source_port, tcp_header.dest_port, tcp_header.seq_num);

        on_tcp_packet(ipv4_header.source_addr, ipv4_header.destination_addr, &tcp_header, tcp_packet, decoded_len - ipv4_header_len, sockets);

        TcpSocket_T *client = accept(server, sockets);
        if (!client)
            continue;

        uint32_t n = read(client, req_buf, sizeof(req_buf) - 1);
        if (!n) {
            continue;
        }
        req_buf[n] = '\0';

        /* Parse: METHOD PATH HTTP/x.x\r\n... */
        uint8_t method[8];
        uint8_t path[64];
        uint32_t off = 0;

        parse_token(req_buf, n, &off, method, sizeof(method), ' ');
        parse_token(req_buf, n, &off, path, sizeof(path), ' ');

        printf((const uint8_t *)"-> %s %s\n", method, path);

        if (memcmp(method, (const uint8_t *)"GET", 4) != 0) {
            const uint8_t b[] = "Method Not Allowed";
            respond(client, 405, (const uint8_t *)"Method Not Allowed", (const uint8_t *)"text/plain", b, strlen(b));

        } else if (path[0] == '/' && path[1] == '\0') {
            route_root(client);

        } else if (memcmp(path, (const uint8_t *)"/info", 6) == 0) {
            route_info(client);

        } else {
            const uint8_t b[] = "Not Found";
            respond(client, 404, (const uint8_t *)"Not Found", (const uint8_t *)"text/plain", b, strlen(b));
        }

        close(client);
    }

    return 0;
}
