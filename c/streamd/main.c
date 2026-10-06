/*
 * rou2exOS screen streamer
 *
 * GET /stream
 *     -> multipart/x-mixed-replace MJPEG stream
 *
 * Intended for:
 *
 *     http://10.3.4.2:8080/stream
 *
 * OBS:
 *     Add Browser Source and use the URL above.
 *
 * Requires:
 *     libcr2/net.c
 *     libcr2/syscall.c
 *     libcr2/mem.c
 *     libcr2/string.c
 *     libcr2/printf.c
 *
 * JPEG:
 *     TinyJPEG (public-domain single-header JPEG encoder)
 *
 * Frame capture:
 *     get_fb_info() provides framebuffer geometry.
 *     capture_framebuffer_rgb24_scaled() returns a complete RGB24 picture.
 *     Cooperating presenters supply completed snapshots in RAM; otherwise
 *     the kernel checks that framebuffer capture did not overlap a write.
 */

#include "bytes.h"
#include "mem.h"
#include "net.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

/*
 * TinyJPEG:
 *
 *     https://github.com/RT-Thread-packages/TinyJPEG/blob/master/tiny_jpeg.h
 *
 * It is public-domain according to its upstream project.
 *
 * We use tje_encode_with_func(), not FILE/stdout.
 */
static void pump_during_encode(void);
#define TJE_YIELD() pump_during_encode()
#define TJE_IMPLEMENTATION
#include "tiny_jpeg.h"

/* ------------------------------------------------------------- */
/* Configuration                                                  */
/* ------------------------------------------------------------- */

#define STREAM_PORT 8080
#define STREAM_FPS 30
#define STREAM_INTERVAL_MS (1000 / STREAM_FPS)
#define FRAME_HEADER_RESERVE 128U
#define TCP_FLAG_PSH 0x08
#define STREAM_WIDTH 640
#define STREAM_HEIGHT 480
#define RGB_BUFFER_SIZE (STREAM_WIDTH * STREAM_HEIGHT * 3U)

/*
 * TinyJPEG has quality values:
 *
 * 1 = smallest
 * 2 = finer quantization (larger files; the base tables are divided by 10)
 * 3 = highest
 */
#ifndef JPEG_QUALITY
#define JPEG_QUALITY 2
#endif

/*
 * Maximum TCP payload we give libcr2.
 *
 * The kernel builds TCP requests in 1400 bytes, with 30 bytes of
 * request metadata. Stay below its 1370-byte payload limit.
 *
 * The browser advertises its own receive window. send_bytes() further
 * limits each segment to the space currently available in that window.
 */
#define TCP_CHUNK 1300U

/*
 * Maximum JPEG buffer.
 *
 * This is deliberately generous for ordinary desktop resolutions.
 *
 * 1280x720 screenshots at quality 2 should normally be far below
 * this. 1920x1080 can get considerably larger depending on the
 * screen contents.
 */
#define JPEG_BUFFER_SIZE (1024 * 1024)
#define JPEG_INITIAL_BUFFER_SIZE (64U * 1024U)

/*
 * HTTP request buffer.
 */
#define HTTP_REQUEST_SIZE 1024

/* Persistent diagnostic log. */
#define STREAM_LOG_PATH "/mnt/tmp/STREAMD.LOG"

/* ------------------------------------------------------------- */
/* Globals                                                        */
/* ------------------------------------------------------------- */

static TcpSocket_T sockets[MAX_SOCKETS];

static FBInfo_T fb;

static uint8_t *rgb_buffer;
static uint8_t *previous_rgb;
static int jpeg_valid;
static uint8_t *jpeg_buffer;
static uint8_t *jpeg_buffers[2];
static struct { uint8_t *allocation; uint32_t capacity; } jpeg_storage[2];
static int jpeg_slot = -1; /* -1 for callers supplying their own JPEG storage. */
static uint8_t *cached_jpeg_buffer;
static uint32_t cached_jpeg_size;
static uint64_t last_snapshot_id;
static FBCaptureInfo_T capture_info;
static uint64_t captured_at;
static int capture_reused;

static uint32_t jpeg_size;

/*
 * Lightweight performance counters.
 *
 * Counters stay in memory during capture/encoding/transmission. One summary
 * is written every five seconds, outside the TCP segment loop.
 */
static uint32_t perf_frame_count;
static uint32_t perf_capture_ms;
static uint32_t perf_jpeg_ms;
static uint32_t perf_retries;
static uint32_t perf_capture_busy;
static uint32_t perf_dropped;
static uint32_t perf_skipped;

static uint64_t log_offset;
static int log_enabled;

/* ------------------------------------------------------------- */
/* Tiny helpers                                                   */
/* ------------------------------------------------------------- */

static uint32_t str_len(const uint8_t *s) {
    uint32_t n = 0;

    while (s[n])
        ++n;

    return n;
}

static void log_write_raw(const uint8_t *data, uint32_t len) {
    int64_t written;

    if (!log_enabled || !data || !len)
        return;

    written = write_file_at((const uint8_t *)STREAM_LOG_PATH, data, log_offset, (uint64_t)len);

    if (written > 0)
        log_offset += (uint64_t)written;
}

