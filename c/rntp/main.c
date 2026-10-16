/* RNTP shared RAM export and client, using libcr2 TCP. */
#include "net.h"
void printf(const uint8_t *format, ...);
#include "bridge.h"
#include "protocol.h"
#include "sessions.h"
#include "read_ahead.h"

#define EXCHANGE_TIMEOUT 5000ULL
#define RETRANSMIT_MS 250ULL
#define HEARTBEAT_MS 2000ULL
#define LEASE_TIMEOUT 10000ULL

typedef struct {
    uint8_t rx[RNTP_FRAME], tx[RNTP_FRAME], tcp_rx[32768];
    int receive_installed;
    uint32_t rx_len, tx_len, tx_pos, flight;
    uint32_t flight_seq;
    uint64_t sent_at, activity;
    int close_after;
} Stream;

static TcpSocket_T sockets[MAX_SOCKETS];
static Stream streams[MAX_SOCKETS];
static uint8_t packet[2048];
static RntpSessions sessions;
static int serving;

static void zero(void *ptr, uint64_t length) {
    uint8_t *p = ptr;
    for (uint64_t i = 0; i < length; ++i) p[i] = 0;
}
static void copy(void *dst, const void *src, uint32_t length) {
    uint8_t *out = dst;
    const uint8_t *in = src;
    for (uint32_t i = 0; i < length; ++i) out[i] = in[i];
}
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static int number(const char *s, uint32_t *value) {
    uint32_t n = 0;
    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9' || n > 6553) return 0;
        n = n * 10 + (uint32_t)(*s++ - '0');
        if (n > 65535) return 0;
    }
    *value = n;
    return 1;
}
static int address(const char *s, uint8_t out[4]) {
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t n = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (uint32_t)(*s++ - '0');
            if (++digits > 3 || n > 255) return 0;
        }
        if (!digits || (i < 3 ? *s++ != '.' : *s != 0)) return 0;
        out[i] = (uint8_t)n;
    }
    return out[0] > 0 && out[0] < 224;
}

static void reset(TcpSocket_T *s) {
    if (serving) rntp_sessions_release(&sessions, s->id);
    if (s->used && s->state == SOCKET_ESTABLISHED)
        send_tcp_packet(s, 0, 0, TCP_FLAG_RST | TCP_FLAG_ACK);
    free_socket(s);
    zero(&streams[s->id], sizeof(Stream));
}

/* Cumulative ACKs advance a bounded send window. Lost segments replay the
 * outstanding sequence range; the application never resubmits a block write. */
static void transmit(TcpSocket_T *s, Stream *st, uint64_t now) {
    uint32_t mss = tcp_peer_mss(s);
    if (mss > TCP_MAX_PAYLOAD) mss = TCP_MAX_PAYLOAD;
    if (st->flight) {
        uint32_t acked = s->tx_acked - st->flight_seq;
        if (acked <= st->flight && acked) {
            st->tx_pos += acked;
            st->flight_seq += acked;
            st->flight -= acked;
            st->sent_at = now;
        }
        if (st->flight && now - st->sent_at >= RETRANSMIT_MS) {
            uint32_t next = s->seq_num;
            s->seq_num = st->flight_seq;
            for (uint32_t pos = 0; pos < st->flight;) {
                uint32_t amount = st->flight - pos;
                if (amount > mss) amount = mss;
                send_tcp_packet(s, st->tx + st->tx_pos + pos, amount, TCP_FLAG_ACK);
                pos += amount;
            }
            s->seq_num = next;
            st->sent_at = now;
        }
    }
    // Pace bursts to leave room in the kernel's packet queues and let the
    // other daemon drain them. No acknowledgment wait between these segments.
    for (uint32_t burst = 0; burst < 8 && st->tx_pos + st->flight < st->tx_len; ++burst) {
        uint32_t window = s->peer_window;
        if (st->flight >= window) break;
        uint32_t amount = st->tx_len - st->tx_pos - st->flight;
        if (amount > mss) amount = mss;
        if (amount > window - st->flight) amount = window - st->flight;
        if (!st->flight) { st->flight_seq = s->seq_num; st->sent_at = now; }
        uint32_t sent = tcp_write(s, st->tx + st->tx_pos + st->flight, amount);
        if (!sent) break;
        st->flight += sent;
    }
    if (!st->flight && st->tx_pos < st->tx_len && !s->peer_window && now - st->sent_at >= RETRANSMIT_MS) {
        send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
        st->sent_at = now;
    }
    if (st->tx_len && st->tx_pos == st->tx_len && !st->flight) {
        st->tx_len = st->tx_pos = 0;
        if (st->close_after) reset(s);
    }
}

