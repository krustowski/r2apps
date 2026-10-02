//
// netmux — see netmux.h.
//

#include "netmux.h"

#include <r2/heap.hpp>
#include <r2/libc.hpp>
#include <r2/net.hpp>
#include <r2/syscall.hpp>
#include <r2/time.hpp>

//  From c/libcr2's net.h, declared here: its headers define their own integer
//  types and clash with libc++r2's.
extern "C"
{
    typedef int64_t (*NetFrameSource_T)(uint8_t *buf, uint32_t cap, uint8_t blocking);
    void net_set_frame_source(NetFrameSource_T fn);
}

namespace {

const uint32_t FRAME = 2048;
//  Frames a stack may have waiting here while it is not asking: a full TCP
//  window of the browser's, and then some.
const int STASH = 32;
//  A stack that has not asked for this long is not listening: its frames are
//  not kept for it.
const uint64_t IDLE_MS = 3000;

struct Stash
{
    uint8_t *frames = nullptr; // STASH * FRAME, allocated on first use
    uint16_t len[STASH] = {};
    int head = 0, count = 0;
    uint64_t lastPull = 0;
};

Stash g_stash[NETMUX_CLIENTS];
bool g_driver = false;
NetmuxStats g_stats = {};

struct Claim
{
    int who;
    uint16_t lo, hi;
};
Claim g_claims[8];
int g_nclaims = 0;

uint8_t g_frame[FRAME];

uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

bool listening(int who, uint64_t now)
{
    return g_stash[who].lastPull && now - g_stash[who].lastPull < IDLE_MS;
}

//  Whose the frame is: a client, or -1 for all of them (ARP).
int owner(const uint8_t *f, size_t n, uint64_t now)
{
    int fallback = listening(NETMUX_CR2, now) || !listening(NETMUX_WEB, now) ? NETMUX_CR2 : NETMUX_WEB;
    if (n < 14)
        return fallback;
    uint16_t type = get16(f + 12);
    if (type == 0x0806)
        return -1;
    if (type != 0x0800 || n < 14 + 20)
        return fallback;
    const uint8_t *ip = f + 14;
    uint8_t proto = ip[9];
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    //  An echo reply to one of the host scan's probes.
    if (proto == 1 && n >= 14 + ihl + 8 && ip[ihl] == 0 && get16(ip + ihl + 4) == NETMUX_NSK_ICMP_ID)
        return listening(NETMUX_NSK, now) ? NETMUX_NSK : fallback;
    if ((proto == 6 || proto == 17) && n >= 14 + ihl + 4)
    {
        uint16_t dport = get16(ip + ihl + 2);
        for (int i = 0; i < g_nclaims; i++)
            if (dport >= g_claims[i].lo && dport <= g_claims[i].hi)
                return g_claims[i].who;
        return NETMUX_CR2;
    }
    //  ICMP and the rest: to one stack, so an echo is answered once --- the
    //  browser's when it is there, since c/libcr2's answers only as a server.
    return listening(NETMUX_WEB, now) ? NETMUX_WEB : fallback;
}

void keep(int who, const uint8_t *f, size_t n)
{
    Stash &s = g_stash[who];
    if (!s.frames)
        s.frames = (uint8_t *)r2::heap::allocate((size_t)STASH * FRAME);
    if (!s.frames || s.count == STASH)
    {
        g_stats.dropped[who]++;
        return;
    }
    int at = (s.head + s.count) % STASH;
    memcpy(s.frames + (size_t)at * FRAME, f, n);
    s.len[at] = (uint16_t)n;
    s.count++;
}

int64_t give(int who, uint8_t *buf, uint32_t cap)
{
    Stash &s = g_stash[who];
    size_t n = s.len[s.head];
    if (n > cap)
        n = cap;
    memcpy(buf, s.frames + (size_t)s.head * FRAME, n);
    s.head = (s.head + 1) % STASH;
    s.count--;
    g_stats.frames[who]++;
    return (int64_t)n;
}

} // namespace

extern "C" int64_t netmux_pull(int who, uint8_t *buf, uint32_t cap)
{
    if (who < 0 || who >= NETMUX_CLIENTS)
        return 0;
    uint64_t now = r2::ticks();
    g_stash[who].lastPull = now;

    //  What waited here comes first: it arrived first.
    if (g_stash[who].count)
        return give(who, buf, cap);

    //  Then the kernel's queue, sorting as it goes, until a frame is this
    //  caller's or the queue is empty.  Bounded, like any caller's loop.
    for (int k = 0; k < 64; k++)
    {
        int64_t n = r2::raw_syscall(r2::Sys::ReceivePort, 0, (int64_t)g_frame);
        if (n <= 0)
            return 0;
        if ((uint64_t)n > FRAME)
            continue;
        int to = owner(g_frame, (size_t)n, now);
        if (to == -1)
        {
            //  Everyone's: a copy for each of the others that is there to
            //  ask; a client that is not running needs nothing kept.
            for (int c = 0; c < NETMUX_CLIENTS; c++)
                if (c != who && listening(c, now))
                    keep(c, g_frame, (size_t)n);
        }
        else if (to != who)
        {
            if (listening(to, now))
                keep(to, g_frame, (size_t)n);
            else
                g_stats.dropped[to]++;
            continue;
        }
        size_t take = (size_t)n < cap ? (size_t)n : cap;
        memcpy(buf, g_frame, take);
        g_stats.frames[who]++;
        return (int64_t)take;
    }
    return 0;
}

extern "C" void netmux_claim(int who, uint16_t lo, uint16_t hi)
{
    for (int i = 0; i < g_nclaims; i++)
        if (g_claims[i].who == who && g_claims[i].lo == lo && g_claims[i].hi == hi)
            return;
    if (g_nclaims < 8)
        g_claims[g_nclaims++] = Claim{who, lo, hi};
}

extern "C" NetmuxStats netmux_stats() { return g_stats; }

extern "C" bool netmux_take_driver()
{
    if (g_driver)
        return true;
    auto st = r2::net::status();
    if (st && !st->driver_active && r2::net::register_driver())
        g_driver = true;
    return g_driver;
}

extern "C" bool netmux_is_driver() { return g_driver; }

namespace {

//  c/libcr2's side: its driver asks through net_set_frame_source.  A blocking
//  ask waits here, a millisecond at a time.
int64_t cr2Source(uint8_t *buf, uint32_t cap, uint8_t blocking)
{
    for (;;)
    {
        int64_t n = netmux_pull(NETMUX_CR2, buf, cap);
        if (n > 0 || !blocking)
            return n;
        r2::sleep(1);
    }
}

} // namespace

void netmux_install_cr2() { net_set_frame_source(cr2Source); }
