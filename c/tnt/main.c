#include "mem.h"
#include "net.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

#include "auth.h"
#include "shell.h"
#include "xfer.h"

/*
 *  tnt
 *
 *  TELNET server on port 23 for rou2exOS.
 *
 *  Accepts TCP connections via the libcr2 TCP/IP stack (SLIP or Ethernet),
 *  greets each client with a banner, and dispatches r2sh-compatible commands
 *  with output written back to the socket instead of the kernel console.
 *
 *  Once Memento has been logged in to (auth.h), a new connection has to give
 *  that login and password before it gets the shell.  Connections made before
 *  are left as they are.
 *
 *  krusty@vxn.dev / Apr 24, 2026
 */

#define BIND_PORT 23

/*  Tries at the login before the connection is closed.  */
#define AUTH_TRIES 3

/*  Where a session is with the login.  */
enum {
    AUTH_DONE = 0, /* the shell: logged in, or nothing to log in to */
    AUTH_LOGIN,    /* "login: " is waiting for its answer */
    AUTH_PASSWORD  /* and then "password: " */
};

/*  TELNET: the server will echo, so the client stops echoing what is typed
 *  (and the server echoes nothing: the password does not show); and back.
 *  Only to a client that has spoken TELNET itself (Session_T.telnet): nc
 *  prints these bytes as they come.  */
static const uint8_t ECHO_OFF[] = {0xFF, 0xFB, 0x01}; /* IAC WILL ECHO */
static const uint8_t ECHO_ON[] = {0xFF, 0xFC, 0x01};  /* IAC WONT ECHO */

static void say(TcpSocket_T *sock, const char *text) {
    write(sock, (const uint8_t *)text, strlen((const uint8_t *)text));
}

typedef struct {
    uint8_t active;
    uint8_t line[LINE_CAP];
    uint8_t llen;
    uint8_t last_line[LINE_CAP]; /* last executed command for up-arrow recall */
    uint8_t last_llen;
    uint8_t esc_state;           /* 0=normal  1=saw ESC  2=saw ESC+[ */
    uint8_t telnet;              /* the client sent a TELNET command: it is one */
    uint8_t cr;                  /* the last byte ended a line with CR */
    uint8_t iac_state;           /* TELNET_* : where in a TELNET command */
    uint8_t iac_verb;            /* its WILL/WONT/DO/DONT, for the option */
    uint8_t auth;                /* AUTH_* */
    uint8_t tries;               /* failed logins so far */
    uint8_t login[LINE_CAP];     /* the login, while the password is asked */
    uint8_t login_len;
} Session_T;

/*
 *  TELNET option negotiation.
 *
 *  A telnet client offers and asks for options as it connects (to port 23;
 *  to another port only when told to, `open host -port`).  None of them is
 *  answered yes: every DO is answered WONT and every WILL is answered DONT,
 *  but for ECHO, which the server offers itself for the password.  That
 *  matters for SUPPRESS-GO-AHEAD: a client that asked for it and was never
 *  refused believes it on, and the first option it hears about afterwards
 *  (the WILL ECHO before the password) makes it work out its mode again and
 *  go to a character at a time with its own echo, where Enter shows as ^M
 *  and the server echoes nothing.  Refused, it stays in line mode, echo or
 *  not.  Commands may be split across segments: hence the state per session.
 */
enum {
    TELNET_DATA = 0, /* not in a command */
    TELNET_IAC,      /* after IAC */
    TELNET_OPTION,   /* after IAC WILL/WONT/DO/DONT: the option comes next */
    TELNET_SB,       /* in a subnegotiation, up to IAC SE */
    TELNET_SB_IAC    /* an IAC in it */
};

#define TN_SE 0xF0
#define TN_SB 0xFA
#define TN_WILL 0xFB
#define TN_WONT 0xFC
#define TN_DO 0xFD
#define TN_DONT 0xFE
#define TN_IAC 0xFF
#define TN_OPT_ECHO 0x01

