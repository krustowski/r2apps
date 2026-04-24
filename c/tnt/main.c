#include "mem.h"
#include "net.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

#include "shell.h"

/*
 *  tnt
 *
 *  TELNET server on port 23 for rou2exOS.
 *
 *  Accepts TCP connections via the libcr2 TCP/IP stack (SLIP or Ethernet),
 *  greets each client with a banner, and dispatches r2sh-compatible commands
 *  with output written back to the socket instead of the kernel console.
 *
 *  krusty@vxn.dev / Apr 24, 2026
 */

#define BIND_PORT 23

typedef struct {
    uint8_t active;
    uint8_t line[LINE_CAP];
    uint8_t llen;
} Session_T;

uint8_t debug = 0;

int main(int argc, char **argv) {
    const uint8_t *net_arg = (const uint8_t *)"slip";

    if (argc > 1) {
        printf((const uint8_t *)"tnt: started with args:");
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

    uint8_t packet_buf[2048];
    uint8_t tcp_buf[1500];
    uint8_t rx_buf[RX_BUFFER_SIZE];

    /* Static so BSS zeroes them — sockets need used=0, sessions need active=0. */
    static TcpSocket_T sockets[MAX_SOCKETS];
    static Session_T sessions[MAX_SOCKETS];

    Ipv4Header_T ipv4_hdr;
    TcpHeader_T tcp_hdr;

    TcpSocket_T *server = socket_tcp4(sockets);
    bind(server, BIND_PORT);
    listen(server);

    print((const uint8_t *)"-> tnt telnet server starting on port 23\n");

    if (net_driver_bind_port(net_arg, BIND_PORT) < 0) {
        print((const uint8_t *)"-> net driver init failed\n");
        return 1;
    }
    printf((const uint8_t *)"-> net driver: %s\n", net_arg);

    for (;;) {
        int64_t n = net_drv.recv(packet_buf, sizeof(packet_buf));

        if (n > 0) {
            uint16_t hlen = parse_ipv4_packet(packet_buf, &ipv4_hdr);
            if (hlen && ipv4_hdr.protocol == 6) {
                uint32_t seg_len = (uint32_t)n - hlen;

                memcpy(tcp_buf, packet_buf + hlen, (uint16_t)seg_len);

                parse_tcp_packet(tcp_buf, &tcp_hdr);
                on_tcp_packet(ipv4_hdr.source_addr, ipv4_hdr.destination_addr, &tcp_hdr, tcp_buf, seg_len, sockets);
            }
        }

        /*
         *  Greet newly ESTABLISHED connections; clean up sessions for closed
         *  sockets.  The scan runs every iteration so the banner is sent as
         *  soon as the SYN arrives — before the client sends any data.
         */
        for (int i = 0; i < MAX_SOCKETS; i++) {
            TcpSocket_T *s = &sockets[i];

            if (s->used && s->state == SOCKET_ESTABLISHED && s->local_port == BIND_PORT) {
                if (!sessions[i].active) {
                    sessions[i].active = 1;
                    sessions[i].llen = 0;

                    shell_banner(s);
                    shell_prompt(s);
                }
            } else if (!s->used && sessions[i].active) {
                sessions[i].active = 0;
                sessions[i].llen = 0;
            }
        }

        TcpSocket_T *client = accept(server, sockets);
        if (!client)
            continue;

        Session_T *sess = &sessions[client->id];
        if (!sess->active)
            continue;

        uint32_t rn = read(client, rx_buf, sizeof(rx_buf));

        for (uint32_t j = 0; j < rn; j++) {
            uint8_t b = rx_buf[j];

            /*
             *  Strip TELNET IAC option sequences so they don't pollute the
             *  line buffer.  IAC (0xFF) is followed by a command byte; if the
             *  command is WILL/WONT/DO/DONT (0xFB–0xFE) there is also an
             *  option byte — skip all three bytes in that case.
             */
            if (b == 0xFF) {
                if (j + 1 < rn)
                    j += (rx_buf[j + 1] >= 0xFB) ? 2 : 1;
                continue;
            }

            if (b == '\r') {
                /* TELNET sends CRLF; \n below fires on the LF */
                continue;
            }

            if (b == '\n') {
                int quit = shell_dispatch(client, sess->line, sess->llen);
                sess->llen = 0;
                if (quit) {
                    close(client);
                    sessions[client->id].active = 0;
                    break;
                }
                shell_prompt(client);
                continue;
            }

            if (b == 0x7f || b == '\b') { /* DEL or BS */
                if (sess->llen > 0)
                    sess->llen--;
                continue;
            }

            if (sess->llen < LINE_CAP - 1)
                sess->line[sess->llen++] = b;
        }
    }

    return 0;
}
