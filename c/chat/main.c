#include "net.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

/*
 *  chat
 *
 *  Simple one-to-one TCP chat over SLIP/serial for rou2exOS.
 *  Server mode: listen on CHAT_PORT, wait for the peer to connect.
 *  Client mode: connect to PEER_IP:CHAT_PORT (sends TCP SYN).
 *
 *  Usage on the other end (host Linux with slattach):
 *    server:  nc -l -p 9000        (then r2 connects in client mode)
 *    client:  nc 10.3.3.2 9000     (r2 listens in server mode)
 *
 *  krusty@vxn.dev / Apr, 20 2026
 */

#define CHAT_PORT 9000
#define LINE_CAP 128
#define PIPE_CAP 17

/* Adjust to match your SLIP link addresses. */
static const uint8_t MY_IP[4] = {10, 3, 3, 2};
static const uint8_t PEER_IP[4] = {10, 3, 3, 1};

/* PS/2 Set 1 make-code → ASCII, unshifted. */
static const uint8_t sc_normal[0x3a] = {
    0,    0,   '1', '2',  '3', '4', '5', '6', /* 00-07 */
    '7',  '8', '9', '0',  '-', '=', 0,   0,   /* 08-0f */
    'q',  'w', 'e', 'r',  't', 'y', 'u', 'i', /* 10-17 */
    'o',  'p', '[', ']',  0,   0,   'a', 's', /* 18-1f */
    'd',  'f', 'g', 'h',  'j', 'k', 'l', ';', /* 20-27 */
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', /* 28-2f */
    'b',  'n', 'm', ',',  '.', '/', 0,   0,   /* 30-37 */
    0,    ' ',                                /* 38-39 */
};

/* PS/2 Set 1 make-code → ASCII, shifted. */
static const uint8_t sc_shift[0x3a] = {
    0, 0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,   0,   'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0,
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   0,   0,   ' ',
};

static void reprint_prompt(const uint8_t *line, uint8_t llen) {
    print((const uint8_t *)"chat> ");
    for (uint8_t i = 0; i < llen; i++) {
        uint8_t ch[2] = {line[i], '\0'};
        print(ch);
    }
}

/* Print a received message above the current input line. */
static void show_received(const uint8_t *data, uint32_t len, const uint8_t *line, uint8_t llen) {
    print((const uint8_t *)"\r\n[>] ");
    for (uint32_t i = 0; i < len; i++) {
        if (data[i] == '\r')
            continue;
        if (data[i] == '\n')
            break;
        uint8_t ch[2] = {data[i], '\0'};
        print(ch);
    }
    print((const uint8_t *)"\n");
    reprint_prompt(line, llen);
}