/*  Takes <b> when it belongs to a TELNET command (1), or leaves it to the
 *  line (0).  An IAC IAC is a 0xff byte of data, and goes to the line.  */
static int telnet_byte(TcpSocket_T *sock, Session_T *sess, uint8_t b) {
    switch (sess->iac_state) {
    case TELNET_DATA:
        if (b != TN_IAC)
            return 0;
        /*  That is how a telnet client is told apart from nc.  */
        sess->telnet = 1;
        sess->iac_state = TELNET_IAC;
        return 1;

    case TELNET_IAC:
        if (b == TN_IAC) {
            sess->iac_state = TELNET_DATA;
            return 0;
        }
        if (b >= TN_WILL) {
            sess->iac_verb = b;
            sess->iac_state = TELNET_OPTION;
        } else {
            sess->iac_state = b == TN_SB ? TELNET_SB : TELNET_DATA;
        }
        return 1;

    case TELNET_OPTION: {
        sess->iac_state = TELNET_DATA;
        /*  DO/DONT ECHO answer the server's own WILL/WONT ECHO; WONT and
         *  DONT agree with what is already the case.  Neither is answered,
         *  or the two sides would go on answering each other.  */
        uint8_t reply = 0;
        if (sess->iac_verb == TN_WILL)
            reply = TN_DONT;
        else if (sess->iac_verb == TN_DO && b != TN_OPT_ECHO)
            reply = TN_WONT;
        if (reply) {
            uint8_t r[3] = {TN_IAC, reply, b};
            write(sock, r, 3);
        }
        return 1;
    }

    case TELNET_SB:
        if (b == TN_IAC)
            sess->iac_state = TELNET_SB_IAC;
        return 1;

    default: /* TELNET_SB_IAC */
        sess->iac_state = b == TN_SE ? TELNET_DATA : TELNET_SB;
        return 1;
    }
}

/*
 *  A line typed while the session is logging in.  Returns 1 when the
 *  connection is to be closed (too many tries).
 */
static int auth_line(TcpSocket_T *sock, Session_T *sess) {
    if (sess->auth == AUTH_LOGIN) {
        memcpy(sess->login, sess->line, sess->llen);
        sess->login_len = sess->llen;
        sess->auth = AUTH_PASSWORD;
        if (sess->telnet)
            write(sock, ECHO_OFF, sizeof(ECHO_OFF));
        say(sock, "password: ");
        return 0;
    }

    int ok = auth_check(sess->login, sess->login_len, sess->line, sess->llen);
    for (uint32_t k = 0; k < LINE_CAP; k++)
        sess->line[k] = 0, sess->login[k] = 0;
    sess->login_len = 0;
    if (sess->telnet) {
        write(sock, ECHO_ON, sizeof(ECHO_ON));
        say(sock, "\r\n"); /* the Enter the client did not echo */
    }

    if (ok) {
        sess->auth = AUTH_DONE;
        sess->tries = 0;
        shell_prompt(sock);
        shell_flush();
        return 0;
    }
    if (++sess->tries >= AUTH_TRIES) {
        say(sock, "Login incorrect. Goodbye.\r\n");
        return 1;
    }
    sess->auth = AUTH_LOGIN;
    say(sock, "Login incorrect.\r\n\r\nlogin: ");
    return 0;
}

uint8_t debug = 0;

