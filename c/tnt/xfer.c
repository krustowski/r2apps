#include "bytes.h"
#include "mem.h"
#include "net.h"
#include "string.h"
#include "syscall.h"

#include "xfer.h"

/*
 *  The sending side of `get`: see xfer.h for why it does not simply write()
 *  the file into a socket.
 *
 *  Offsets below are into the stream: the HTTP header (if any), then the
 *  file.  The peer's ACK minus the sequence number the stream started at is
 *  how much of it has arrived; everything past that is sent again when a
 *  retransmission timer runs out or three duplicate ACKs say a segment was
 *  lost (go-back-N).  The window is what the peer advertises, capped by a
 *  small congestion window that starts at two segments, grows by one per ACK
 *  and falls back after a loss, so a NIC that drops bursts is not flooded.
 */

#define SEG 1024 /* what shell output already goes out in, known to fit */
#define CWND_MAX 16
#define CACHE_CAP 8192

#define RTO_MIN 250
#define RTO_MAX 2000
#define WAIT_CLIENT_MS 60000
#define SNIFF_MS 500      /* how long a silent client has to start an HTTP request */
#define REQUEST_MS 3000   /* how long an HTTP request has to be complete */
#define STALL_MS 15000    /* no progress for this long ends the transfer */
#define FIN_TRIES 5

static uint8_t is_eth = 0;

void xfer_init(const uint8_t *net_name) { is_eth = net_name && net_name[0] == 'e'; }

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

static uint8_t lower(uint8_t c) { return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + 32) : c; }

int64_t xfer_file_size(const uint8_t *path) {
    uint8_t probe;

    /* A directory, or nothing at all, is an error here; an empty file reads 0. */
    if (read_file_at(path, &probe, 0, 1) < 0)
        return -1;

    uint8_t parent[64];
    uint32_t last = 0;
    uint32_t i;
    for (i = 0; path[i] && i < 63; i++) {
        parent[i] = path[i];
        if (path[i] == '/')
            last = i;
    }
    const uint8_t *name = path + last + 1;
    uint32_t name_len = strlen(name);

    parent[last ? last : 1] = '\0'; /* "/x" lists "/" */

    static VfsDirEntry_T entries[64];
    int64_t count = list_dir_path(parent, entries);
    if (count < 0 || count > 64)
        return XFER_SIZE_UNKNOWN;

    for (int64_t e = 0; e < count; e++) {
        if (entries[e].is_dir || entries[e].name_len != name_len)
            continue;
        uint32_t k = 0;
        while (k < name_len && lower(entries[e].name[k]) == lower(name[k]))
            k++;
        if (k == name_len)
            return entries[e].size;
    }

    return XFER_SIZE_UNKNOWN;
}

/*
 *  The stream: header bytes, then the file through a cache of one aligned
 *  block, so the resends after a loss (always within the last few KiB) do not
 *  go back to the disk.
 */
static const uint8_t *src_path;
static uint8_t head[256];
static uint32_t head_len;
static uint8_t cache[CACHE_CAP];
static uint32_t cache_off, cache_len;
static uint8_t cache_valid, read_error;

/* Copies up to <len> bytes of the stream at <off>; short only at the end. */
static uint32_t stream_copy(uint8_t *dst, uint32_t off, uint32_t len) {
    uint32_t n = 0;

    while (n < len && off < head_len)
        dst[n++] = head[off++];

    while (n < len) {
        uint32_t f = off - head_len;

        if (!cache_valid || f < cache_off || f >= cache_off + cache_len) {
            cache_off = f - f % CACHE_CAP;
            int64_t r = read_file_at(src_path, cache, cache_off, CACHE_CAP);
            if (r < 0) {
                read_error = 1;
                r = 0;
            }
            cache_len = (uint32_t)r;
            cache_valid = 1;
            if (f >= cache_off + cache_len)
                break; /* end of file */
        }

        uint32_t k = cache_off + cache_len - f;
        if (k > len - n)
            k = len - n;
        memcpy(dst + n, cache + (f - cache_off), (uint16_t)k);
        n += k;
        off += k;
    }

    return n;
}

/*
 *  What the peer of the data connection has told us, gathered by pump() from
 *  every segment it sends.  Only its ACK numbers matter to the sender, and
 *  on_tcp_packet() keeps none of them, so they are taken here on the way past.
 */
