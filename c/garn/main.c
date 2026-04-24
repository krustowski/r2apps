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

uint8_t debug = 0;

int main(int argc, char **argv) {
    const uint8_t *net_arg = (const uint8_t *)"slip";

    if (argc > 1) {
        printf((const uint8_t *)"garn: started with args:");
        for (int i = 1; i < argc; i++) {
            printf((const uint8_t *)" %s", (uint8_t *)argv[i]);

            if (memcmp((uint8_t *)argv[i], (uint8_t *)"debug", 5) == 0) {
                debug = 1;
            } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"--net", 5) == 0 && i + 1 < argc) {
                net_arg = (const uint8_t *)argv[i + 1];
                i++;
                printf((const uint8_t *)" %s", (uint8_t *)argv[i]);
            } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"eth", 3) == 0) {
                net_arg = (const uint8_t *)"eth";
            } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"slip", 4) == 0) {
                net_arg = (const uint8_t *)"slip";
            }
        }
        print((const uint8_t *)"\n");
    }

    uint8_t temp_buf[2048];
    uint8_t packet_buf[2048];
    uint8_t tcp_packet[1500];
    uint8_t req_buf[1024];
    TcpSocket_T sockets[MAX_SOCKETS];

    Ipv4Header_T ipv4_header;
    uint16_t ipv4_header_len = 0;

    TcpHeader_T tcp_header;

    TcpSocket_T *server = socket_tcp4(sockets);
    bind(server, BIND_PORT);

    listen(server);

    TcpSocket_T *sse_client = 0;
    uint16_t sse_remote_port = 0; /* discriminator: ephemeral port changes on reconnect */
    uint8_t sse_last_sec = 0xff;  /* last RTC second we processed; 0xff = none yet */
    uint32_t hb_iter = 0;         /* heartbeat counter — keeps recv from blocking indefinitely */

    print((const uint8_t *)"-> garn HTTP/1.0 service starting on port 80\n");

    if (net_driver_bind_port(net_arg, BIND_PORT) < 0) {
        print((const uint8_t *)"-> net driver init failed\n");
        return 1;
    }
    printf((const uint8_t *)"-> net driver: %s\n", net_arg);

    for (;;) {
        if (sse_client) {
            if (sse_client->state != SOCKET_ESTABLISHED || sse_client->remote_port != sse_remote_port) {
                sse_client = 0;
                hb_iter = 0;
            } else {
                /* receive_data blocks until a packet arrives; the only way to ensure it
                 * returns in time for the next 5-second event is to generate incoming traffic
                 * ourselves.  Send a tiny SSE comment every ~64 iterations so the browser ACKs
                 * it, which unblocks recv within one round-trip (~1 ms on a local link). */
                uint8_t do_hb = 0;
                RTC_T rtc;
                if (read_rtc(&rtc) && rtc.seconds != sse_last_sec) {
                    sse_last_sec = rtc.seconds;
                    if (rtc.seconds % 5 == 0) {
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
                        hb_iter = 0;
                    } else {
                        do_hb = 1; /* non-event second — heartbeat wakes up recv */
                    }
                } else if ((++hb_iter & 0x3F) == 0) {
                    do_hb = 1; /* throttled keepalive between second changes */
                }
                if (do_hb) {
                    static const uint8_t hb[] = ": \n";
                    write(sse_client, hb, sizeof(hb) - 1);
                }
            }
        }

        int64_t decoded_len = net_drv.recv(packet_buf, sizeof(packet_buf));
        if (decoded_len <= 0)
            continue;

        ipv4_header_len = parse_ipv4_packet(packet_buf, &ipv4_header);
        if (!ipv4_header_len || ipv4_header.protocol != 6)
            continue;

        memcpy(tcp_packet, packet_buf + ipv4_header_len, (uint32_t)decoded_len - ipv4_header_len);
        parse_tcp_packet(tcp_packet, &tcp_header);

        if (debug) {
            printf((const uint8_t *)">> TCP src=%u dst=%u seq=%u\n", tcp_header.source_port, tcp_header.dest_port, tcp_header.seq_num);
        }

        on_tcp_packet(ipv4_header.source_addr, ipv4_header.destination_addr, &tcp_header, tcp_packet, (uint32_t)decoded_len - ipv4_header_len, sockets);

        TcpSocket_T *client = accept(server, sockets);
        if (!client)
            continue;

        /* SSE socket stays open — incoming data is HTTP header continuations, not new requests.
         * Guard with remote_port: if the slot was freed and reallocated for a new connection,
         * the new connection will have a different ephemeral port and must be processed normally. */
        if (client == sse_client && client->remote_port == sse_remote_port) {
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

        /* HTTP/1.1 browsers may send an absolute URI: http://10.3.4.2/foo
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

        if (debug) {
            printf((const uint8_t *)"-> %s %s\n", method, rpath);
        }

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
            if (sse_client && sse_client->state == SOCKET_ESTABLISHED && sse_client->remote_port == sse_remote_port)
                close(sse_client);
            route_events(client);
            sse_client = client;
            sse_remote_port = client->remote_port;
            sse_last_sec = 0xff;
            hb_iter = 0;
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
