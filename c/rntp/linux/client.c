#define _POSIX_C_SOURCE 200809L
#include "client.h"
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#define EMPTY UINT64_MAX
static uint64_t milliseconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void invalidate(RntpClient *c) {
    for (unsigned i = 0; i < 224; ++i) c->cache[i].lba = EMPTY;
}
static int fail(RntpClient *c, int error) {
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    c->count = 0; c->locked = 0; invalidate(c); return error;
}
static int bytes(RntpClient *c, uint8_t *buffer, size_t size, int sending, uint64_t deadline) {
    while (size) {
        ssize_t n = sending ? send(c->fd, buffer, size, MSG_NOSIGNAL) : recv(c->fd, buffer, size, 0);
        if (n > 0) { buffer += n; size -= n; continue; }
        if (!n) return fail(c, -EIO);
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return fail(c, -EIO);
        uint64_t now = milliseconds(); if (now >= deadline) return fail(c, -ETIMEDOUT);
        struct pollfd p = { .fd = c->fd, .events = sending ? POLLOUT : POLLIN };
        int result = poll(&p, 1, (int)(deadline - now));
        if (!result) return fail(c, -ETIMEDOUT);
        if (result < 0 && errno != EINTR) return fail(c, -EIO);
    }
    return 0;
}
static int rpc(RntpClient *c, uint8_t op, uint64_t lba, uint32_t length, RntpHeader *response) {
    if (c->fd < 0) return -EIO;
    RntpHeader h = { op, 0, length, ++c->id, lba, op == RNTP_HELLO_SHARED ? 0 : c->generation };
    rntp_encode(c->frame, &h);
    if (length) memcpy(c->frame + RNTP_HEADER, c->payload, length);
    uint64_t deadline = milliseconds() + 5000;
    int e = bytes(c, c->frame, RNTP_HEADER + length, 1, deadline);
    if (e < 0) return e;
    e = bytes(c, c->frame, RNTP_HEADER, 0, deadline); if (e < 0) return e;
    if (!rntp_decode(c->frame, response) || response->id != h.id || response->op != (op | RNTP_RESPONSE)
        || (op != RNTP_HELLO_SHARED && (response->lba != lba || response->generation != c->generation)))
        return fail(c, -EPROTO);
    e = bytes(c, c->payload, response->length, 0, deadline); if (e < 0) return e;
    if (response->status == RNTP_BUSY && op == RNTP_LOCK && !response->length) return -EBUSY;
    if (response->status) return fail(c, response->status == RNTP_BUSY ? -EBUSY :
        response->status == RNTP_STALE ? -ESTALE : -EPROTONOSUPPORT);
    return 0;
}
static void cache_put(RntpClient *c, uint64_t lba, const uint8_t data[512]) {
    unsigned begin = lba < c->metadata_end ? 0 : 192, end = begin ? 224 : 192, at = begin;
    for (unsigned i = begin; i < end; ++i) {
        if (c->cache[i].lba == lba || c->cache[i].lba == EMPTY) { at = i; break; }
        if (c->cache[i].stamp < c->cache[at].stamp) at = i;
    }
    c->cache[at].lba = lba; c->cache[at].stamp = ++c->clock;
    memcpy(c->cache[at].data, data, 512);
}
static int flush(RntpClient *c) {
    if (!c->count) return 0;
    rntp_put_number(c->payload, c->count, 4);
    for (unsigned i = 0; i < c->count; ++i) {
        uint8_t *p = c->payload + 4 + i * RNTP_WRITE_RECORD;
        rntp_put_number(p, c->writes[i].lba, 8); memcpy(p + 8, c->writes[i].data, 512);
    }
    RntpHeader h; int e = rpc(c, RNTP_WRITE_BATCH, 0, 4 + c->count * RNTP_WRITE_RECORD, &h);
    if (e < 0) return e;
    if (h.length) return fail(c, -EPROTO);
    for (unsigned i = 0; i < c->count; ++i) cache_put(c, c->writes[i].lba, c->writes[i].data);
    c->count = 0; return 0;
}
int rntp_client_connect(RntpClient *c, const char *host, const char *port) {
    memset(c, 0, sizeof(*c)); c->fd = -1; invalidate(c); atomic_init(&c->stop, 0);
    int result = pthread_mutex_init(&c->mutex, NULL); if (result) return -result;
    c->initialized = 1;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *addresses;
    if (getaddrinfo(host, port, &hints, &addresses)) return -EHOSTUNREACH;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0) continue;
        int e = connect(fd, a->ai_addr, a->ai_addrlen);
        if (e < 0 && errno == EINPROGRESS) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            if (poll(&p, 1, 5000) > 0) {
                socklen_t n = sizeof(e); if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n)) e = errno;
            } else e = ETIMEDOUT;
        } else if (e < 0) e = errno;
        if (!e) { c->fd = fd; int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); break; }
        close(fd);
    }
    freeaddrinfo(addresses); if (c->fd < 0) return -ECONNREFUSED;
    RntpHeader h; result = rpc(c, RNTP_HELLO_SHARED, 0, 0, &h); if (result < 0) return result;
    if (h.length != 512 || !h.generation || !h.lba || h.lba > (64U << 20) / 512) return fail(c, -EPROTO);
    c->sectors = h.lba; c->generation = h.generation; memcpy(c->boot, c->payload, 512);
    result = rpc(c, RNTP_CAPS, 0, 0, &h); if (result < 0) return result;
    uint64_t required = RNTP_CAP_BATCH | RNTP_CAP_SHARED | RNTP_CAP_READ_BATCH;
    if (h.length != 8 || (rntp_get_number(c->payload, 8) & required) != required) return fail(c, -EPROTONOSUPPORT);
    const uint8_t *b = c->boot;
    c->metadata_end = (b[14] | (b[15] << 8)) + b[16] * (b[22] | (b[23] << 8))
        + ((b[17] | (b[18] << 8)) * 32 + 511) / 512;
    return 0;
}
int rntp_client_begin(RntpClient *c) {
    pthread_mutex_lock(&c->mutex);
    uint64_t deadline = milliseconds() + 5000; RntpHeader h; int e;
    for (;;) {
        e = rpc(c, RNTP_LOCK, 0, 0, &h);
        if (e != -EBUSY || milliseconds() >= deadline) break;
        struct timespec delay = { .tv_nsec = 5000000 }; nanosleep(&delay, NULL);
    }
    if (e < 0) { pthread_mutex_unlock(&c->mutex); return e; }
    if (h.length != 8 || !rntp_get_number(c->payload, 8)) {
        fail(c, -EPROTO); pthread_mutex_unlock(&c->mutex); return -EPROTO;
    }
    uint64_t revision = rntp_get_number(c->payload, 8);
    if (revision != c->revision) invalidate(c);
    c->revision = revision; c->locked = 1; return 0;
}
int rntp_client_finish(RntpClient *c, int commit) {
    int e = 0;
    if (!c->locked) e = -EIO;
    else {
        if (commit) e = flush(c); else { c->count = 0; invalidate(c); }
        if (e >= 0) {
            RntpHeader h; e = rpc(c, commit ? RNTP_UNLOCK : RNTP_ABORT, 0, 0, &h);
            if (e >= 0) {
                if (h.length != 8 || !rntp_get_number(c->payload, 8)) e = fail(c, -EPROTO);
                else c->revision = rntp_get_number(c->payload, 8);
            }
        }
    }
    c->locked = 0; pthread_mutex_unlock(&c->mutex); return e;
}
int rntp_client_read(void *ptr, uint64_t lba, uint8_t data[512]) {
    RntpClient *c = ptr;
    if (!c->locked || c->fd < 0 || lba >= c->sectors) return -EIO;
    for (unsigned i = 0; i < c->count; ++i)
        if (c->writes[i].lba == lba) { memcpy(data, c->writes[i].data, 512); return 0; }
    for (unsigned i = 0; i < 224; ++i)
        if (c->cache[i].lba == lba) {
            c->cache[i].stamp = ++c->clock; memcpy(data, c->cache[i].data, 512); return 0;
        }
    int e = flush(c); if (e < 0) return e;
    uint32_t count = c->sectors - lba < 32 ? c->sectors - lba : 32;
    if (lba < c->metadata_end && count > c->metadata_end - lba) count = c->metadata_end - lba;
    rntp_put_number(c->payload, count, 4);
    RntpHeader h; e = rpc(c, RNTP_READ_BATCH, lba, 4, &h); if (e < 0) return e;
    if (h.length != count * 512) return fail(c, -EPROTO);
    for (unsigned i = 0; i < count; ++i) cache_put(c, lba + i, c->payload + i * 512);
    memcpy(data, c->payload, 512); return 0;
}
int rntp_client_write(void *ptr, uint64_t lba, const uint8_t data[512]) {
    RntpClient *c = ptr;
    if (!c->locked || c->fd < 0 || !lba || lba >= c->sectors) return -EIO;
    unsigned at;
    for (at = 0; at < c->count && c->writes[at].lba != lba; ++at) {}
    if (at == RNTP_MAX_WRITES) { int e = flush(c); if (e < 0) return e; at = 0; }
    if (at == c->count) ++c->count;
    c->writes[at].lba = lba; memcpy(c->writes[at].data, data, 512); return 0;
}
int rntp_client_free(RntpClient *c, uint64_t *bytes_out) {
    if (!c->locked) return -EIO;
    RntpHeader h; int e = rpc(c, RNTP_STAT, 0, 0, &h); if (e < 0) return e;
    uint64_t value = rntp_get_number(c->payload, 8);
    if (h.length != 8 || value > c->sectors * 512) return fail(c, -EPROTO);
    *bytes_out = value; return 0;
}
static void *heartbeat(void *ptr) {
    RntpClient *c = ptr; unsigned ticks = 0;
    while (!atomic_load(&c->stop)) {
        struct timespec delay = { .tv_nsec = 100000000 }; nanosleep(&delay, NULL);
        if (++ticks < 20) continue;
        ticks = 0;
        if (!pthread_mutex_trylock(&c->mutex)) {
            if (c->fd >= 0) { RntpHeader h; if (!rpc(c, RNTP_PING, 0, 0, &h) && h.length) fail(c, -EPROTO); }
            pthread_mutex_unlock(&c->mutex);
        }
    }
    return NULL;
}
int rntp_client_start(RntpClient *c) {
    int e = pthread_create(&c->heartbeat, NULL, heartbeat, c);
    if (e) return -e;
    c->started = 1; return 0;
}
void rntp_client_close(RntpClient *c) {
    if (!c->initialized) return;
    atomic_store(&c->stop, 1);
    if (c->started) { pthread_join(c->heartbeat, NULL); c->started = 0; }
    pthread_mutex_lock(&c->mutex); fail(c, -EIO); pthread_mutex_unlock(&c->mutex);
    pthread_mutex_destroy(&c->mutex); c->initialized = 0;
}
