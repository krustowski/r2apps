#define _POSIX_C_SOURCE 200809L
#include "sessions.h"
#include "fixture.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>

typedef struct { int fd; uint32_t rx, tx, sent; uint64_t active; uint8_t in[RNTP_FRAME], out[RNTP_FRAME]; } Peer;
static volatile sig_atomic_t stop;
static void stop_server(int n) { (void)n; stop = 1; }
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void drop(RntpSessions *s, Peer *p, unsigned i) {
    close(p[i].fd); p[i].fd = -1; p[i].rx = p[i].tx = p[i].sent = 0; rntp_sessions_release(s, i);
}
int main(int argc, char **argv) {
    unsigned mib = argc > 1 ? (unsigned)atoi(argv[1]) : 2;
    if (mib != 2 && mib != 16 && mib != 64) return 2;
    uint8_t *image = fixture(mib), *undo = malloc(mib << 20); assert(undo);
    RntpSessions *s = malloc(sizeof(*s)); assert(s && rntp_sessions_init(s, image, undo, mib * 2048, 123));
    Peer *peers = calloc(RNTP_PEERS, sizeof(*peers)); assert(peers);
    for (unsigned i = 0; i < RNTP_PEERS; ++i) peers[i].fd = -1;
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); assert(listener >= 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert(!bind(listener, (void *)&a, sizeof(a)) && !listen(listener, 8));
    socklen_t size = sizeof(a); assert(!getsockname(listener, (void *)&a, &size));
    signal(SIGTERM, stop_server); signal(SIGINT, stop_server);
    printf("%u\n", (unsigned)ntohs(a.sin_port)); fflush(stdout);
    while (!stop) {
        struct pollfd p[RNTP_PEERS + 1] = {{ .fd = listener, .events = POLLIN }};
        for (unsigned i = 0; i < RNTP_PEERS; ++i) {
            p[i + 1].fd = peers[i].fd; p[i + 1].events = peers[i].tx ? POLLOUT : POLLIN;
        }
        int result = poll(p, RNTP_PEERS + 1, 100);
        if (result < 0) { if (errno == EINTR) continue; break; }
        if (p[0].revents & POLLIN) {
            int fd = accept(listener, NULL, NULL);
            if (fd >= 0) {
                unsigned i; for (i = 0; i < RNTP_PEERS && peers[i].fd >= 0; ++i) {}
                if (i == RNTP_PEERS) close(fd);
                else {
                    int one = 1; fcntl(fd, F_SETFL, O_NONBLOCK); setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    peers[i].fd = fd; peers[i].active = now();
                }
            }
        }
        for (unsigned i = 0; i < RNTP_PEERS; ++i) {
            Peer *peer = peers + i;
            if (peer->fd < 0) continue;
            if (now() - peer->active >= 10000 || p[i + 1].revents & (POLLERR | POLLHUP | POLLNVAL)) { drop(s, peers, i); continue; }
            if (peer->tx && p[i + 1].revents & POLLOUT) {
                ssize_t n = send(peer->fd, peer->out + peer->sent, peer->tx - peer->sent, MSG_NOSIGNAL);
                if (n > 0) { peer->sent += n; peer->active = now(); if (peer->sent == peer->tx) peer->tx = peer->sent = 0; }
                else if (!n || (errno != EINTR && errno != EAGAIN)) { drop(s, peers, i); continue; }
            }
            if (!peer->tx && p[i + 1].revents & POLLIN) {
                uint32_t want = RNTP_HEADER; RntpHeader h;
                if (peer->rx >= RNTP_HEADER) {
                    if (!rntp_decode(peer->in, &h)) { drop(s, peers, i); continue; }
                    want += h.length;
                }
                ssize_t n = recv(peer->fd, peer->in + peer->rx, want - peer->rx, 0);
                if (n > 0) { peer->rx += n; peer->active = now(); }
                else if (!n || (errno != EINTR && errno != EAGAIN)) { drop(s, peers, i); continue; }
                if (peer->rx >= RNTP_HEADER) {
                    if (!rntp_decode(peer->in, &h)) { drop(s, peers, i); continue; }
                    if (peer->rx == RNTP_HEADER + h.length) {
                        peer->tx = rntp_sessions_handle(s, i, peer->in, peer->out); peer->rx = 0;
                        if (!peer->tx) drop(s, peers, i);
                    }
                }
            }
        }
    }
    for (unsigned i = 0; i < RNTP_PEERS; ++i) if (peers[i].fd >= 0) drop(s, peers, i);
    if (argc > 2) { FILE *out = fopen(argv[2], "wb"); assert(out); assert(fwrite(image, mib << 20, 1, out) == 1); fclose(out); }
    close(listener); free(s); free(image); free(undo); free(peers); return 0;
}