static void outgoing(Stream *st, uint32_t length, uint64_t now) {
    st->tx_len = length; st->tx_pos = st->flight = 0;
    st->activity = now;
}

/* Consume exactly one framed message. Keep both split headers and any bytes
 * belonging to the following message; tcp_read() discards a short-read tail. */
static int incoming(TcpSocket_T *s, Stream *st) {
    for (;;) {
        uint32_t need = RNTP_HEADER;
        RntpHeader header;
        if (st->rx_len >= RNTP_HEADER) {
            if (!rntp_decode(st->rx, &header)) return -1;
            need += header.length;
        }
        if (st->rx_len == need) return 1;
        if (!s->rx_len) return 0;
        uint32_t take = need - st->rx_len;
        if (take > s->rx_len) take = s->rx_len;
        uint8_t *received = tcp_receive_data(s);
        copy(st->rx + st->rx_len, received, take);
        st->rx_len += take;
        st->activity = get_ticks();
        for (uint32_t i = take; i < s->rx_len; ++i)
            received[i - take] = received[i];
        s->rx_len -= take;
        send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
    }
}

static void network(uint64_t now) {
    net_set_time((uint32_t)(now / 1000 + 1));
    for (uint32_t n = 0; n < 32; ++n) {
        int length = net_recv_nb(packet, sizeof(packet));
        if (length <= 0) break;
        uint32_t ihl = (packet[0] & 15U) * 4;
        if (length < 40 || ihl < 20 || ihl + 20 > (uint32_t)length || packet[9] != 6) continue;
        uint32_t total = ((uint32_t)packet[2] << 8) | packet[3];
        if (total > (uint32_t)length || total < ihl + 20) continue;
        TcpHeader_T header;
        parse_tcp_packet(packet + ihl, &header);
        uint32_t thl = (header.data_offset_reserved_flags >> 12) * 4;
        if (thl < 20 || ihl + thl > total) continue;
        /* Verify the complete TCP pseudoheader checksum before changing state. */
        uint32_t sum = 6 + total - ihl;
        for (uint32_t i = 12; i < 20; i += 2) sum += ((uint32_t)packet[i] << 8) | packet[i + 1];
        for (uint32_t i = ihl; i + 1 < total; i += 2) sum += ((uint32_t)packet[i] << 8) | packet[i + 1];
        if ((total - ihl) & 1) sum += (uint32_t)packet[total - 1] << 8;
        while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
        if (sum != 65535) continue;
        on_tcp_packet(packet + 12, packet + 16, &header, packet + ihl, total - ihl, sockets);
        // libcr2 can replace a socket on RST/SYN before the next serve pass.
        // Releasing here prevents its previous transaction crossing slot reuse.
        if (serving) for (unsigned i = 0; i < MAX_SOCKETS; ++i) {
            if (streams[i].receive_installed && (!sockets[i].used
                || tcp_receive_data(&sockets[i]) != streams[i].tcp_rx)) {
                rntp_sessions_release(&sessions, i);
                zero(&streams[i], sizeof(Stream));
            }
        }
    }
}