static void log_text(const uint8_t *text) {
    if (text)
        log_write_raw(text, str_len(text));
}

static void log_u32(uint32_t value) {
    uint8_t buf[16];
    uint32_t n = 0;
    uint32_t i;

    if (value == 0) {
        buf[n++] = '0';
    } else {
        while (value && n < sizeof(buf)) {
            buf[n++] = (uint8_t)('0' + (value % 10));
            value /= 10;
        }

        for (i = 0; i < n / 2; ++i) {
            uint8_t t = buf[i];
            buf[i] = buf[n - 1 - i];
            buf[n - 1 - i] = t;
        }
    }

    log_write_raw(buf, n);
}

static void log_hex8(uint8_t value) {
    static const uint8_t hex[] = "0123456789ABCDEF";
    uint8_t buf[2];

    buf[0] = hex[(value >> 4) & 0x0F];
    buf[1] = hex[value & 0x0F];
    log_write_raw(buf, 2);
}

static void log_line(const uint8_t *text) {
    log_text(text);
    log_text((const uint8_t *)"\n");
}

static void log_init(void) {
    /*
     * Start a fresh log for every run.  write_file_at() creates the
     * file when necessary, so failure to delete a nonexistent file is
     * harmless.
     */
    delete_file((const uint8_t *)STREAM_LOG_PATH);
    log_offset = 0;
    log_enabled = 1;
    log_line((const uint8_t *)"[streamer] log started");
}

/*
 * We intentionally don't use libcr2 memcpy() here for large copies:
 * its prototype uses uint16_t for the length.
 */
static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t i;

    for (i = 0; i < n; ++i)
        dst[i] = src[i];
}

/* Both RGB buffers come from malloc and are aligned for 64-bit loads. Compare
 * exactly, rather than relying on a hash that could reuse a different picture. */
