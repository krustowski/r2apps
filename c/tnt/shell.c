#include "mem.h"
#include "net.h"
#include "string.h"
#include "syscall.h"

#include "bsh.h"
#include "shell.h"
#include "xfer.h"

/*
 *  The telnet side of the shell.  The commands are bsh's (c/bsh), the same
 *  as r2sh's; what is here is what only a TCP connection has: where the
 *  output goes, and the commands that need the network --- `get`, `net`, and
 *  a `read` that streams the file with resends.
 */

/*
 *  Output is gathered here and goes out in segments of up to OUT_CAP bytes
 *  when the command is done (shell_flush), instead of one TCP segment per
 *  string: `mem` alone was fifty.  The TCP layer does not retransmit, so a
 *  segment the NIC could not take in a burst was lost for good and the
 *  session hung on the gap; fewer, fuller segments make that much rarer.
 */
#define OUT_CAP 1024
static uint8_t out_buf[OUT_CAP];
static uint32_t out_len = 0;
static TcpSocket_T *out_sock = 0;

void shell_flush(void) {
    if (out_sock && out_len)
        write(out_sock, out_buf, out_len);
    out_len = 0;
}

static void out(TcpSocket_T *sock, const uint8_t *s, uint32_t n) {
    if (sock != out_sock) {
        shell_flush();
        out_sock = sock;
    }
    while (n) {
        if (out_len == OUT_CAP)
            shell_flush();
        uint32_t room = OUT_CAP - out_len;
        uint32_t chunk = n < room ? n : room;
        memcpy(out_buf + out_len, s, (uint16_t)chunk);
        out_len += chunk;
        s += chunk;
        n -= chunk;
    }
}

static int peer_telnet = 0;
void shell_set_telnet(int on) { peer_telnet = on; }

/*  A session per socket slot, and what its commands need of the connection.  */
typedef struct {
    TcpSocket_T *sock;
    TcpSocket_T *sockets;
} Conn_T;

static BshSession sessions[MAX_SOCKETS];
static Conn_T conns[MAX_SOCKETS];

static BshSession *session_of(TcpSocket_T *sock) { return &sessions[sock->id % MAX_SOCKETS]; }
static Conn_T *conn_of(BshSession *s) { return (Conn_T *)s->host; }

/*  bsh's text to the connection: '\n' as "\r\n" (a "\r\n" already there is
 *  left as it is), and for a telnet client a 0xff byte doubled, or it takes
 *  it for a TELNET command.  */
static void tnt_write(BshSession *s, const uint8_t *text, uint32_t len) {
    static uint8_t last = 0;
    TcpSocket_T *sock = conn_of(s)->sock;
    uint32_t from = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = text[i];
        if ((b == '\n' && last != '\r') || (b == 0xFF && peer_telnet)) {
            out(sock, text + from, i - from);
            out(sock, b == '\n' ? (const uint8_t *)"\r" : (const uint8_t *)"\xff", 1);
            from = i;
        }
        last = b;
    }
    out(sock, text + from, len - from);
}

static void tnt_clear(BshSession *s) { bsh_str(s, "\x1b[2J\x1b[H"); /* ANSI: erase, cursor home */ }

/* ------------------------------------------------------------------------ *
 *  tnt's own commands
 * ------------------------------------------------------------------------ */

/*
 *  `read`: the file from the user heap (bsh_load_file), sent down the session
 *  the way `get` sends a download --- following the client's ACKs and sending
 *  again what went missing --- since libcr2's TCP does not, and a file of any
 *  size is far more than one burst of segments.  A key pressed meanwhile
 *  stops it.
 */
static int cmd_read(BshSession *s, const uint8_t *arg) {
    if (!arg[0]) {
        bsh_str(s, "read: usage: read <path>\n");
        return 0;
    }
    uint8_t abs[BSH_PATH];
    bsh_abs_path(s, abs, arg);
    uint8_t *buf;
    uint32_t len;
    if (!bsh_load_file(s, "read", abs, &buf, &len))
        return 0;
    if (!len) {
        free(buf);
        bsh_str(s, "(empty file)\n");
        return 0;
    }

    /*  For the terminal, as tnt_write does it: a bare \n becomes \r\n (one
     *  already after a \r is left as it is), and for a telnet client a 0xff
     *  byte is doubled.  In place from the end, in the block grown by what
     *  that adds.  */
    int dbl = peer_telnet;
    uint32_t extra = 0;
    for (uint32_t j = 0; j < len; j++)
        if ((buf[j] == '\n' && (!j || buf[j - 1] != '\r')) || (dbl && buf[j] == 0xFF))
            extra++;
    if (extra) {
        uint8_t *grown = realloc(buf, len + extra);
        if (!grown) {
            free(buf);
            bsh_str(s, "read: no memory for ");
            bsh_u64(s, len + extra, 0);
            bsh_str(s, " bytes\n");
            return 0;
        }
        buf = grown;
        uint32_t w = len + extra;
        for (uint32_t j = len; j-- > 0;) {
            uint8_t b = buf[j];
            buf[--w] = b;
            if (b == '\n' && (!j || buf[j - 1] != '\r'))
                buf[--w] = '\r';
            else if (dbl && b == 0xFF)
                buf[--w] = 0xFF;
        }
        len += extra;
    }

    Conn_T *c = conn_of(s);
    shell_flush(); /* what came before goes first */
    XferResult_T res;
    XferStatus_T st = xfer_send_buffer(c->sockets, c->sock, buf, len, &res);
    free(buf);

    if (st == XFER_CANCELLED) {
        bsh_str(s, "\n[stopped after ");
        bsh_u64(s, res.bytes, 0);
        bsh_str(s, " bytes]\n");
    } else if (st == XFER_OK) {
        bsh_str(s, "\n");
    }
    /* Stalled or reset: the session is gone, and there is nobody to tell. */
    return 0;
}

