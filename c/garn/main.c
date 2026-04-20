#include "mem.h"
#include "net.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

#include "helpers.h"
#include "parser.h"
#include "router.h"

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

    TcpSocket_T *sse_client = 0;
    uint8_t sse_last_sec = 0xff; /* RTC seconds at last send; 0xff = never sent */
    uint32_t sse_idle = 0;       /* throttle counter — read RTC every N idle iterations */

    print((const uint8_t *)"-> garn HTTP/1.0 service starting on port 80\n");

    if (!serial_init()) {
        print((const uint8_t *)"-> serial port could not be initialized\n");
        return 1;
    }

    for (;;) {
        if (!serial_read(&value)) {
            /* Serial port idle — push SSE time event every ~5 seconds via RTC. */
            if (sse_client && (++sse_idle & 0x3FFF) == 0) {
                if (sse_client->state != SOCKET_ESTABLISHED) {
                    sse_client = 0;
                } else {
                    RTC_T rtc;
                    if (read_rtc(&rtc) && rtc.seconds % 5 == 0 && rtc.seconds != sse_last_sec) {
                        sse_last_sec = rtc.seconds;

                        /* Format HH:MM:SS */
                        uint8_t evt[64];
                        uint8_t num[12];
                        uint32_t en = 0;
                        en = str_append(evt, en, (const uint8_t *)"event: time\ndata: ");
                        u32_to_str(rtc.hours, num);
                        en = str_append(evt, en, num);
                        evt[en++] = ':';
                        u32_to_str(rtc.minutes, num);
                        en = str_append(evt, en, num);
                        evt[en++] = ':';
                        u32_to_str(rtc.seconds, num);
                        en = str_append(evt, en, num);
                        evt[en++] = '\n';
                        evt[en++] = '\n';
                        write(sse_client, evt, en);
                    }
                }
            }
            continue;
        }

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

        /* SSE socket stays open — incoming data is HTTP header continuations, not new requests. */
        if (client == sse_client) {
            client->rx_len = 0;
            continue;
        }

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

        /* HTTP/1.1 browsers may send an absolute URI: http://10.3.3.2/foo
         * Strip scheme://host so routing always sees a path starting with '/'. */
        const uint8_t *rpath = path;
        if (rpath[0] != '/') {
            const uint8_t *p = rpath;
            while (*p && !(*p == ':' && p[1] == '/' && p[2] == '/'))
                p++;
            if (*p) {
                p += 3;
                while (*p && *p != '/')
                    p++;
            }
            rpath = (*p == '/') ? p : (const uint8_t *)"/";
        }

        printf((const uint8_t *)"-> %s %s\n", method, rpath);

        uint8_t do_close = 1;

        /* Silently drop non-request TCP segments (e.g. HTTP header continuations).
         * Real HTTP methods are pure uppercase letters; header names always contain ':'. */
        {
            uint8_t has_colon = 0;
            for (uint8_t i = 0; i < sizeof(method) && method[i]; i++)
                if (method[i] == ':') {
                    has_colon = 1;
                    break;
                }
            if (method[0] == '\0' || has_colon)
                continue;
        }

        if (memcmp(method, (const uint8_t *)"GET", 4) != 0) {
            const uint8_t b[] = "Method Not Allowed";
            respond(client, 405, (const uint8_t *)"Method Not Allowed", (const uint8_t *)"text/plain", b, strlen(b));

        } else if (rpath[0] == '/' && rpath[1] == '\0') {
            route_root(client);

        } else if (memcmp(rpath, (const uint8_t *)"/info", 6) == 0) {
            route_info(client);

        } else if (memcmp(rpath, (const uint8_t *)"/events", 8) == 0) {
            if (sse_client && sse_client->state == SOCKET_ESTABLISHED)
                close(sse_client);
            route_events(client);
            sse_client = client;
            sse_last_sec = 0xff; /* reset so the first event fires immediately */
            do_close = 0;

        } else if (rpath[0] == '/' && rpath[1] != '\0') {
            route_file(client, rpath + 1, temp_buf, sizeof(temp_buf));

        } else {
            const uint8_t b[] = "Not Found";
            respond(client, 404, (const uint8_t *)"Not Found", (const uint8_t *)"text/plain", b, strlen(b));
        }

        if (do_close)
            close(client);
    }

    return 0;
}