static int buffers_equal(const uint8_t *a, const uint8_t *b, uint32_t len) {
    const uint64_t *aw = (const uint64_t *)a;
    const uint64_t *bw = (const uint64_t *)b;
    uint32_t words = len / sizeof(uint64_t);
    uint32_t i;
    for (i = 0; i < words; ++i)
        if (aw[i] != bw[i])
            return 0;
    for (i = words * sizeof(uint64_t); i < len; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* ------------------------------------------------------------- */
/* JPEG output callback                                           */
/* ------------------------------------------------------------- */

static int resize_jpeg_slot(uint32_t slot, uint32_t capacity) {
    uint8_t *old = jpeg_buffers[slot];
    uint8_t *allocation = (uint8_t *)realloc(jpeg_storage[slot].allocation,
                                            FRAME_HEADER_RESERVE + capacity + 2U);
    if (!allocation) return 0;
    jpeg_storage[slot].allocation = allocation;
    jpeg_storage[slot].capacity = capacity;
    jpeg_buffers[slot] = allocation + FRAME_HEADER_RESERVE;
    if (cached_jpeg_buffer == old) cached_jpeg_buffer = jpeg_buffers[slot];
    if (jpeg_buffer == old) jpeg_buffer = jpeg_buffers[slot];
    return 1;
}

static int reserve_jpeg(uint32_t required) {
    uint32_t capacity;
    if (required > JPEG_BUFFER_SIZE) return 0;
    if (jpeg_slot < 0) return 1;
    capacity = jpeg_storage[jpeg_slot].capacity;
    if (required <= capacity) return 1;
    if (capacity < JPEG_INITIAL_BUFFER_SIZE) capacity = JPEG_INITIAL_BUFFER_SIZE;
    while (capacity < required) capacity *= 2;
    return resize_jpeg_slot((uint32_t)jpeg_slot, capacity);
}

/*
 * TinyJPEG calls this repeatedly as JPEG data becomes available.
 *
 * context = jpeg_buffer
 *
 * TinyJPEG normally writes approximately 1 KiB chunks, so this is
 * also a convenient way to avoid any large internal allocation.
 */
static void jpeg_write_callback(void *context, void *data, int size) {
    (void)context; /* Storage may move while the synchronous encoder runs. */

    if (size <= 0)
        return;

    if (jpeg_size > JPEG_BUFFER_SIZE ||
        (uint32_t)size > JPEG_BUFFER_SIZE - jpeg_size ||
        !reserve_jpeg(jpeg_size + (uint32_t)size)) {
        /*
         * We cannot report an error through this callback using the
         * TinyJPEG API. The caller checks jpeg_size afterwards.
         */
        jpeg_size = JPEG_BUFFER_SIZE + 1;
        return;
    }

    copy_bytes(jpeg_buffer + jpeg_size, (const uint8_t *)data, (uint32_t)size);

    jpeg_size += (uint32_t)size;
}

/* ------------------------------------------------------------- */
/* Framebuffer -> RGB                                             */
/* ------------------------------------------------------------- */

/*
 * The GRUB framebuffer is normally 32bpp on rou2exOS, but get_fb_info()
 * exposes bpp so we reject formats we don't know how to interpret.
 *
 * For 32bpp this assumes:
 *
 *     B8 G8 R8 X8
 *
 * which is the common VESA/GRUB little-endian layout.
 *
 * If rou2exOS's framebuffer is actually X8 R8 G8 B8, swap the
 * assignments below.
 */
static int framebuffer_to_rgb(void) {
    /*
     * Kernel performs capture + nearest-neighbour downscale + conversion
     * directly into the RGB24 buffer consumed by TinyJPEG.
     */
    int64_t result;
    capture_info = (FBCaptureInfo_T){jpeg_valid ? last_snapshot_id : 0, 0, 0, 0};
    result = capture_framebuffer_rgb24_scaled_if_new(rgb_buffer, STREAM_WIDTH, STREAM_HEIGHT, &capture_info);
    if (result == FB_CAPTURE_UNCHANGED && jpeg_valid &&
        (capture_info.flags & FB_CAPTURE_INFO_SNAPSHOT))
        return 2;
    if (result == FB_CAPTURE_BUSY) {
        ++perf_capture_busy;
        return -1; // Discard this RGB buffer; keep the last complete JPEG.
    }
    if (result != 0) {
        log_line((const uint8_t *)"[video] scaled framebuffer capture failed");
        return 0;
    }

    return 1;
}

/* ------------------------------------------------------------- */
/* Frame capture                                                  */
/* ------------------------------------------------------------- */

static int capture_frame(void) {
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    int result;

    t0 = get_ticks();

    result = framebuffer_to_rgb();
    if (result <= 0)
        return result;

    t1 = get_ticks();

    captured_at = capture_info.timestamp_ms ? capture_info.timestamp_ms : t1;
    capture_reused = 0;
    if (jpeg_valid && (result == 2 || buffers_equal(rgb_buffer, previous_rgb, RGB_BUFFER_SIZE))) {
        if (!reserve_jpeg(cached_jpeg_size)) return 0;
        if (jpeg_buffer != cached_jpeg_buffer)
            copy_bytes(jpeg_buffer, cached_jpeg_buffer, cached_jpeg_size);
        jpeg_size = cached_jpeg_size;
        last_snapshot_id = (capture_info.flags & FB_CAPTURE_INFO_SNAPSHOT) ? capture_info.frame_id : 0;
        perf_capture_ms = (uint32_t)(t1 - t0);
        perf_jpeg_ms = (uint32_t)(get_ticks() - t1);
        capture_reused = 1;
        return 1; // Reuse the complete JPEG; do not encode an unchanged desktop.
    }

    jpeg_valid = 0;
    jpeg_size = 0;
    if (!tje_encode_with_func(jpeg_write_callback, jpeg_buffer, JPEG_QUALITY, (int)STREAM_WIDTH, (int)STREAM_HEIGHT, 3, rgb_buffer)) {
        log_line((const uint8_t *)"[jpeg] encoder failed");
        return 0;
    }

    t2 = get_ticks();

    if (jpeg_size == 0 || jpeg_size > JPEG_BUFFER_SIZE) {
        log_line((const uint8_t *)"[jpeg] buffer overflow");
        return 0;
    }

    perf_capture_ms = (uint32_t)(t1 - t0);
    perf_jpeg_ms = (uint32_t)(t2 - t1);
    jpeg_valid = 1;
    cached_jpeg_buffer = jpeg_buffer;
    cached_jpeg_size = jpeg_size;
    last_snapshot_id = (capture_info.flags & FB_CAPTURE_INFO_SNAPSHOT) ? capture_info.frame_id : 0;
    {
        uint8_t *old = previous_rgb;
        previous_rgb = rgb_buffer;
        rgb_buffer = old;
    }

    return 1;
}

/* ------------------------------------------------------------- */
/* TCP helpers                                                    */
/* ------------------------------------------------------------- */

/*
 * Process one raw IPv4 packet returned by net_recv_nb().
 *
 * net_recv_nb() returns the IPv4 packet itself (Ethernet headers have
 * already been removed), and the IPv4 total_length field is still in
 * network byte order.  parse_ipv4_packet() and parse_tcp_packet() both
 * copy raw packet fields into their structures, so validate all lengths
 * before parsing the TCP header.
 */
static void pump_tcp_packet(const uint8_t *packet, uint32_t packet_len) {
    Ipv4Header_T ip;
    uint16_t ihl;
    uint32_t ip_total;
    uint32_t tcp_len;
    uint32_t tcp_hdr_len;
    uint8_t version;

    if (!packet)
        return;

    if (packet_len < 20)
        return;

    version = (uint8_t)(packet[0] >> 4);
    if (version != 4)
        return;

    ihl = (uint16_t)((packet[0] & 0x0F) * 4);

    if (ihl < 20 || (uint32_t)ihl > packet_len)
        return;

    parse_ipv4_packet(packet, &ip);

    if (ip.protocol != 6)
        return;

    ip_total = (uint32_t)swap16(ip.total_length);

    if (ip_total < (uint32_t)ihl + 20)
        return;

    if (ip_total > packet_len)
        return;

    tcp_len = ip_total - (uint32_t)ihl;

    tcp_hdr_len = (uint32_t)(((packet[ihl + 12] >> 4) & 0x0F) * 4);

    if (tcp_hdr_len < 20 || tcp_hdr_len > tcp_len)
        return;

    {
        TcpHeader_T tcp;

        parse_tcp_packet(packet + ihl, &tcp);

        on_tcp_packet(ip.source_addr, ip.destination_addr, &tcp, packet + ihl, tcp_len, sockets);
    }
}

/*
 * Send a potentially large buffer through libcr2.
 *
 * libcr2's write() eventually constructs a complete IPv4 packet,
 * so don't give it the entire JPEG.
 *
 * Pump ACKs between chunks and respect the browser's receive window.
 */
/* libcr2 has no retransmission queue. Keep each logical write alive until
 * acknowledged, so lost segments can be resent directly from its buffer. */
#define TCP_TX_WINDOW_BYTES 32768U
#define TCP_RETRY_MS 100U
#define TCP_RETRY_MAX_MS 2000U
#define TCP_STALL_MS 15000U
#define TCP_RX_BATCH 32U

static void pump_tcp_batch(void) {
    uint8_t rx[1518];
    uint32_t i;

    net_set_time((uint32_t)(get_ticks() / 1000));
    for (i = 0; i < TCP_RX_BATCH; ++i) {
        int len = net_recv_nb(rx, sizeof(rx));
        if (len <= 0)
            break;
        pump_tcp_packet(rx, (uint32_t)len);
    }
}

/* Use a copy: send_tcp_packet advances seq_num, but retransmitting must
 * neither allocate new sequence numbers nor change the live socket. */
static void resend_bytes(TcpSocket_T *sock, uint32_t seq, const uint8_t *data, uint32_t len) {
    ++perf_retries;
    TcpSocket_T retry = *sock;
    retry.seq_num = seq;
    send_tcp_packet(&retry, data, len, TCP_FLAG_ACK | (len ? TCP_FLAG_PSH : 0));
}

/* Entries own immutable buffers until cumulative ACK retirement. Two entries
 * permit frame N+1 to be encoded/sent while frame N's final ACK is pending. */
#define TCP_TX_SLOTS 2U
#define TCP_TX_BATCH 16U

typedef struct {
    const uint8_t *data;
    uint32_t base, len, pos;
    uint64_t started;
    uint32_t slot;
} TcpTxPart;

typedef struct {
    TcpSocket_T *sock;
    TcpTxPart parts[TCP_TX_SLOTS];
    uint32_t head, count, end_seq, acked, retry_ms;
    uint64_t progress_at, retry_at;
    int failed;
    void (*completed)(void *, const TcpTxPart *);
    void *context;
} TcpTxQueue;

static TcpTxQueue *encoding_tx;

static int tx_init(TcpTxQueue *tx, TcpSocket_T *sock) {
    *tx = (TcpTxQueue){0};
    if (sock->tx_acked != sock->seq_num) return 0;
    tx->sock = sock;
    tx->end_seq = tx->acked = sock->seq_num;
    tx->retry_ms = TCP_RETRY_MS;
    tx->progress_at = tx->retry_at = get_ticks();
    return 1;
}

static int tx_enqueue(TcpTxQueue *tx, const uint8_t *data, uint32_t len) {
    uint32_t slot = (tx->head + tx->count) % TCP_TX_SLOTS;
    if (tx->failed || tx->count == TCP_TX_SLOTS || !data || !len) return 0;
    if (!tx->count) {
        tx->progress_at = tx->retry_at = get_ticks();
        tx->retry_ms = TCP_RETRY_MS;
    }
    tx->parts[slot] = (TcpTxPart){data, tx->end_seq, len, 0, get_ticks(), slot};
    tx->end_seq += len;
    ++tx->count;
    return 1;
}

/* A not-yet-sent tail can be replaced without changing any transmitted bytes
 * or TCP sequence numbers. A partially sent frame must always finish. */
static int tx_drop_unsent_tail(TcpTxQueue *tx) {
    uint32_t slot;
    if (!tx->count) return 0;
    slot = (tx->head + tx->count - 1) % TCP_TX_SLOTS;
    if (tx->parts[slot].pos) return 0;
    tx->end_seq = tx->parts[slot].base;
    --tx->count;
    return 1;
}

/* Nonblocking: -1 = failed, 0 = empty, 1 = waiting, 2 = made progress.
 * No sleeps here, including when called from the JPEG row hook. */
static int tx_poll(TcpTxQueue *tx) {
    TcpSocket_T *sock = tx->sock;
    uint64_t now;
    uint32_t advanced, in_flight, allowed, mss;
    int progress = 0;

    if (tx->failed) return -1;
    pump_tcp_batch();
    if (!sock->used || sock->state != SOCKET_ESTABLISHED) goto failed;
    now = get_ticks();
    advanced = sock->tx_acked - tx->acked;
    if (advanced > sock->seq_num - tx->acked) goto failed;
    if (advanced) {
        tx->acked = sock->tx_acked;
        tx->progress_at = tx->retry_at = now;
        tx->retry_ms = TCP_RETRY_MS;
        progress = 1;
    }
    while (tx->count) {
        TcpTxPart *part = &tx->parts[tx->head];
        if ((int32_t)(tx->acked - (part->base + part->len)) < 0) break;
        if (tx->completed) tx->completed(tx->context, part);
        tx->head = (tx->head + 1) % TCP_TX_SLOTS;
        --tx->count;
    }
    if (!tx->count) return 0;
    if (now - tx->progress_at >= TCP_STALL_MS) {
        log_text((const uint8_t *)"[tcp] stalled: unacked=");
        log_u32(sock->seq_num - tx->acked);
        log_text((const uint8_t *)" peer_window=");
        log_u32(sock->peer_window);
        log_line((const uint8_t *)"; resetting client");
        send_tcp_packet(sock, 0, 0, TCP_FLAG_RST | TCP_FLAG_ACK);
        free_socket(sock);
        goto failed;
    }

    allowed = sock->peer_window;
    if (allowed > TCP_TX_WINDOW_BYTES) allowed = TCP_TX_WINDOW_BYTES;
    mss = tcp_peer_mss(sock);
    if (!mss || mss > TCP_CHUNK) mss = TCP_CHUNK;
    for (uint32_t sent = 0; sent < TCP_TX_BATCH; ++sent) {
        TcpTxPart *part = 0;
        uint32_t chunk, before;
        in_flight = sock->seq_num - tx->acked;
        if (in_flight >= allowed) break;
        for (uint32_t i = 0; i < tx->count; ++i) {
            TcpTxPart *candidate = &tx->parts[(tx->head + i) % TCP_TX_SLOTS];
            if (candidate->pos < candidate->len) { part = candidate; break; }
        }
        if (!part) break;
        chunk = part->len - part->pos;
        if (chunk > mss) chunk = mss;
        if (chunk > allowed - in_flight) chunk = allowed - in_flight;
        before = sock->seq_num;
        if (before != part->base + part->pos) goto failed;
        send_tcp_packet(sock, part->data + part->pos, chunk, TCP_FLAG_ACK | TCP_FLAG_PSH);
        if (sock->seq_num - before != chunk) goto failed;
        if (!in_flight) tx->retry_at = now;
        part->pos += chunk;
        progress = 1;
    }

    in_flight = sock->seq_num - tx->acked;
    if (now - tx->retry_at >= tx->retry_ms) {
        TcpTxPart *part = &tx->parts[tx->head];
        uint32_t offset = tx->acked - part->base;
        if (in_flight) {
            uint32_t chunk = part->pos - offset;
            if (chunk > mss) chunk = mss;
            if (!sock->peer_window) chunk = 1;
            else if (chunk > sock->peer_window) chunk = sock->peer_window;
            resend_bytes(sock, tx->acked, part->data + offset, chunk);
        } else if (!sock->peer_window) {
            resend_bytes(sock, tx->acked - 1, offset ? part->data + offset - 1 : 0, offset ? 1 : 0);
        }
        tx->retry_at = now;
        if (tx->retry_ms < TCP_RETRY_MAX_MS) {
            tx->retry_ms *= 2;
            if (tx->retry_ms > TCP_RETRY_MAX_MS) tx->retry_ms = TCP_RETRY_MAX_MS;
        }
    }
    return progress ? 2 : 1;
failed:
    tx->failed = 1;
    return -1;
}

static void pump_during_encode(void) {
    if (encoding_tx) tx_poll(encoding_tx);
}

/* HTTP responses still use a blocking wrapper over the same recovery logic. */
static int send_bytes(TcpSocket_T *sock, const uint8_t *data, uint32_t len) {
    TcpTxQueue tx;
    if (!tx_init(&tx, sock)) return 0;
    if (!len) return 1;
    if (!tx_enqueue(&tx, data, len)) return 0;
    while (tx.count) {
        int result = tx_poll(&tx);
        if (result < 0) return 0;
        if (result == 1) sleep_ms(1);
    }
    return 1;
}

/* ------------------------------------------------------------- */
/* HTTP                                                          */
/* ------------------------------------------------------------- */

static int request_is_stream(const uint8_t *request, uint32_t len) {
    /*
     * We only care about:
     *
     *     GET /stream HTTP/1.x
     *
     * Don't bother implementing a complete HTTP parser.
     */

    static const uint8_t prefix[] = "GET /stream ";

    uint32_t prefix_len = sizeof(prefix) - 1;

    if (len < prefix_len)
        return 0;

    if (memcmp(request, prefix, prefix_len) != 0) {
        return 0;
    }

    return 1;
}

static int serve_http(TcpSocket_T *sock) {
    uint8_t request[HTTP_REQUEST_SIZE];
    uint32_t request_len;

    request_len = read(sock, request, sizeof(request));
    if (!request_len) {
        return 0;
    }

    if (!request_is_stream(request, request_len)) {
        static const uint8_t response[] = "HTTP/1.1 404 Not Found\r\n"
                                          "Content-Type: text/plain\r\n"
                                          "Content-Length: 10\r\n"
                                          "Connection: close\r\n"
                                          "\r\n"
                                          "not found\n";

        send_bytes(sock, response, sizeof(response) - 1);

        return 0;
    }

    /*
     * MJPEG response.
     *
     * OBS/Chromium understands multipart/x-mixed-replace with
     * image/jpeg parts.
     */
    static const uint8_t response[] = "HTTP/1.1 200 OK\r\n"
                                      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                                      "Pragma: no-cache\r\n"
                                      "Connection: close\r\n"
                                      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                                      "\r\n";

    log_line((const uint8_t *)"[http] GET /stream accepted");

    if (!send_bytes(sock, response, sizeof(response) - 1)) {
        log_line((const uint8_t *)"[http] response send failed");
        return 0;
    }

    log_line((const uint8_t *)"[http] MJPEG headers sent");
    return 1;
}

/* ------------------------------------------------------------- */
/* MJPEG stream                                                   */
/* ------------------------------------------------------------- */

/* Use 33/34 ms intervals at 30 FPS instead of rounding every frame down. */
static uint32_t frame_interval_ms(uint32_t *fraction) {
    uint32_t interval = STREAM_INTERVAL_MS;
    *fraction += 1000U % STREAM_FPS;
    if (*fraction >= STREAM_FPS) {
        *fraction -= STREAM_FPS;
        ++interval;
    }
    return interval;
}

/* JPEG storage has headroom and two trailing bytes. Send the complete part
 * in one logical write: separate tiny writes introduce delayed-ACK waits. */
static uint32_t prepare_mjpeg_frame(uint8_t *jpeg, uint32_t size) {
    static const uint8_t prefix[] = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ";
    uint8_t header[FRAME_HEADER_RESERVE];
    uint8_t digits[10];
    uint32_t p = sizeof(prefix) - 1;
    uint32_t n = size;
    uint32_t nd = 0;

    copy_bytes(header, prefix, p);
    do {
        digits[nd++] = (uint8_t)('0' + n % 10);
        n /= 10;
    } while (n);
    while (nd)
        header[p++] = digits[--nd];
    header[p++] = '\r';
    header[p++] = '\n';
    header[p++] = '\r';
    header[p++] = '\n';
    copy_bytes(jpeg - p, header, p);
    jpeg[size] = '\r';
    jpeg[size + 1] = '\n';
    return p;
}

typedef struct {
    uint8_t *jpeg;
    uint32_t size, capture_ms, jpeg_ms;
    uint64_t captured_at, source_id;
    int reused;
} StreamFrame;

typedef struct {
    uint64_t started;
    uint32_t frames, source_frames, encoded_frames;
    uint32_t capture_ms, jpeg_ms, send_ms, bytes, work_max_ms, age_max_ms;
    uint32_t retries, busy, dropped, skipped;
    int source_known;
} PerfWindow;

typedef struct {
    PerfWindow performance;
    StreamFrame frames[TCP_TX_SLOTS];
    uint64_t last_source_id;
} StreamSession;

static void append_text(uint8_t *line, uint32_t *p, const uint8_t *text) {
    while (*text) line[(*p)++] = *text++;
}
static void append_number(uint8_t *line, uint32_t *p, uint32_t value) {
    uint8_t digits[10];
    uint32_t n = 0;
    do { digits[n++] = (uint8_t)('0' + value % 10); value /= 10; } while (value);
    while (n) line[(*p)++] = digits[--n];
}
static void append_fps(uint8_t *line, uint32_t *p, uint32_t frames, uint64_t elapsed) {
    uint32_t fps10 = (uint32_t)((uint64_t)frames * 10000 / elapsed);
    append_number(line, p, fps10 / 10);
    line[(*p)++] = '.';
    line[(*p)++] = (uint8_t)('0' + fps10 % 10);
}
static PerfWindow perf_window(uint64_t now) {
    PerfWindow window = {0};
    window.started = now;
    window.retries = perf_retries;
    window.busy = perf_capture_busy;
    window.dropped = perf_dropped;
    window.skipped = perf_skipped;
    window.source_known = 1;
    return window;
}
static void frame_completed(void *context, const TcpTxPart *part) {
    StreamSession *session = (StreamSession *)context;
    PerfWindow *window = &session->performance;
    StreamFrame *frame = &session->frames[part->slot];
    uint64_t now = get_ticks();
    uint64_t elapsed = now - window->started;
    uint32_t age = (uint32_t)(now - frame->captured_at);
    uint32_t work = frame->capture_ms + frame->jpeg_ms;
    uint8_t line[384];
    uint32_t p = 0;

    /* Shrink only after ACK retirement; no outstanding bytes may move. */
    uint32_t capacity = JPEG_INITIAL_BUFFER_SIZE;
    while (capacity < frame->size) capacity *= 2;
    if (capacity < jpeg_storage[part->slot].capacity)
        resize_jpeg_slot(part->slot, capacity);

    ++perf_frame_count;
    ++window->frames;
    window->encoded_frames += !frame->reused;
    if (frame->source_id) {
        if (frame->source_id != session->last_source_id) ++window->source_frames;
        session->last_source_id = frame->source_id;
    } else window->source_known = 0;
    window->capture_ms += frame->capture_ms;
    window->jpeg_ms += frame->jpeg_ms;
    window->send_ms += (uint32_t)(now - part->started);
    window->bytes += frame->size;
    if (work > window->work_max_ms) window->work_max_ms = work;
    if (age > window->age_max_ms) window->age_max_ms = age;
    if (elapsed < 5000) return;

    append_text(line, &p, (const uint8_t *)"[perf] fps=");
    append_fps(line, &p, window->frames, elapsed);
    append_text(line, &p, (const uint8_t *)" source_fps=");
    if (window->source_known) append_fps(line, &p, window->source_frames, elapsed);
    else append_text(line, &p, (const uint8_t *)"unknown");
    append_text(line, &p, (const uint8_t *)" encoded_fps=");
    append_fps(line, &p, window->encoded_frames, elapsed);
    append_text(line, &p, (const uint8_t *)" capture_ms=");
    append_number(line, &p, window->capture_ms / window->frames);
    append_text(line, &p, (const uint8_t *)" jpeg_ms=");
    append_number(line, &p, window->jpeg_ms / window->frames);
    append_text(line, &p, (const uint8_t *)" send_ms=");
    append_number(line, &p, window->send_ms / window->frames);
    append_text(line, &p, (const uint8_t *)" work_max_ms=");
    append_number(line, &p, window->work_max_ms);
    append_text(line, &p, (const uint8_t *)" age_max_ms=");
    append_number(line, &p, window->age_max_ms);
    append_text(line, &p, (const uint8_t *)" bytes=");
    append_number(line, &p, window->bytes / window->frames);
    append_text(line, &p, (const uint8_t *)" cached=");
    append_number(line, &p, window->frames - window->encoded_frames);
    append_text(line, &p, (const uint8_t *)" retries=");
    append_number(line, &p, perf_retries - window->retries);
    append_text(line, &p, (const uint8_t *)" capture_busy=");
    append_number(line, &p, perf_capture_busy - window->busy);
    append_text(line, &p, (const uint8_t *)" replaced=");
    append_number(line, &p, perf_dropped - window->dropped);
    append_text(line, &p, (const uint8_t *)" skipped=");
    append_number(line, &p, perf_skipped - window->skipped);
    line[p++] = '\n';
    log_write_raw(line, p);
    *window = perf_window(now);
}

static int stream_client(TcpSocket_T *sock) {
    uint64_t next_frame;
    uint32_t fraction = 0;
    TcpTxQueue tx;
    StreamSession session = {0};

    if (!serve_http(sock) || !tx_init(&tx, sock)) return 0;
    session.performance = perf_window(get_ticks());
    for (uint32_t i = 0; i < TCP_TX_SLOTS; ++i) session.frames[i].jpeg = jpeg_buffers[i];
    tx.completed = frame_completed;
    tx.context = &session;
    encoding_tx = &tx;
    next_frame = get_ticks();

    for (;;) {
        uint64_t now;
        uint32_t slot, interval, header_size;
        StreamFrame *frame;
        int result = tx_poll(&tx);
        if (result < 0) break;
        now = get_ticks();
        if (now < next_frame) {
            if (result != 2) sleep_ms(1);
            continue;
        }
        interval = frame_interval_ms(&fraction);
        next_frame += interval;
        if (next_frame <= now) {
            ++perf_skipped;
            next_frame = now + interval;
        }
        if (tx.count == TCP_TX_SLOTS) {
            if (!tx_drop_unsent_tail(&tx)) {
                ++perf_skipped;
                continue;
            }
            ++perf_dropped;
        }
        slot = (tx.head + tx.count) % TCP_TX_SLOTS;
        frame = &session.frames[slot];
        jpeg_slot = (int)slot;
        jpeg_buffer = jpeg_buffers[slot];
        result = capture_frame();
        if (tx.failed) break;
        if (result < 0) {
            next_frame = get_ticks() + 1;
            sleep_ms(1);
            continue;
        }
        if (!result) {
            log_line((const uint8_t *)"[stream] frame capture failed");
            continue;
        }
        frame->size = jpeg_size;
        frame->jpeg = jpeg_buffer;
        frame->capture_ms = perf_capture_ms;
        frame->jpeg_ms = perf_jpeg_ms;
        frame->captured_at = captured_at;
        frame->source_id = last_snapshot_id;
        frame->reused = capture_reused;
        header_size = prepare_mjpeg_frame(frame->jpeg, frame->size);
        /* Encoding can retire earlier entries, but cannot change the next
         * free ring slot: (head + count) remains invariant on retirement. */
        if (!tx_enqueue(&tx, frame->jpeg - header_size, header_size + frame->size + 2U)) break;
    }
    encoding_tx = 0;
    jpeg_slot = -1;
    return 0;
}

/* ------------------------------------------------------------- */
/* Initialisation                                                 */
/* ------------------------------------------------------------- */

static int initialise_video(void) {
    log_line((const uint8_t *)"[video] initialise_video");

    if (get_fb_info(&fb)) {
        log_line((const uint8_t *)"[video] no VESA framebuffer");

        return 0;
    }

    log_text((const uint8_t *)"[video] framebuffer ");
    log_u32(fb.width);
    log_text((const uint8_t *)"x");
    log_u32(fb.height);
    log_text((const uint8_t *)" pitch=");
    log_u32(fb.pitch);
    log_text((const uint8_t *)" bpp=");
    log_u32(fb.bpp);
    log_text((const uint8_t *)"\n");

    log_text((const uint8_t *)"[video] stream resolution ");
    log_u32(STREAM_WIDTH);
    log_text((const uint8_t *)"x");
    log_u32(STREAM_HEIGHT);
    log_text((const uint8_t *)" @ ");
    log_u32(STREAM_FPS);
    log_text((const uint8_t *)" fps\n");

    /*
     * RGB24 image fed to TinyJPEG.
     */
    rgb_buffer = (uint8_t *)malloc(RGB_BUFFER_SIZE);
    previous_rgb = (uint8_t *)malloc(RGB_BUFFER_SIZE);

    if (!rgb_buffer || !previous_rgb) {
        log_line((const uint8_t *)"[video] RGB buffer allocation failed");

        return 0;
    }

    for (uint32_t i = 0; i < 2; ++i) {
        uint8_t *allocation = (uint8_t *)malloc(FRAME_HEADER_RESERVE + JPEG_INITIAL_BUFFER_SIZE + 2U);
        if (!allocation) {
            log_line((const uint8_t *)"[video] JPEG buffer allocation failed");
            return 0;
        }
        jpeg_buffers[i] = allocation + FRAME_HEADER_RESERVE;
        jpeg_storage[i].allocation = allocation;
        jpeg_storage[i].capacity = JPEG_INITIAL_BUFFER_SIZE;
    }
    jpeg_buffer = jpeg_buffers[0];

    return 1;
}

static int initialise_network(void) {
    static const uint8_t eth[] = "eth";

    log_line((const uint8_t *)"[net] initialise_network");

    /*
     * This uses libcr2's port-specific Ethernet binding.
     *
     * It also calls net_register(), so this application can bootstrap
     * the global Ethernet driver if necessary.
     */
    if (net_driver_bind_port(eth, STREAM_PORT) != 0) {

        log_line((const uint8_t *)"[net] network init failed");

        return 0;
    }

    log_line((const uint8_t *)"[net] port binding succeeded");

    net_set_nonblocking(1);
    log_line((const uint8_t *)"[net] nonblocking enabled");

    socket_pool_init(sockets);
    log_line((const uint8_t *)"[net] socket pool initialized");

    /*
     * libcr2's socket_reap() uses this clock.
     */
    net_set_time((uint32_t)(get_ticks() / 1000));

    return 1;
}

/* ------------------------------------------------------------- */
/* Main                                                           */
/* ------------------------------------------------------------- */

int main(int argc, uint8_t **argv) {
    TcpSocket_T *listener;

    (void)argc;
    (void)argv;

    log_init();
    log_line((const uint8_t *)"[streamer] rou2exOS screen streamer");

    if (!initialise_video())
        return 1;

    if (!initialise_network())
        return 1;

    listener = socket_tcp4(sockets);

    if (!listener) {
        log_line((const uint8_t *)"[tcp] socket allocation failed");

        return 1;
    }

    bind(listener, STREAM_PORT);
    listen(listener);
    log_line((const uint8_t *)"[tcp] listening on :8080");

    for (;;) {

        TcpSocket_T *client;

        net_set_time((uint32_t)(get_ticks() / 1000));

        /* Bound each receive batch so timers and accepting clients also run. */
        pump_tcp_batch();

        /*
         * Find a completed HTTP connection.
         *
         * libcr2's accept() deliberately waits until there is request
         * data in rx_buffer, which is convenient for this tiny server.
         */
        client = accept(listener, sockets);

        if (client) {
            log_line((const uint8_t *)"[tcp] client accepted");

            /*
             * This version handles one stream at a time.
             *
             * After OBS disconnects, the socket gets closed and the
             * listener continues accepting the next client.
             */
            stream_client(client);
            close(client);
        }

        /*
         * Keep abandoned sockets from filling the eight-slot pool.
         *
         * Protect the listener (slot determined from its id).
         */
        {
            SocketSet_T protect = (SocketSet_T)(1u << listener->id);

            socket_reap(sockets, 10, protect);
        }

        sleep_ms(10);
    }

    return 0;
}
