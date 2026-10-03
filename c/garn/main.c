#include "mem.h"
#include "net.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

#include "config.h"
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

uint8_t debug = 0;

/* Main-loop pacing.  The loop no longer parks inside recv(), so it has to give
 * the CPU back itself.  10 ms is fine enough for a one-second event tick while
 * keeping the process off the run queue in between. */
#define POLL_INTERVAL_MS 10

/* Frames drained per iteration before the timed work below gets its turn, so a
 * burst of traffic cannot starve event pushes and socket reaping. */
#define RECV_BUDGET 16

/* How often abandoned sockets are looked for, and how quiet a connection has
 * to be to count as abandoned.  Browsers open speculative connections they
 * never send a request on; those have to be collected or they pin a slot
 * each until the pool is empty. */
#define GC_INTERVAL_SECS 2
#define IDLE_TIMEOUT_SECS 15

/*
 *  request_complete()
 *
 *  True once a blank line has terminated the request head.  Readiness fires on
 *  the first byte to arrive and a request can span several segments, so
 *  parsing on readiness alone would hand route dispatch half a request line.
 */
static uint8_t request_complete(const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n')
            return 1;

        if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return 1;
    }

    return 0;
}

/*
 *  handle_client()
 *
 *  Serves one complete request.  Returns 1 when the connection has become an
 *  event stream and the caller must keep it open, 0 when it has been closed.
 */