static struct {
    uint8_t on; /* a connection is being watched */
    TcpSocket_T *conn;
    uint8_t ip[4];
    uint16_t rport, lport;
    uint8_t sending;  /* base is set: acks count */
    uint32_t base;    /* sequence number of stream offset 0 */
    uint32_t acked;   /* stream bytes the peer has */
    uint32_t hi;      /* highest stream offset sent (+1 for the FIN) */
    uint16_t window;
    uint8_t dup;      /* duplicate ACKs in a row */
    uint8_t progress; /* acked moved since the sender last looked */
    uint8_t reset;
    uint8_t fin; /* the peer is done sending */
} peer;

/* Takes one packet, if there is one, through the TCP layer.  1 if it did. */
static int pump(TcpSocket_T sockets[MAX_SOCKETS]) {
    static uint8_t pkt[2048];
    static uint8_t seg[1500];

    int n = net_drv.recv(pkt, sizeof(pkt));
    if (n <= 0)
        return 0;

    Ipv4Header_T ip;
    TcpHeader_T th;
    uint16_t hlen = parse_ipv4_packet(pkt, &ip);
    if (!hlen || ip.protocol != 6 || (uint32_t)n <= hlen || (uint32_t)n - hlen > sizeof(seg))
        return 1;

    uint32_t seg_len = (uint32_t)n - hlen;
    memcpy(seg, pkt + hlen, (uint16_t)seg_len);
    parse_tcp_packet(seg, &th);

    uint8_t own_fin = 0;
    uint32_t data_len = 0;

    if (peer.on && th.dest_port == peer.lport && th.source_port == peer.rport && memcmp(ip.source_addr, peer.ip, 4) == 0) {
        uint8_t flags = th.data_offset_reserved_flags & 0xFF;
        uint32_t thl = ((th.data_offset_reserved_flags >> 12) & 0xF) * 4;
        data_len = seg_len > thl ? seg_len - thl : 0;

        /* A FIN here means the peer has nothing more to say -- nc sends one
         * as soon as its stdin ends -- not that it stopped listening.  The TCP
         * layer would drop a socket with nothing unread on it, so it does not
         * get to see this FIN; it is answered below instead. */
        if ((flags & TCP_FLAG_FIN) && (peer.conn->state == SOCKET_ESTABLISHED || peer.conn->state == SOCKET_CLOSE_WAIT)) {
            own_fin = 1;
            th.data_offset_reserved_flags &= (uint16_t)~TCP_FLAG_FIN;
        }

        if (flags & TCP_FLAG_RST)
            peer.reset = 1;

        if (flags & TCP_FLAG_ACK) {
            peer.window = swap16(th.window_size);

            if (peer.sending) {
                int32_t d = (int32_t)(th.ack_num - peer.base);

                if (d > (int32_t)peer.acked && d <= (int32_t)peer.hi) {
                    peer.acked = (uint32_t)d;
                    peer.dup = 0;
                    peer.progress = 1;
                } else if (d == (int32_t)peer.acked && !data_len && !(flags & (TCP_FLAG_SYN | TCP_FLAG_FIN))) {
                    if (peer.dup < 255)
                        peer.dup++;
                }
            }
        }
    }

    on_tcp_packet(ip.source_addr, ip.destination_addr, &th, seg, seg_len, sockets);

    if (own_fin && peer.conn->used) {
        TcpSocket_T *c = peer.conn;

        if (th.seq_num + data_len == c->ack_num) {
            /* Everything before the FIN is in: take the FIN too. */
            c->ack_num++;
            c->state = SOCKET_CLOSE_WAIT;
            peer.fin = 1;
            send_tcp_packet(c, 0, 0, TCP_FLAG_ACK);
        } else if (th.seq_num + data_len + 1 == c->ack_num) {
            send_tcp_packet(c, 0, 0, TCP_FLAG_ACK); /* our ACK of it was lost */
        }
    }

    return 1;
}

/* Nothing arrived and nothing could be sent: let the rest of the system run.
 * Not under SLIP, whose reader takes one byte per call and would crawl. */
static void idle(void) {
    if (is_eth)
        sleep_ms(1);
}

static int peer_gone(TcpSocket_T *conn) {
    return peer.reset || !conn->used || conn->remote_port != peer.rport || memcmp(conn->remote_ip, peer.ip, 4) != 0;
}

/* A key typed into the telnet session. */
static int cancelled(TcpSocket_T *session) {
    if (!session->used || !session->rx_len)
        return 0;
    session->rx_len = 0;
    return 1;
}

static uint32_t append(uint8_t *dst, uint32_t n, uint32_t cap, const uint8_t *s) {
    while (*s && n < cap)
        dst[n++] = *s++;
    return n;
}