int main(void) {
    uint8_t pipe[PIPE_CAP];
    uint8_t line[LINE_CAP];
    uint8_t llen = 0;
    uint8_t shift = 0;

    uint8_t temp_buf[2048];
    uint8_t packet_buf[2048];
    uint8_t tcp_packet[1500];
    uint8_t rx_buf[1024];
    TcpSocket_T sockets[MAX_SOCKETS];

    uint32_t temp_len = 0;
    int64_t decoded_len = 0;
    uint32_t value = 0;

    Ipv4Header_T ipv4_header;
    uint16_t ipv4_header_len = 0;
    TcpHeader_T tcp_header;

    TcpSocket_T *chat_sock = 0;
    TcpSocket_T *server = 0;
    uint8_t mode = 0; /* 's' or 'c' */
    uint8_t connected = 0;

    for (uint8_t i = 0; i < PIPE_CAP; i++)
        pipe[i] = 0;

    print((const uint8_t *)"\nchat - TCP chat over SLIP\n");
    print((const uint8_t *)"Select mode: [s] server  [c] client\n");

    if (!pipe_subscribe(pipe)) {
        print((const uint8_t *)"chat: pipe subscribe failed\n");
        return 1;
    }

    /* Mode selection — block until 's' or 'c' is pressed. */
    while (!mode) {
        uint8_t sc = pipe[0];
        if (!sc)
            continue;
        pipe[0] = 0;
        if (sc & 0x80)
            continue; /* key release */
        if (sc == 0x1f)
            mode = 's'; /* 's' make code */
        if (sc == 0x2e)
            mode = 'c'; /* 'c' make code */
    }

    if (!serial_init()) {
        print((const uint8_t *)"chat: serial init failed\n");
        return 1;
    }

    if (mode == 's') {
        print((const uint8_t *)"[server] listening on port 9000...\n");
        server = socket_tcp4(sockets);
        bind(server, CHAT_PORT);
        listen(server);
    } else {
        print((const uint8_t *)"[client] connecting to peer...\n");
        chat_sock = tcp_connect(sockets, PEER_IP, CHAT_PORT, CHAT_PORT, MY_IP);
        if (!chat_sock) {
            print((const uint8_t *)"chat: socket alloc failed\n");
            return 1;
        }
    }

    for (;;) {
        /* ---- keyboard ---- */
        uint8_t sc = pipe[0];
        if (sc) {
            pipe[0] = 0;

            if (sc & 0x80) {
                uint8_t mk = sc & 0x7f;
                if (mk == 0x2a || mk == 0x36)
                    shift = 0;
            } else if (sc == 0x2a || sc == 0x36) {
                shift = 1;
            } else if (sc == 0x1c) {
                /* Enter — send line */
                print((const uint8_t *)"\n");
                if (chat_sock && chat_sock->state == SOCKET_ESTABLISHED && llen > 0) {
                    print((const uint8_t *)"[me] ");
                    for (uint8_t i = 0; i < llen; i++) {
                        uint8_t ch[2] = {line[i], '\0'};
                        print(ch);
                    }
                    print((const uint8_t *)"\n");
                    line[llen++] = '\n';
                    write(chat_sock, line, llen);
                }
                llen = 0;
                reprint_prompt(line, llen);
            } else if (sc == 0x01) {
                /* Escape — quit */
                print((const uint8_t *)"\r\nchat: bye\n");
                pipe_unsubscribe(pipe);
                return 0;
            } else if (sc == 0x0e) {
                /* Backspace */
                if (llen > 0) {
                    llen--;
                    print((const uint8_t *)"\b \b");
                }
            } else if (sc < 0x3a) {
                uint8_t ch = shift ? sc_shift[sc] : sc_normal[sc];
                if (ch && llen < LINE_CAP - 2) {
                    line[llen++] = ch;
                    uint8_t buf[2] = {ch, '\0'};
                    print(buf);
                }
            }
        }

        /* ---- serial / network ---- */
        if (!serial_read(&value))
            continue;

        if (temp_len >= sizeof(temp_buf))
            temp_len = 0;
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

        on_tcp_packet(ipv4_header.source_addr, ipv4_header.destination_addr, &tcp_header, tcp_packet, decoded_len - ipv4_header_len, sockets);

        /* Server: accept first incoming connection. */
        if (mode == 's' && !chat_sock) {
            TcpSocket_T *c = accept(server, sockets);
            if (c) {
                chat_sock = c;
                connected = 1;
                print((const uint8_t *)"\n[connected]\n");
                reprint_prompt(line, llen);
            }
        }

        /* Client: detect when SYN-ACK completes the handshake. */
        if (mode == 'c' && chat_sock && !connected && chat_sock->state == SOCKET_ESTABLISHED) {
            connected = 1;
            print((const uint8_t *)"\n[connected]\n");
            reprint_prompt(line, llen);
        }

        /* Read incoming chat data. */
        if (chat_sock && chat_sock->state == SOCKET_ESTABLISHED) {
            uint32_t n = read(chat_sock, rx_buf, sizeof(rx_buf) - 1);
            if (n > 0) {
                rx_buf[n] = '\0';
                show_received(rx_buf, n, line, llen);
            }
        }

        /* Detect remote disconnect. */
        if (connected && chat_sock && chat_sock->state == SOCKET_CLOSED) {
            print((const uint8_t *)"\r\n[disconnected]\r\n");
            chat_sock = 0;
            connected = 0;
            if (mode == 's') {
                /* Re-arm the server for a new connection. */
                server = socket_tcp4(sockets);
                bind(server, CHAT_PORT);
                listen(server);
                print((const uint8_t *)"[server] waiting for new connection...\n");
                reprint_prompt(line, llen);
            } else {
                pipe_unsubscribe(pipe);
                return 0;
            }
        }
    }

    return 0;
}