static uint8_t handle_client(TcpSocket_T *client, uint8_t *req_buf, uint32_t req_cap, const GarnConfig_T *cfg) {
    uint32_t n = read(client, req_buf, req_cap - 1);

    if (!n) {
        close(client);
        return 0;
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

    /* Drop non-request segments: valid HTTP methods are non-empty uppercase
     * ASCII letters only (GET, POST, ...).  Header names (Host:, X-Forwarded-For:)
     * contain hyphens, digits or lowercase --- all rejected by this check. */
    {
        uint8_t valid = (method[0] != '\0');
        for (uint8_t i = 0; valid && method[i]; i++)
            if (method[i] < 'A' || method[i] > 'Z')
                valid = 0;

        if (!valid) {
            /* Not a request line.  Close rather than leave the socket sitting
             * allocated until the reaper reaches it. */
            close(client);
            return 0;
        }
    }

    if (memcmp(method, (const uint8_t *)"GET", 4) != 0) {
        const uint8_t b[] = "Method Not Allowed";
        respond(client, 405, (const uint8_t *)"Method Not Allowed", (const uint8_t *)"text/plain", b, strlen(b));

    } else if (rpath[0] == '/' && rpath[1] == '\0') {
        route_file(client, (const uint8_t *)"INDEX.HTM", cfg->path);

    } else if (memcmp(rpath, (const uint8_t *)"/info", 6) == 0) {
        route_info(client);

    } else if (memcmp(rpath, (const uint8_t *)"/events", 8) == 0) {
        route_events(client);

        return 1; /* caller records it as the event stream and keeps it open */

    } else if (rpath[0] == '/' && rpath[1] != '\0') {
        route_file(client, rpath + 1, cfg->path);

    } else {
        const uint8_t b[] = "Not Found";
        respond(client, 404, (const uint8_t *)"Not Found", (const uint8_t *)"text/plain", b, strlen(b));
    }

    close(client);

    return 0;
}

int main(int argc, char **argv) {
    GarnConfig_T cfg;
    config_defaults(&cfg);

    /* Capture the kernel cwd at launch so we can find garn.cfg even if
     * the shell changes directory after starting us.
     * run_elf() has no argv mechanism, so --config can't be passed from the
     * shell — the cwd at launch is the only per-instance discriminator.
     * Workflow: cd /mnt/fat/GARN1 && run garn  →  reads /mnt/fat/GARN1/garn.cfg
     *           cd /mnt/fat/GARN2 && run garn  →  reads /mnt/fat/GARN2/garn.cfg */
    SysInfo_T si;
    si.system_path[0]  = '\0';
    si.system_path[31] = '\0';
    read_sysinfo(&si);
    si.system_path[31] = '\0';

    uint8_t default_config[64];
    {
        uint32_t n = strlen(si.system_path);
        memcpy(default_config, si.system_path, n);
        if (n > 0 && default_config[n - 1] != '/') default_config[n++] = '/';
        memcpy(default_config + n, "GARN.CFG\0", 9);
    }

    /* Scan all args for --config; unrecognized tokens (e.g. binary name) are
     * silently skipped so this works regardless of argv[0] vs argv[1] convention. */
    const uint8_t *config_path = default_config;
    for (int i = 1; i < argc - 1; i++) {
        if (memcmp((uint8_t *)argv[i], (uint8_t *)"--config", 9) == 0) {
            config_path = (const uint8_t *)argv[i + 1];
            break;
        }
    }
    config_load(config_path, &cfg);

    /* Apply CLI overrides on top of config; print only recognised args so the
     * binary name (which the kernel passes as an argv token) stays invisible. */
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (memcmp((uint8_t *)argv[i], (uint8_t *)"debug", 6) == 0) {
                cfg.debug = 1;
            } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"--net", 5) == 0 && i + 1 < argc) {
                i++;
                uint32_t nlen = strlen((const uint8_t *)argv[i]);
                if (nlen > 7) nlen = 7;
                memcpy(cfg.net, (uint8_t *)argv[i], nlen);
                cfg.net[nlen] = '\0';
            } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"eth", 4) == 0) {
                memcpy(cfg.net, (const uint8_t *)"eth\0", 4);
            } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"slip", 5) == 0) {
                memcpy(cfg.net, (const uint8_t *)"slip\0", 5);
            }
            /* unrecognised tokens (binary name, --config + its value) silently skipped */
        }
    }

    debug = cfg.debug;

    /* Forward it to the TCP layer so its per-packet trace follows the same
     * switch rather than printing for every ACK regardless. */
    net_set_debug(cfg.debug);

    /* Publish static IP to sysinfo so ETH driver picks it up instead of DHCP.
     * ETH reads sysinfo at startup: if ip_addr is non-zero it skips DHCP. */
    if (cfg.ip[0] || cfg.ip[1] || cfg.ip[2] || cfg.ip[3]) {
        SysInfo_T ip_si;
        ip_si.system_path[0] = ip_si.system_path[31] = '\0';
        read_sysinfo(&ip_si);
        ip_si.system_path[31] = '\0';
        memcpy(ip_si.ip_addr, cfg.ip, 4);
        write_sysinfo(&ip_si);
    }

    uint8_t packet_buf[2048];
    uint8_t tcp_packet[1500];
    uint8_t req_buf[1024];
    static TcpSocket_T sockets[MAX_SOCKETS];

    Ipv4Header_T ipv4_header;
    uint16_t ipv4_header_len = 0;

    TcpHeader_T tcp_header;

    /* _crt0.asm reserves BSS but never clears it, so the pool is only zeroed
     * if the kernel's ELF loader clears the memsz-over-filesz gap.  Do it
     * explicitly and the socket states are well-defined either way. */
    socket_pool_init(sockets);

    TcpSocket_T *server = socket_tcp4(sockets);
    bind(server, cfg.port);

    listen(server);

    TcpSocket_T *sse_client = 0;
    uint16_t sse_remote_port = 0; /* discriminator: ephemeral port changes on reconnect */
    uint8_t sse_last_sec = 0xff;  /* last RTC second we processed; 0xff = none yet */
    uint32_t now = 0;             /* uptime seconds, +1 so 0 keeps meaning "no clock" */
    uint32_t last_gc = 0;

    printf((const uint8_t *)"-> garn HTTP/1.0 service starting on port %u\n", (uint32_t)cfg.port);

    if (net_driver_bind_port(cfg.net, cfg.port) < 0) {
        print((const uint8_t *)"-> net driver init failed\n");
        return 1;
    }
    printf((const uint8_t *)"-> net driver: %s\n", cfg.net);

    /* The blocking receive suspends this process in the kernel until a frame
     * arrives, which would make every other stage of the loop below run only
     * as often as packets happen to turn up: no event-stream heartbeat on an
     * idle connection, and no socket reaping on a quiet link.  Take the
     * non-blocking reader and pace the loop with sleep_ms() instead. */
    net_set_nonblocking(1);

    /* Publish the driver's local IP to sysinfo so tools like the Memento
     * NetWindow can display it --- but only when nobody has: the eth driver
     * owns that address (DHCP may hand it one after we started), and
     * overwriting it with our built-in default would take the lease away
     * from every service, us included. */
    {
        SysInfo_T pub_si;
        if (read_sysinfo(&pub_si) &&
            !(pub_si.ip_addr[0] | pub_si.ip_addr[1] | pub_si.ip_addr[2] | pub_si.ip_addr[3])) {
            net_get_local_ip(pub_si.ip_addr);
            write_sysinfo(&pub_si);
        }
    }

    for (;;) {
        /* Publish the clock to the TCP layer, which stamps socket activity
         * from it.  +1 keeps the value non-zero at boot, since net_set_time(0)
         * is what tells socket_reap() there is no clock to judge idleness by. */
        {
            SysInfo_T tick_si;
            if (read_sysinfo(&tick_si))
                now = tick_si.system_uptime + 1;
        }
        net_set_time(now);

        /* 1. Drain whatever the driver has queued, up to a budget. */
        for (uint8_t budget = 0; budget < RECV_BUDGET; budget++) {
            int64_t decoded_len = net_drv.recv(packet_buf, sizeof(packet_buf));

            if (decoded_len <= 0)
                break;

            ipv4_header_len = parse_ipv4_packet(packet_buf, &ipv4_header);
            if (!ipv4_header_len || ipv4_header.protocol != 6)
                continue;

            uint32_t seg_len = (uint32_t)decoded_len - ipv4_header_len;
            if (seg_len > sizeof(tcp_packet))
                continue;

            memcpy(tcp_packet, packet_buf + ipv4_header_len, seg_len);
            parse_tcp_packet(tcp_packet, &tcp_header);

            if (debug) {
                printf((const uint8_t *)">> TCP src=%u dst=%u seq=%u\n", tcp_header.source_port, tcp_header.dest_port, tcp_header.seq_num);
            }

            on_tcp_packet(ipv4_header.source_addr, ipv4_header.destination_addr, &tcp_header, tcp_packet, seg_len, sockets);
        }

        /* 2. Serve every socket holding a complete request.  A browser opens
         *    several connections in parallel, so servicing only one per
         *    received frame leaves the rest waiting on unrelated traffic.
         *    accept() reports the first readable socket and cannot express
         *    "this request is not finished yet", so scan the set directly. */
        SocketSet_T ready = socket_select(sockets, SEL_READ);

        for (uint8_t i = 0; i < MAX_SOCKETS && ready; i++) {
            if (!(ready & (SocketSet_T)(1u << i)))
                continue;

            ready &= (SocketSet_T)~(1u << i);

            TcpSocket_T *client = &sockets[i];

            if (client == server || client->local_port != cfg.port)
                continue;

            /* Anything arriving on the event stream is a header continuation
             * or a stray segment, never a new request.  Guard with the
             * ephemeral port: if the slot was freed and handed to a different
             * connection, that one must be served normally. */
            if (client == sse_client && client->remote_port == sse_remote_port) {
                client->rx_len = 0;
                continue;
            }

            /* Wait for the whole request head, unless the peer has half-closed
             * (nothing more is coming) or the buffer is full (nothing more
             * would fit). */
            if (!request_complete(client->rx_buffer, client->rx_len) && client->state != SOCKET_CLOSE_WAIT && client->rx_len < RX_BUFFER_SIZE)
                continue;

            if (handle_client(client, req_buf, sizeof(req_buf), &cfg)) {
                /* Only one stream at a time: retire the previous one. */
                if (sse_client && sse_client != client && sse_client->used && sse_client->remote_port == sse_remote_port)
                    close(sse_client);

                sse_client = client;
                sse_remote_port = client->remote_port;
                sse_last_sec = 0xff;
            }
        }

        /* 3. Push to the event stream on wall time.  This is the stage that
         *    the blocking receive used to strand: with no packets arriving on
         *    an idle stream, it never ran and the client saw nothing until it
         *    gave up. */
        if (sse_client) {
            if (!sse_client->used || sse_client->state != SOCKET_ESTABLISHED || sse_client->remote_port != sse_remote_port) {
                sse_client = 0;
            } else {
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
                    } else {
                        static const uint8_t hb[] = ": \n";
                        write(sse_client, hb, sizeof(hb) - 1);
                    }
                }
            }
        }

        /* 4. Collect abandoned sockets, on wall time rather than per packet.
         *    The event stream is exempt: it is deliberately quiet inbound. */
        if (now - last_gc >= GC_INTERVAL_SECS) {
            last_gc = now;

            SocketSet_T protect = 0;
            if (sse_client && sse_client->used)
                protect |= (SocketSet_T)(1u << sse_client->id);

            socket_reap(sockets, IDLE_TIMEOUT_SECS, protect);
        }

        sleep_ms(POLL_INTERVAL_MS);
    }

    return 0;
}