static void build_http_head(const uint8_t *name, int64_t size) {
    uint8_t num[12];
    uint32_t n = 0;
    uint32_t cap = sizeof(head);

    n = append(head, n, cap, (const uint8_t *)"HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n");
    if (size >= 0) {
        u32_to_str((uint32_t)size, num);
        n = append(head, n, cap, (const uint8_t *)"Content-Length: ");
        n = append(head, n, cap, num);
        n = append(head, n, cap, (const uint8_t *)"\r\n");
    }
    n = append(head, n, cap, (const uint8_t *)"Content-Disposition: attachment; filename=\"");
    for (uint32_t i = 0; name[i] && i < 64 && n < cap; i++)
        if (name[i] != '"' && name[i] >= 0x20)
            head[n++] = name[i];
    n = append(head, n, cap, (const uint8_t *)"\"\r\nConnection: close\r\n\r\n");

    head_len = n;
}

/* Is there a complete HTTP request head in the socket? */
static int request_complete(const TcpSocket_T *s) {
    for (uint32_t i = 3; i < s->rx_len; i++)
        if (s->rx_buffer[i - 3] == '\r' && s->rx_buffer[i - 2] == '\n' && s->rx_buffer[i - 1] == '\r' && s->rx_buffer[i] == '\n')
            return 1;
    return s->rx_len == RX_BUFFER_SIZE;
}

static void abort_conn(TcpSocket_T *conn) {
    if (!conn->used)
        return;
    send_tcp_packet(conn, 0, 0, TCP_FLAG_RST | TCP_FLAG_ACK);
    free_socket(conn);
}