/*
 *  get: hand a file to another computer over a data connection of its own,
 *  since the telnet one cannot carry it (see xfer.h).  The instructions go
 *  out before the wait starts, so the user knows what to run.
 */
static int cmd_get(BshSession *s, const uint8_t *arg) {
    uint8_t path[BSH_PATH];
    uint8_t i = 0;
    while (arg[i] && arg[i] != ' ' && i < BSH_PATH - 1) {
        path[i] = arg[i];
        i++;
    }
    path[i] = '\0';

    uint32_t port = 0;
    const uint8_t *p = arg + i;
    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9' && port < 65536)
        port = port * 10 + (uint32_t)(*p++ - '0');
    if (!port)
        port = XFER_PORT;

    if (!i || *p || port > 65535 || port == 23) {
        bsh_str(s, "get: usage: get <path> [port]\n");
        return 0;
    }

    uint8_t abs[BSH_PATH];
    bsh_abs_path(s, abs, path);
    const uint8_t *name = abs;
    for (uint8_t k = 0; abs[k]; k++)
        if (abs[k] == '/')
            name = abs + k + 1;

    int64_t size = bsh_file_size(abs);
    if (size == -1) {
        bsh_str(s, "get: cannot read '");
        bsh_ustr(s, abs);
        bsh_str(s, "'\n");
        return 0;
    }

    uint8_t ip[4];
    net_get_local_ip(ip);

    bsh_str(s, "get: ");
    bsh_ustr(s, abs);
    if (size >= 0) {
        bsh_str(s, ", ");
        bsh_u64(s, (uint64_t)size, 0);
        bsh_str(s, " bytes");
    }
    bsh_str(s, "\nWaiting 60 s for a connection to port ");
    bsh_u64(s, port, 0);
    bsh_str(s, " (Enter cancels), e.g.\n  curl -o ");
    bsh_ustr(s, name);
    bsh_str(s, " http://");
    bsh_ip(s, ip);
    bsh_str(s, ":");
    bsh_u64(s, port, 0);
    bsh_str(s, "/\n  nc ");
    bsh_ip(s, ip);
    bsh_str(s, " ");
    bsh_u64(s, port, 0);
    bsh_str(s, " > ");
    bsh_ustr(s, name);
    bsh_str(s, "\n");
    shell_flush();

    Conn_T *c = conn_of(s);
    XferResult_T res;
    XferStatus_T st = xfer_send(c->sockets, c->sock, abs, name, size, (uint16_t)port, &res);

    switch (st) {
    case XFER_OK: {
        bsh_str(s, "get: sent ");
        bsh_u64(s, res.bytes, 0);
        bsh_str(s, " bytes to ");
        bsh_ip(s, res.peer_ip);
        bsh_str(s, res.http ? " (http) in " : " (raw) in ");
        bsh_u64(s, res.ms / 1000, 0);
        bsh_str(s, ".");
        bsh_u64(s, (res.ms % 1000) / 100, 0);
        bsh_str(s, " s, crc32 ");
        static const char hex[] = "0123456789abcdef";
        uint8_t h[8];
        for (int k = 0; k < 8; k++)
            h[k] = (uint8_t)hex[(res.crc32 >> (28 - 4 * k)) & 0xF];
        bsh_out(s, h, 8);
        if (res.resent) {
            bsh_str(s, ", ");
            bsh_u64(s, res.resent, 0);
            bsh_str(s, " segments resent");
        }
        bsh_str(s, "\n");
        break;
    }
    case XFER_NO_SOCKET:
        bsh_str(s, "get: no free socket\n");
        break;
    case XFER_NO_CLIENT:
        bsh_str(s, "get: nobody connected\n");
        break;
    case XFER_CANCELLED:
        bsh_str(s, "get: cancelled\n");
        break;
    case XFER_STALLED:
        bsh_str(s, "get: the receiver stopped answering\n");
        break;
    case XFER_RESET:
        bsh_str(s, "get: the receiver closed the connection\n");
        break;
    case XFER_READ_ERROR:
        bsh_str(s, "get: read error\n");
        break;
    }
    return 0;
}

