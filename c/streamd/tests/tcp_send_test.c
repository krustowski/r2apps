/* Host regression tests for streamd's sender; no r2 kernel required.
 * gcc -O2 -ffreestanding -ffunction-sections -fdata-sections -Ilibcr2 \
 *   streamd/tests/tcp_send_test.c -Wl,--gc-sections -o /tmp/streamd-tcp-test
 */
#define main streamd_main
#include "../main.c"
#undef main

extern int puts(const char *);

static TcpSocket_T *live;
static uint64_t clock_ms;
static uint32_t receiver_seq, initial_seq, calls, retries, resets;
static uint32_t pending_ack, drop_call, take_once;
static uint16_t advertised_window;
static uint16_t advertised_mss;
static uint32_t max_segment, ack_delay_ms;
static uint64_t ack_ready_at;
static int ack_pending, lose_acks, blackhole, bad_data, fail_send;
static int reopen_on_probe, disconnect_at;
static const uint8_t *expected;
static uint32_t expected_len;

uint64_t get_ticks(void) { return clock_ms; }
void sleep_ms(uint64_t ms) { clock_ms += ms; }
void net_set_time(uint32_t secs) { (void)secs; }
uint16_t tcp_peer_mss(const TcpSocket_T *s) { (void)s; return advertised_mss; }
int64_t write_file_at(const uint8_t *path, const uint8_t *data,
                      uint64_t offset, uint64_t len) {
    (void)path; (void)data; (void)offset;
    return (int64_t)len;
}
void free_socket(TcpSocket_T *s) { s->used = 0; s->state = SOCKET_CLOSED; }

void send_tcp_packet(TcpSocket_T *s, const uint8_t *data, uint32_t len,
                     uint8_t flags) {
    uint32_t seq = s->seq_num;
    if (flags & TCP_FLAG_RST) { ++resets; return; }
    ++calls;
    if (len > max_segment) max_segment = len;
    if (s != live) ++retries;
    if (fail_send) return;
    s->seq_num += len;
    if (blackhole || calls == drop_call) return;
    if (reopen_on_probe && s != live) {
        advertised_window = 4096;
        reopen_on_probe = 0;
    }
    if (advertised_window && seq == receiver_seq) {
        if (take_once && len > take_once) {
            len = take_once;
            take_once = 0;
        }
        for (uint32_t i = 0; i < len; ++i) {
            uint32_t offset = seq + i - initial_seq;
            if (offset >= expected_len || data[i] != expected[offset])
                bad_data = 1;
        }
        receiver_seq += len;
    }
    if (lose_acks && s == live) return;
    pending_ack = receiver_seq;
    ack_pending = 1;
    ack_ready_at = clock_ms + ack_delay_ms;
}
uint32_t write(TcpSocket_T *s, const uint8_t *data, uint32_t len) {
    send_tcp_packet(s, data, len, TCP_FLAG_ACK);
    return len;
}

int net_recv_nb(uint8_t *buf, uint32_t cap) {
    (void)cap;
    if (disconnect_at && clock_ms >= (uint64_t)disconnect_at) {
        free_socket(live);
        return 0;
    }
    if (!ack_pending || clock_ms < ack_ready_at) return 0;
    for (uint32_t i = 0; i < 40; ++i) buf[i] = 0;
    buf[0] = 0x45;
    buf[2] = 0; buf[3] = 40; buf[9] = 6;
    uint32_t ack = swap32(pending_ack);
    copy_bytes(buf + 28, (const uint8_t *)&ack, 4);
    buf[32] = 0x50; buf[33] = TCP_FLAG_ACK;
    buf[34] = advertised_window >> 8;
    buf[35] = advertised_window & 255;
    ack_pending = 0;
    return 40;
}
uint16_t parse_ipv4_packet(const uint8_t *p, Ipv4Header_T *ip) {
    ip->protocol = p[9];
    ip->total_length = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
    return 20;
}
uint16_t parse_tcp_packet(const uint8_t *p, TcpHeader_T *tcp) {
    tcp->ack_num = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
                   ((uint32_t)p[10] << 8) | p[11];
    tcp->window_size = ((uint16_t)p[14] << 8) | p[15];
    return 20;
}
void on_tcp_packet(const uint8_t src[4], const uint8_t dst[4],
                   TcpHeader_T *tcp, const uint8_t *p, uint32_t len,
                   TcpSocket_T pool[MAX_SOCKETS]) {
    (void)src; (void)dst; (void)p; (void)len; (void)pool;
    live->tx_acked = tcp->ack_num;
    live->peer_window = tcp->window_size;
}