/* The transfer proper, from the first byte to the acknowledged FIN. */
static XferStatus_T send_stream(TcpSocket_T sockets[MAX_SOCKETS], TcpSocket_T *session, TcpSocket_T *conn, int64_t size, XferResult_T *res) {
    static uint8_t buf[SEG];

    uint8_t total_known = size >= 0;
    uint32_t total = total_known ? head_len + (uint32_t)size : 0;
    uint32_t next = 0;
    uint32_t crc_off = 0; /* file bytes folded into the CRC so far */
    uint32_t cwnd = 2;
    uint32_t rto = RTO_MIN;
    uint8_t recovering = 0;
    uint64_t now = get_ticks();
    uint64_t mark = now;          /* when the retransmission timer started */
    uint64_t last_progress = now;

    peer.base = conn->seq_num;
    peer.acked = 0;
    peer.hi = 0;
    peer.dup = 0;
    peer.progress = 0;
    peer.sending = 1;

    for (;;) {
        int got = pump(sockets);
        now = get_ticks();

        if (peer_gone(conn))
            return XFER_RESET;
        if (cancelled(session)) {
            abort_conn(conn);
            return XFER_CANCELLED;
        }

        if (peer.progress) {
            peer.progress = 0;
            recovering = 0;
            rto = RTO_MIN;
            mark = now;
            last_progress = now;
            if (cwnd < CWND_MAX)
                cwnd++;
            if (next < peer.acked) /* acked past a go-back point */
                next = peer.acked;
        }

        if (total_known && peer.acked >= total)
            break;

        if (now - last_progress > STALL_MS) {
            abort_conn(conn);
            return XFER_STALLED;
        }

        if (next > peer.acked && now - mark >= rto) {
            /* Timeout: everything past what arrived is sent again. */
            next = peer.acked;
            cwnd = 1;
            rto = rto * 2 > RTO_MAX ? RTO_MAX : rto * 2;
            mark = now;
            recovering = 1;
        } else if (peer.dup >= 3 && !recovering) {
            /* Fast retransmit.  The resends draw duplicate ACKs of their own
             * from a peer that already had some of them; those must not set
             * off another round, hence `recovering` until acked moves. */
            next = peer.acked;
            cwnd = cwnd / 2 ? cwnd / 2 : 1;
            mark = now;
            recovering = 1;
            peer.dup = 0;
        }

        uint32_t limit = cwnd * SEG;
        if (peer.window < limit)
            limit = peer.window;

        int sent = 0;
        while (!total_known || next < total) {
            uint32_t inflight = next - peer.acked;
            uint32_t want = SEG;
            if (total_known && total - next < want)
                want = total - next;

            /* One segment may always go when none is out: the probe that
             * gets a zero window going again. */
            if (inflight && inflight + want > limit)
                break;

            uint32_t len = stream_copy(buf, next, want);
            if (read_error) {
                abort_conn(conn);
                return XFER_READ_ERROR;
            }
            if (len < want) {
                total = next + len;
                total_known = 1;
            }
            if (!len)
                break;

            if (next + len > head_len && next + len - head_len > crc_off) {
                uint32_t skip = crc_off + head_len - next; /* stream bytes already folded in */
                if (next > crc_off + head_len)
                    skip = 0;
                res->crc32 = crc32_update(res->crc32, buf + skip, len - skip);
                crc_off = next + len - head_len;
            }

            if (next < peer.hi)
                res->resent++;
            if (!inflight)
                mark = now;

            conn->seq_num = peer.base + next;
            write(conn, buf, len);
            next += len;
            if (next > peer.hi)
                peer.hi = next;
            sent = 1;
        }

        if (total_known && next >= total && peer.acked >= total)
            break;

        if (!got && !sent)
            idle();
    }

    res->bytes = total - head_len;

    /* Close, and send the FIN again until it is acknowledged: nc only exits
     * once it has it. */
    close(conn);
    peer.hi = total + 1;
    mark = get_ticks();
    rto = RTO_MIN;
    for (int tries = 0; tries < FIN_TRIES;) {
        int got = pump(sockets);
        if (peer_gone(conn))
            break;
        if (peer.acked == total + 1) {
            /* Both halves closed: nothing is left for the TCP layer to wait
             * for, and it would otherwise keep the slot in FIN_WAIT. */
            if (peer.fin)
                free_socket(conn);
            break;
        }
        if (get_ticks() - mark >= rto) {
            conn->seq_num = peer.base + total;
            send_tcp_packet(conn, 0, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
            mark = get_ticks();
            rto *= 2;
            tries++;
        } else if (!got) {
            idle();
        }
    }

    return XFER_OK;
}

XferStatus_T xfer_send(TcpSocket_T sockets[MAX_SOCKETS], TcpSocket_T *session, const uint8_t *path, const uint8_t *name, int64_t size, uint16_t port, XferResult_T *res) {
    res->bytes = 0;
    res->crc32 = 0;
    res->resent = 0;
    res->ms = 0;
    res->http = 0;

    src_path = path;
    head_len = 0;
    cache_valid = 0;
    read_error = 0;
    peer.on = 0;
    peer.sending = 0;
    peer.reset = 0;
    peer.fin = 0;

    TcpSocket_T *listener = socket_tcp4(sockets);
    if (!listener)
        return XFER_NO_SOCKET;
    bind(listener, port);
    listen(listener);

    /* Under ETH the kernel hands this process only the ports it has bound. */
    if (is_eth)
        net_bind_port(port);
    net_set_nonblocking(1);

    XferStatus_T status = XFER_OK;
    TcpSocket_T *conn = 0;
    uint64_t start = get_ticks();

    /* Wait for a client. */
    while (!conn) {
        int got = pump(sockets);

        for (int i = 0; i < MAX_SOCKETS; i++) {
            TcpSocket_T *s = &sockets[i];
            if (s->used && s != listener && s->local_port == port && s->state == SOCKET_ESTABLISHED) {
                conn = s;
                break;
            }
        }
        if (conn)
            break;

        if (cancelled(session)) {
            status = XFER_CANCELLED;
            goto out;
        }
        if (get_ticks() - start > WAIT_CLIENT_MS) {
            status = XFER_NO_CLIENT;
            goto out;
        }
        if (!got)
            idle();
    }

    peer.on = 1;
    peer.conn = conn;
    memcpy(peer.ip, conn->remote_ip, 4);
    peer.rport = conn->remote_port;
    peer.lport = port;
    peer.window = SEG;
    memcpy(res->peer_ip, conn->remote_ip, 4);

    /* HTTP or raw?  An HTTP client speaks first; nc never does. */
    uint64_t t0 = get_ticks();
    for (;;) {
        int got = pump(sockets);
        uint64_t waited = get_ticks() - t0;

        if (peer_gone(conn)) {
            status = XFER_RESET;
            goto out;
        }
        if (cancelled(session)) {
            abort_conn(conn);
            status = XFER_CANCELLED;
            goto out;
        }
        if (request_complete(conn) || (!conn->rx_len && waited >= SNIFF_MS) || waited >= REQUEST_MS)
            break;
        if (!got)
            idle();
    }

    if (conn->rx_len >= 4 && memcmp(conn->rx_buffer, (const uint8_t *)"GET ", 4) == 0) {
        res->http = 1;
        build_http_head(name, size);
    }
    conn->rx_len = 0; /* the request itself is of no further interest */

    status = send_stream(sockets, session, conn, size, res);
    res->ms = get_ticks() - t0;

out:
    peer.on = 0;

    /* Anyone else who connected meanwhile is turned away. */
    for (int i = 0; i < MAX_SOCKETS; i++) {
        TcpSocket_T *s = &sockets[i];
        if (s->used && s != listener && s != conn && s->local_port == port && s->state != SOCKET_FIN_WAIT)
            abort_conn(s);
    }
    free_socket(listener);

    net_set_nonblocking(0);
    return status;
}