static int serve(uint32_t mib, uint16_t port) {
    uint64_t bytes = (uint64_t)mib << 20;
    uint8_t *image = malloc(bytes);
    if (!image) { print((const uint8_t *)"rntp: cannot allocate export\n"); return 1; }
    if (netdisk_call(5, image, bytes)) { print((const uint8_t *)"rntp: FAT format failed\n"); free(image); return 1; }
    uint64_t generation = get_ticks() + 1;
    TcpSocket_T *listener = socket_tcp4(sockets);
    if (!listener) { free(image); return 1; }
    bind(listener, port); listen(listener);
    uint8_t *undo = malloc(bytes);
    if (!undo || !rntp_sessions_init(&sessions, image, undo, bytes / 512, generation)) {
        print((const uint8_t *)"rntp: cannot allocate shared export undo buffer\n");
        if (undo) free(undo);
        free(image); reset(listener); return 1;
    }
    serving = 1;
    printf((const uint8_t *)"rntp: serving %u MiB %s on TCP %u\n", mib,
           (const uint8_t *)(image[58] == '2' ? "FAT12" : "FAT16"), (uint32_t)port);
    for (;;) {
        uint64_t now = get_ticks();
        network(now);
        for (uint32_t i = 0; i < MAX_SOCKETS; ++i) {
            TcpSocket_T *s = &sockets[i]; Stream *st = &streams[i];
            if (s == listener) continue;
            if (!s->used) { rntp_sessions_release(&sessions, i); zero(st, sizeof(*st)); continue; }
            if (!st->receive_installed) {
                if (!tcp_set_receive_buffer(s, st->tcp_rx, sizeof(st->tcp_rx))) { reset(s); continue; }
                st->receive_installed = 1;
                send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
            }
            if (!st->activity) st->activity = now;
            if (s->state != SOCKET_ESTABLISHED || now - st->activity >= LEASE_TIMEOUT) {
                reset(s); continue;
            }
            transmit(s, st, now);
            if (!s->used || st->tx_len) continue;
            int ready = incoming(s, st);
            if (ready < 0) { reset(s); continue; }
            if (!ready) continue;
            RntpHeader request, response;
            rntp_decode(st->rx, &request);
            uint32_t length = rntp_sessions_handle(&sessions, i, st->rx, st->tx);
            if (!length) { reset(s); continue; }
            rntp_decode(st->tx, &response);
            if ((request.op == RNTP_HELLO || request.op == RNTP_HELLO_SHARED) && response.status == RNTP_OK)
                print((const uint8_t *)"rntp: client attached\n");
            st->rx_len = 0;
            st->close_after = response.status != RNTP_OK
                && !(request.op == RNTP_LOCK && response.status == RNTP_BUSY);
            outgoing(st, length, now);
            transmit(s, st, now);
        }
        sleep_ms(1);
    }
}

static void make_request(Stream *st, uint8_t op, uint64_t id, uint64_t lba,
                         uint64_t generation, const uint8_t *data, uint64_t now) {
    RntpHeader header = { op, 0, data ? RNTP_SECTOR : 0, id, lba, generation };
    rntp_encode(st->tx, &header);
    if (data) copy(st->tx + RNTP_HEADER, data, RNTP_SECTOR);
    outgoing(st, RNTP_HEADER + header.length, now);
}