static const char *tcp_state_name(SocketState st) {
    switch (st) {
    case SOCKET_CLOSED:
        return "CLOSED     ";
    case SOCKET_LISTENING:
        return "LISTEN     ";
    case SOCKET_SYN_SENT:
        return "SYN_SENT   ";
    case SOCKET_ESTABLISHED:
        return "ESTABLISHED";
    case SOCKET_FIN_WAIT:
        return "FIN_WAIT   ";
    case SOCKET_CLOSE_WAIT:
        return "CLOSE_WAIT ";
    default:
        return "UNKNOWN    ";
    }
}

static int cmd_net(BshSession *s, const uint8_t *arg) {
    (void)arg;
    uint8_t ip[4];
    uint8_t mac[6];
    net_get_local_ip(ip);
    net_get_local_mac(mac);

    bsh_str(s, "Interface:\n  ip   ");
    bsh_ip(s, ip);
    bsh_str(s, "\n  mac  ");
    bsh_mac(s, mac);

    /* The rest as the eth driver published it (syscall 0x3d). */
    NetConfig_T cfg;
    for (uint32_t i = 0; i < sizeof(cfg); i++)
        ((uint8_t *)&cfg)[i] = 0;
    if (get_net_config(&cfg) == 0) {
        static const char *const sources[] = {"not set", "static", "DHCP", "fallback, no DHCP answer yet"};
        bsh_str(s, "\n  mask ");
        bsh_ip(s, cfg.netmask);
        bsh_str(s, "\n  gw   ");
        bsh_ip(s, cfg.gateway);
        if (cfg.gateway_mac[0] | cfg.gateway_mac[1] | cfg.gateway_mac[2] | cfg.gateway_mac[3] |
            cfg.gateway_mac[4] | cfg.gateway_mac[5]) {
            bsh_str(s, " at ");
            bsh_mac(s, cfg.gateway_mac);
        }
        bsh_str(s, "\n  dns  ");
        bsh_ip(s, cfg.dns);
        bsh_str(s, "\n  from ");
        bsh_str(s, sources[cfg.source <= 3 ? cfg.source : 0]);
    }
    bsh_str(s, "\n\nConnections:\n");

    TcpSocket_T *sockets = conn_of(s)->sockets;
    uint8_t found = 0;
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        TcpSocket_T *t = &sockets[i];
        if (!t->used)
            continue;
        found = 1;
        bsh_str(s, "  ");
        bsh_str(s, tcp_state_name(t->state));
        bsh_str(s, "  ");
        if (t->state == SOCKET_LISTENING) {
            bsh_str(s, "*:");
            bsh_u64(s, t->local_port, 0);
        } else {
            bsh_ip(s, t->remote_ip);
            bsh_str(s, ":");
            bsh_u64(s, t->remote_port, 0);
            bsh_str(s, "  ->  ");
            bsh_ip(s, t->local_ip);
            bsh_str(s, ":");
            bsh_u64(s, t->local_port, 0);
        }
        bsh_str(s, "\n");
    }
    if (!found)
        bsh_str(s, "  (none)\n");
    return 0;
}

static const BshCommand tnt_commands[] = {
    {"read", "<path>", "print a file (a key stops it)", cmd_read},
    {"get", "<path> [port]", "send a file to curl/wget/nc (port 8023)", cmd_get},
    {"net", "", "show the interface and the connections", cmd_net},
    {0, 0, 0, 0},
};

/* ------------------------------------------------------------------------ *
 *  For main.c
 * ------------------------------------------------------------------------ */

void shell_session_start(TcpSocket_T *sock) {
    BshSession *s = session_of(sock);
    Conn_T *c = &conns[sock->id % MAX_SOCKETS];
    c->sock = sock;
    c->sockets = 0;
    bsh_init(s, tnt_write, tnt_commands, c);
    s->clear = tnt_clear;
}

void shell_banner(TcpSocket_T *sock) {
    static const char banner[] = "\r\ntnt - rou2exOS telnet service\r\nType 'help' for commands.\r\n\r\n";
    out(sock, (const uint8_t *)banner, sizeof(banner) - 1);
}

void shell_prompt(TcpSocket_T *sock) { bsh_prompt(session_of(sock)); }

int shell_dispatch(TcpSocket_T *sock, TcpSocket_T sockets[MAX_SOCKETS], uint8_t *line, uint8_t len) {
    BshSession *s = session_of(sock);
    conn_of(s)->sockets = sockets;
    if (bsh_dispatch(s, line, len) != BSH_EXIT)
        return 0;
    bsh_str(s, "Goodbye.\n");
    return 1;
}
