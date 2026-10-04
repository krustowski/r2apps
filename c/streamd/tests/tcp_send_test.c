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
static int ack_pending, lose_acks, blackhole, bad_data, fail_send;
static int reopen_on_probe, disconnect_at;
static const uint8_t *expected;
static uint32_t expected_len;

uint64_t get_ticks(void) { return clock_ms; }
void sleep_ms(uint64_t ms) { clock_ms += ms; }
void net_set_time(uint32_t secs) { (void)secs; }
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
    if (!ack_pending) return 0;
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
int64_t capture_framebuffer_rgb24_scaled(uint8_t *out, uint32_t w, uint32_t h) {
    if (capture_busy) return FB_CAPTURE_BUSY;
    for (uint32_t i = 0; i < w * h * 3; ++i) out[i] = (uint8_t)scene;
    return 0;
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
    initial_seq = receiver_seq = 10000;
    clock_ms = calls = retries = resets = pending_ack = drop_call = take_once = 0;
    ack_pending = lose_acks = blackhole = bad_data = fail_send = 0;
    reopen_on_probe = disconnect_at = 0;
    expected = payload; expected_len = sizeof(payload);
    for (uint32_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 17);
}
static int completed(void) {
    return !bad_data && client.seq_num == initial_seq + sizeof(payload) &&
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
    puts("streamd: TCP recovery, multipart framing, pacing, JPEG reuse and busy capture passed");
    return 0;
}