static int connect_disk(uint8_t ip[4], uint16_t port, uint16_t local_port, int shared) {
    uint8_t local[4]; net_get_local_ip(local);
    TcpSocket_T *s = tcp_connect(sockets, ip, port, local_port, local);
    if (!s) return 1;
    if (!tcp_set_receive_buffer(s, streams[s->id].tcp_rx, sizeof(streams[s->id].tcp_rx))) { reset(s); return 1; }
    streams[s->id].receive_installed = 1;
    uint64_t start = get_ticks(), last_syn = start;
    while (s->used && s->state == SOCKET_SYN_SENT && get_ticks() - start < EXCHANGE_TIMEOUT) {
        uint64_t now = get_ticks(); network(now);
        if (s->state == SOCKET_SYN_SENT && now - last_syn >= RETRANSMIT_MS) {
            uint32_t next = s->seq_num; s->seq_num = 0;
            send_tcp_packet(s, 0, 0, TCP_FLAG_SYN); s->seq_num = next; last_syn = now;
        }
        sleep_ms(1);
    }
    if (!s->used || s->state != SOCKET_ESTABLISHED) {
        print((const uint8_t *)"rntp: TCP connection failed\n"); reset(s); return 1;
    }
    Stream *st = &streams[s->id];
    NetDiskRequest pending; zero(&pending, sizeof(pending));
    NetDiskBatch batch; zero(&batch, sizeof(batch));
    NetDiskAttachInfo mount;
    RntpReadAhead reads; zero(&reads, sizeof(reads));
    NetDiskReadAheadReply read_reply;
    int read_bridge = netdisk_call(13, 0, 0) == RNTP_MAX_WRITES;
    uint32_t read_count = 0;
    uint64_t data_start = 0;
    uint64_t generation = 0, expected_id = 1, expected_lba = 0, control_id = 1;
    uint8_t hello = shared ? RNTP_HELLO_SHARED : RNTP_HELLO;
    uint8_t expected_op = hello;
    int waiting = 1, attached = 0, batch_capable = 0, read_capable = 0, caps_checked = 0, fallback = 0;
    uint64_t retry_at = 0;
    uint64_t last_stat = 0;
    start = get_ticks();
    make_request(st, hello, expected_id, 0, 0, 0, start);
    for (;;) {
        uint64_t now = get_ticks(); network(now);
        if (!s->used || s->state != SOCKET_ESTABLISHED) break;
        transmit(s, st, now);
        if (retry_at && now >= retry_at && !st->tx_len) {
            make_request(st, RNTP_LOCK, expected_id, 0, generation, 0, now);
            retry_at = 0;
        }
        if (waiting) {
            int ready = incoming(s, st);
            if (ready < 0 || now - start >= EXCHANGE_TIMEOUT) break;
            if (ready) {
                RntpHeader reply;
                if (!rntp_decode(st->rx, &reply) || reply.id != expected_id
                    || reply.op != (expected_op | RNTP_RESPONSE)
                    || (expected_op != hello && (reply.generation != generation || reply.lba != expected_lba))) break;
                if (reply.status) {
                    if (shared && expected_op == hello
                        && (reply.status == RNTP_INVALID || reply.status == RNTP_BUSY)) fallback = 1;
                    if (expected_op == RNTP_LOCK && reply.status == RNTP_BUSY && !reply.length) {
                        st->rx_len = 0; retry_at = now + 5; continue;
                    }
                    break;
                }
                if (expected_op == hello) {
                    if (reply.length != 512 || !reply.generation || !reply.lba || reply.lba > NETDISK_MAX / 512) break;
                    mount.attach.sectors = reply.lba; mount.attach.generation = reply.generation;
                    copy(mount.attach.boot, st->rx + RNTP_HEADER, 512);
                    generation = reply.generation;
                    const uint8_t *boot = mount.attach.boot;
                    data_start = (boot[14] | ((uint32_t)boot[15] << 8))
                        + boot[16] * (boot[22] | ((uint32_t)boot[23] << 8))
                        + ((boot[17] | ((uint32_t)boot[18] << 8)) * 32 + 511) / 512;
                } else if (expected_op == RNTP_CAPS) {
                    if (reply.length != 8) break;
                    uint64_t caps = rntp_get_number(st->rx + RNTP_HEADER, 8);
                    batch_capable = (caps & RNTP_CAP_BATCH) != 0;
                    read_capable = (caps & RNTP_CAP_READ_BATCH) != 0;
                    if (shared && (caps & (RNTP_CAP_BATCH | RNTP_CAP_SHARED))
                        != (RNTP_CAP_BATCH | RNTP_CAP_SHARED)) break;
                    caps_checked = 1;
                } else if (expected_op == RNTP_WRITE_BATCH) {
                    if (reply.length) break;
                    NetDiskBatchReply complete = { batch.id, batch.count, 0 };
                    if (netdisk_call(10, &complete, 0)) break;
                } else if (expected_op == RNTP_STAT) {
                    if (reply.length != RNTP_STAT_BYTES) break;
                    uint64_t free_bytes = rntp_decode_free_space(st->rx + RNTP_HEADER);
                    if (!attached) {
                        mount.free_bytes = free_bytes;
                        uint64_t result = shared ? netdisk_call(11, &mount, 0) :
                            (batch_capable ? netdisk_call(8, &mount, 0) : netdisk_call(7, &mount, 0));
                        if (shared && result == 0xfc) { fallback = 1; break; }
                        if (result == 0xfc && batch_capable) { batch_capable = 0; result = netdisk_call(7, &mount, 0); }
                        if (result) {
                            print((const uint8_t *)"rntp: mount refused (busy, invalid FAT or old kernel)\n"); break;
                        }
                        attached = 1;
                        print((const uint8_t *)"rntp: mounted at /mnt/net\n");
                    } else {
                        NetDiskUsageUpdate update = { generation, free_bytes };
                        if (netdisk_call(6, &update, 0)) break;
                    }
                    last_stat = now;
                } else if (expected_op == RNTP_READ_BATCH) {
                    if (pending.op != RNTP_READ || !rntp_read_ahead_store(&reads,
                        expected_lba, mount.attach.sectors, read_count,
                        st->rx + RNTP_HEADER, reply.length)) break;
                    if (read_bridge) {
                        read_reply.id = pending.id; read_reply.lba = pending.lba;
                        read_reply.count = read_count; read_reply.status = 0;
                        copy(read_reply.data, reads.data, read_count * RNTP_SECTOR);
                        // Publish all acknowledged sectors at once, so the
                        // filesystem does not wake the daemon 32 more times.
                        if (netdisk_call(12, &read_reply, 0)) break;
                        rntp_read_ahead_clear(&reads);
                    } else {
                        pending.status = 0;
                        if (!rntp_read_ahead_get(&reads, pending.lba, pending.data)
                            || netdisk_call(3, &pending, 0)) break;
                    }
                } else if (expected_op == RNTP_READ || expected_op == RNTP_WRITE
                    || expected_op == RNTP_LOCK || expected_op == RNTP_UNLOCK || expected_op == RNTP_ABORT) {
                    uint32_t length = expected_op == RNTP_READ ? 512U : expected_op == RNTP_WRITE ? 0U : 8U;
                    if (reply.length != length) break;
                    pending.status = 0;
                    if (reply.length) copy(pending.data, st->rx + RNTP_HEADER, reply.length);
                    if (netdisk_call(3, &pending, 0)) break;
                } else if (reply.length) break;
                st->rx_len = 0; waiting = 0; st->activity = now;
            }
        }
        if (!waiting && !st->tx_len && !attached && generation) {
            expected_op = caps_checked ? RNTP_STAT : RNTP_CAPS; expected_id = ++control_id; expected_lba = 0;
            waiting = 1; start = now;
            make_request(st, expected_op, expected_id, 0, generation, 0, now);
        }
        if (!waiting && !st->tx_len && attached) {
            if (now - last_stat >= HEARTBEAT_MS) {
                // One local-server STAT replaces the heartbeat, including
                // during long transfers; foreground metadata queries never wait.
                expected_op = RNTP_STAT; expected_id = ++control_id;
                expected_lba = 0; waiting = 1; start = now;
                make_request(st, RNTP_STAT, expected_id, 0, generation, 0, now);
            } else {
                uint64_t result = batch_capable ? netdisk_call(9, &batch, 0) : 0;
                if (result == 1) {
                    if (!batch.count || batch.count > RNTP_MAX_WRITES) break;
                    rntp_read_ahead_clear(&reads);
                    expected_op = RNTP_WRITE_BATCH; expected_id = batch.id; expected_lba = 0;
                    waiting = 1; start = now;
                    RntpHeader header = { expected_op, 0, 4 + batch.count * RNTP_WRITE_RECORD,
                        expected_id, 0, generation };
                    rntp_encode(st->tx, &header);
                    rntp_put_number(st->tx + RNTP_HEADER, batch.count, 4);
                    for (uint32_t i = 0; i < batch.count; ++i) {
                        uint8_t *record = st->tx + RNTP_HEADER + 4 + i * RNTP_WRITE_RECORD;
                        rntp_put_number(record, batch.writes[i].lba, 8);
                        copy(record + 8, batch.writes[i].data, RNTP_SECTOR);
                    }
                    outgoing(st, RNTP_HEADER + header.length, now);
                    continue;
                } else if (result) break;
                result = netdisk_call(2, &pending, 0);
                if (result == 1) {
                    if (pending.op == RNTP_READ && rntp_read_ahead_get(&reads, pending.lba, pending.data)) {
                        pending.status = 0;
                        if (netdisk_call(3, &pending, 0)) break;
                        // No TCP exchange for the rest of this contiguous run.
                        sleep_ms(1); continue;
                    }
                    if (pending.op != RNTP_READ) rntp_read_ahead_clear(&reads);
                    expected_op = (uint8_t)pending.op; expected_id = pending.id;
                    expected_lba = pending.lba; waiting = 1; start = now;
                    if (pending.op == RNTP_READ && read_capable) {
                        read_count = rntp_read_ahead_count(pending.lba, mount.attach.sectors, data_start);
                        if (!read_count) break;
                        expected_op = RNTP_READ_BATCH;
                        RntpHeader header = { expected_op, 0, 4, expected_id, expected_lba, generation };
                        rntp_encode(st->tx, &header);
                        rntp_put_number(st->tx + RNTP_HEADER, read_count, 4);
                        outgoing(st, RNTP_HEADER + 4, now);
                    } else {
                        make_request(st, expected_op, expected_id, expected_lba, generation,
                                     expected_op == RNTP_WRITE ? pending.data : 0, now);
                    }
                } else if (result) break;
            }
        }
        sleep_ms(1);
    }
    if (attached) {
        uint64_t deadline = get_ticks() + 1000;
        while (netdisk_call(4, 0, 0) == 0xfa && get_ticks() < deadline) sleep_ms(1);
        print((const uint8_t *)"rntp: disconnected; /mnt/net unmounted\n");
    } else if (!fallback) print((const uint8_t *)"rntp: RNTP handshake failed\n");
    reset(s);
    return fallback ? 2 : 1;
}