int main(int argc, char **argv) {
    const uint8_t *net_arg = (const uint8_t *)"slip";

    /* Process all args from argv[1]; unrecognised tokens (e.g. the binary name
     * the kernel may inject) are silently skipped — only known keywords act. */
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (memcmp((uint8_t *)argv[i], (uint8_t *)"debug", 6) == 0) {
                debug = 1;
                net_set_debug(1);
            } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"--net", 5) == 0 && i + 1 < argc) {
                net_arg = (const uint8_t *)argv[i + 1];
                i++;
            } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"eth", 4) == 0) {
                net_arg = (const uint8_t *)"eth";
            } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"slip", 5) == 0) {
                net_arg = (const uint8_t *)"slip";
            }
        }
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
    xfer_init(net_arg);

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
                    sessions[i].last_llen = 0; /* not the last connection's command */
                    sessions[i].esc_state = 0;
                    sessions[i].tries = 0;
                    sessions[i].telnet = 0;
                    sessions[i].cr = 0;
                    sessions[i].iac_state = TELNET_DATA;
                    sessions[i].login_len = 0;
                    /*  Asked only now, so that the connections already in
                     *  the shell when Memento was logged in to stay there.
                     *  Before anyone gives credentials it is root with no
                     *  password, and the shell straight away.  */
                    sessions[i].auth = auth_required() ? AUTH_LOGIN : AUTH_DONE;

                    shell_session_start(s);
                    shell_banner(s);
                    if (sessions[i].auth == AUTH_LOGIN) {
                        shell_flush();
                        say(s, "Memento is logged in: its login and password, please.\r\n\r\nlogin: ");
                    } else {
                        shell_prompt(s);
                        shell_flush();
                    }
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

            /*  TELNET commands never reach the line (telnet_byte).  */
            if (telnet_byte(client, sess, b))
                continue;

            /*
             *  ANSI/VT100 escape sequence state machine.
             *  Up arrow sends ESC [ A (0x1B 0x5B 0x41).
             *  All other CSI sequences are consumed and ignored.
             */
            if (sess->esc_state == 1) {
                sess->esc_state = (b == '[') ? 2 : 0;
                continue;
            }
            if (sess->esc_state == 2) {
                sess->esc_state = 0;
                if (b == 'A' && sess->last_llen > 0 && sess->auth == AUTH_DONE) {
                    /* Erase what is currently typed, then echo the last command. */
                    for (uint8_t k = 0; k < sess->llen; k++)
                        write(client, (const uint8_t *)"\b \b", 3);
                    memcpy(sess->line, sess->last_line, sess->last_llen);
                    sess->llen = sess->last_llen;
                    write(client, sess->line, sess->llen);
                }
                continue; /* B=down C=right D=left — all ignored */
            }
            if (b == 0x1B) { /* ESC */
                sess->esc_state = 1;
                continue;
            }

            /*
             *  The end of a line: CR LF from a telnet client in line mode,
             *  CR NUL from one sending a character at a time (which it does
             *  once the server echoes, as for the password, having asked for
             *  SUPPRESS-GO-AHEAD itself), a bare LF from nc.  The line ends
             *  on the CR, and the LF or NUL after it is let go by.
             */
            if (b == '\r' || b == '\n' || b == 0) {
                uint8_t after_cr = sess->cr;
                sess->cr = (b == '\r');
                if (b == 0 || (b == '\n' && after_cr))
                    continue;
            } else {
                sess->cr = 0;
            }

            if (b == '\r' || b == '\n') {
                sess->esc_state = 0;
                if (sess->auth != AUTH_DONE) {
                    int quit = auth_line(client, sess);
                    sess->llen = 0;
                    if (quit) {
                        close(client);
                        sessions[client->id].active = 0;
                        break;
                    }
                    continue;
                }
                /* Save non-empty command to history before dispatching. */
                if (sess->llen > 0) {
                    memcpy(sess->last_line, sess->line, sess->llen);
                    sess->last_llen = sess->llen;
                }
                shell_set_telnet(sess->telnet);
                int quit = shell_dispatch(client, sockets, sess->line, sess->llen);
                sess->llen = 0;
                if (quit) {
                    shell_flush();
                    close(client);
                    sessions[client->id].active = 0;
                    break;
                }
                shell_prompt(client);
                shell_flush();
                continue;
            }

            if (b == 0x7f || b == '\b') { /* DEL or BS */
                sess->esc_state = 0;
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