static uint8_t test_rgb_a[RGB_BUFFER_SIZE] __attribute__((aligned(8)));
static uint8_t test_rgb_b[RGB_BUFFER_SIZE] __attribute__((aligned(8)));
static uint8_t test_jpeg[JPEG_BUFFER_SIZE];
static int scene, capture_busy;
static int metadata_mode;
static uint64_t snapshot_id;
static uint32_t rgb_copies;
int64_t capture_framebuffer_rgb24_scaled(uint8_t *out, uint32_t w, uint32_t h) {
    if (capture_busy) return FB_CAPTURE_BUSY;
    for (uint32_t i = 0; i < w * h * 3; ++i) out[i] = (uint8_t)scene;
    return 0;
}
int64_t capture_framebuffer_rgb24_scaled_if_new(uint8_t *out, uint32_t w,
                                                uint32_t h, FBCaptureInfo_T *info) {
    if (capture_busy) return FB_CAPTURE_BUSY;
    if (metadata_mode) {
        uint64_t requested = info->frame_id;
        *info = (FBCaptureInfo_T){snapshot_id, clock_ms, FB_CAPTURE_INFO_SNAPSHOT, 0};
        if (requested && requested == snapshot_id) return FB_CAPTURE_UNCHANGED;
    }
    ++rgb_copies;
    return capture_framebuffer_rgb24_scaled(out, w, h);
}
static uint8_t payload[18000];
static uint8_t multipart[FRAME_HEADER_RESERVE + sizeof(payload) + 2];
static TcpSocket_T client;
static void setup(uint16_t window) {
    client = (TcpSocket_T){0};
    live = &client;
    client.used = 1;
    client.state = SOCKET_ESTABLISHED;
    client.seq_num = client.tx_acked = 10000;
    client.peer_window = advertised_window = window;
    advertised_mss = 1460;
    max_segment = ack_delay_ms = 0;
    ack_ready_at = 0;
    initial_seq = receiver_seq = 10000;
    clock_ms = calls = retries = resets = pending_ack = drop_call = take_once = 0;
    ack_pending = lose_acks = blackhole = bad_data = fail_send = 0;
    reopen_on_probe = disconnect_at = 0;
    expected = payload; expected_len = sizeof(payload);
    for (uint32_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 17);
}
static int completed(void) {
    return !bad_data && client.seq_num == initial_seq + (uint32_t)sizeof(payload) &&
           client.tx_acked == client.seq_num && receiver_seq == client.seq_num;
}
int main(void) {
    setup(4096);
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed() || retries)
        return 1;
    setup(4096); drop_call = 1;
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed() || !retries)
        return 2;
    setup(4096); drop_call = 3;
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed() || !retries)
        return 3;
    setup(4096); lose_acks = 1;
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed() || !retries)
        return 4;
    setup(73);
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed())
        return 5;
    setup(0); reopen_on_probe = 1;
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed() || !retries)
        return 6;
    setup(4096); blackhole = 1;
    if (send_bytes(&client, payload, sizeof(payload)) || client.used || resets != 1 ||
        clock_ms != TCP_STALL_MS)
        return 7;
    /* A new connection works after a stalled one is reset. */
    setup(4096);
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed()) return 8;
    setup(4096); blackhole = 1; disconnect_at = 20;
    if (send_bytes(&client, payload, sizeof(payload)) || clock_ms != 20) return 9;
    setup(4096); fail_send = 1;
    if (send_bytes(&client, payload, sizeof(payload)) || clock_ms) return 10;
    setup(0);
    if (send_bytes(&client, payload, sizeof(payload)) || client.used || resets != 1)
        return 11;
    setup(4096); take_once = 200;
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed() || !retries)
        return 12;
    setup(4096); advertised_window = 0; reopen_on_probe = 1;
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed() || !retries)
        return 13;
    /* Correct multipart framing, with no JPEG copy in the streaming path. */
    setup(4096);
    uint8_t *jpeg = multipart + FRAME_HEADER_RESERVE;
    copy_bytes(jpeg, payload, sizeof(payload));
    uint32_t hs = prepare_mjpeg_frame(jpeg, sizeof(payload));
    static const uint8_t header[] = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 18000\r\n\r\n";
    if (hs != sizeof(header) - 1 || jpeg[sizeof(payload)] != '\r' ||
        jpeg[sizeof(payload) + 1] != '\n') return 14;
    for (uint32_t i = 0; i < hs; ++i)
        if ((jpeg - hs)[i] != header[i]) return 14;
    expected = jpeg - hs;
    expected_len = hs + sizeof(payload) + 2;
    drop_call = 3;
    if (!send_bytes(&client, expected, expected_len) || bad_data ||
        receiver_seq != initial_seq + expected_len ||
        client.seq_num != receiver_seq || client.tx_acked != receiver_seq)
        return 15;

    uint32_t fraction = 0, elapsed = 0;
    for (uint32_t i = 0; i < STREAM_FPS * 10; ++i)
        elapsed += frame_interval_ms(&fraction);
    if (elapsed != 10000 || fraction) return 16;
    /* Reuse identical JPEGs exactly, but encode changed or invalid images. */
    rgb_buffer = test_rgb_a;
    previous_rgb = test_rgb_b;
    jpeg_buffer = test_jpeg;
    jpeg_valid = 0; scene = 37; capture_busy = 0;
    if (capture_frame() != 1 || !jpeg_valid || !jpeg_size) return 17;
    uint8_t *last_rgb = previous_rgb;
    uint32_t last_size = jpeg_size;
    if (capture_frame() != 1 || previous_rgb != last_rgb || jpeg_size != last_size)
        return 18;
    scene = 92;
    if (capture_frame() != 1 || previous_rgb == last_rgb) return 19;
    last_rgb = previous_rgb; last_size = jpeg_size;
    capture_busy = 1;
    if (capture_frame() != -1 || previous_rgb != last_rgb || jpeg_size != last_size || !jpeg_valid)
        return 20;
    capture_busy = 0;
    jpeg_valid = 0; // A failed encoder must never reuse its partial output.
    if (capture_frame() != 1 || previous_rgb == last_rgb || !jpeg_valid) return 21;

    /* The second write can reach the receiver before the first final ACK. */
    setup(65535); ack_delay_ms = 40;
    TcpTxQueue tx;
    if (!tx_init(&tx, &client) || !tx_enqueue(&tx, payload, 9000)) return 22;
    if (tx_poll(&tx) != 2 || receiver_seq != initial_seq + 9000 ||
        client.tx_acked != initial_seq || !tx_enqueue(&tx, payload + 9000, 9000)) return 23;
    if (tx_poll(&tx) != 2 || receiver_seq != initial_seq + sizeof(payload) ||
        client.tx_acked != initial_seq || tx.count != 2) return 24;
    if (tx_drop_unsent_tail(&tx)) return 25; // Sent buffers cannot be discarded.
    clock_ms = 40;
    if (tx_poll(&tx) || tx.count || !completed()) return 26;

    /* Replace only an unsent tail, including its reserved sequence range. */
    setup(65535); ack_delay_ms = 40;
    if (!tx_init(&tx, &client) || !tx_enqueue(&tx, payload, 9000)) return 27;
    tx_poll(&tx);
    if (!tx_enqueue(&tx, payload + 9000, 8000) || !tx_drop_unsent_tail(&tx) ||
        tx.end_seq != initial_seq + 9000 || !tx_enqueue(&tx, payload + 9000, 9000)) return 28;
    tx_poll(&tx); clock_ms = 40;
    if (tx_poll(&tx) || !completed()) return 29;

    /* The row hook moves networking while a JPEG is being encoded. */
    setup(65535); ack_delay_ms = 40;
    if (!tx_init(&tx, &client) || !tx_enqueue(&tx, payload, sizeof(payload))) return 30;
    encoding_tx = &tx;
    scene = 122;
    if (capture_frame() != 1 || receiver_seq != initial_seq + sizeof(payload) ||
        client.tx_acked != initial_seq) return 31;
    encoding_tx = 0; clock_ms = 40;
    if (tx_poll(&tx) || !completed()) return 32;

    setup(4096); advertised_mss = 536;
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed() || max_segment > 536) return 33;
    setup(4096);
    initial_seq = receiver_seq = client.seq_num = client.tx_acked = 0xfffffe00U;
    if (!send_bytes(&client, payload, sizeof(payload)) || !completed()) return 34;

    metadata_mode = 1; snapshot_id = 7; rgb_copies = 0; jpeg_valid = 0;
    if (capture_frame() != 1 || rgb_copies != 1 || last_snapshot_id != 7) return 35;
    last_rgb = previous_rgb;
    if (capture_frame() != 1 || rgb_copies != 1 || previous_rgb != last_rgb || !capture_reused) return 36;
    ++snapshot_id; scene = 201;
    if (capture_frame() != 1 || rgb_copies != 2 || capture_reused || last_snapshot_id != 8) return 37;
    metadata_mode = 0; // Older kernels ignore arg3: exact comparison still works.
    if (capture_frame() != 1 || rgb_copies != 3 || !capture_reused || last_snapshot_id) return 38;

    /* Grow movable output on demand, preserving multipart headroom and every
     * byte already emitted; shrink only once the stored image fits. */
    jpeg_storage[0].allocation = (uint8_t *)malloc(FRAME_HEADER_RESERVE + 4096 + 2);
    if (!jpeg_storage[0].allocation) return 39;
    jpeg_storage[0].capacity = 4096;
    jpeg_buffers[0] = jpeg_storage[0].allocation + FRAME_HEADER_RESERVE;
    jpeg_buffer = jpeg_buffers[0]; jpeg_slot = 0; jpeg_size = 0;
    jpeg_storage[0].allocation[0] = 73;
    jpeg_write_callback(0, payload, sizeof(payload));
    jpeg_write_callback(0, test_rgb_a, 200000);
    if (jpeg_size != sizeof(payload) + 200000 || jpeg_storage[0].capacity != 262144 ||
        jpeg_storage[0].allocation[0] != 73) return 40;
    for (uint32_t i = 0; i < sizeof(payload); ++i)
        if (jpeg_buffer[i] != payload[i]) return 41;
    for (uint32_t i = 0; i < 200000; ++i)
        if (jpeg_buffer[sizeof(payload) + i] != test_rgb_a[i]) return 42;
    if (!resize_jpeg_slot(0, JPEG_INITIAL_BUFFER_SIZE) || reserve_jpeg(JPEG_BUFFER_SIZE + 1)) return 43;
    free(jpeg_storage[0].allocation); jpeg_slot = -1;
    puts("streamd: recovery, pipeline, delayed ACKs, MSS, sequence wrap, JPEG reuse and snapshot metadata passed");
    return 0;
}