int main(int argc, char **argv) {
    if (netdisk_call(0, 0, 0) != NETDISK_ABI) {
        print((const uint8_t *)"rntp: kernel lacks netdisk ABI\n"); return 1;
    }
    int server = argc >= 2 && equal(argv[1], "serve");
    int client = argc >= 3 && equal(argv[1], "connect");
    uint32_t size = 16, port = RNTP_PORT;
    uint8_t ip[4];
    if ((!server && !client) || argc > 4
        || (server && argc >= 3 && (!number(argv[2], &size) || size < 2 || size > 64))
        || (client && !address(argv[2], ip))
        || (argc == 4 && (!number(argv[3], &port) || !port))) {
        print((const uint8_t *)"Usage: rntp serve [2..64 MiB] [port]\n       rntp connect IPv4 [port]\n"); return 1;
    }
    tcp_set_mss(TCP_MAX_PAYLOAD);
    tcp_set_local_packet_crafting(1);
    socket_pool_init(sockets); zero(streams, sizeof(streams));
    uint16_t local_port = server ? (uint16_t)port : (uint16_t)(48000 + get_ticks() % 1000);
    if (net_driver_bind_port((const uint8_t *)"eth", local_port) < 0) {
        print((const uint8_t *)"rntp: network port unavailable\n"); return 1;
    }
    net_set_nonblocking(1);
    if (server) return serve(size, (uint16_t)port);
    int result = connect_disk(ip, (uint16_t)port, local_port, 1);
    if (result != 2) return result;
    // A legacy peer closes an unknown shared HELLO. Use a fresh TCP tuple so
    // its final ACK/RST cannot tear down the exclusive fallback connection.
    ++local_port;
    if (net_driver_bind_port((const uint8_t *)"eth", local_port) < 0) return 1;
    return connect_disk(ip, (uint16_t)port, local_port, 0);
}
