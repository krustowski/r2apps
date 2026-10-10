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
    typedef void (*NetFrameSink_T)(const uint8_t *frame, uint32_t len);
    void net_set_frame_sink(NetFrameSink_T fn);
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
    bool active = false;
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

//  The ports this process bound, as netmux_port_bound() was told: the
//  kernel's registry has sixteen for the whole machine.
struct PortNote
{
    uint16_t port;
    const char *use;
};
PortNote g_ports[16];

uint8_t g_frame[FRAME];

uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

bool listening(int who, uint64_t now)
{
    return g_stash[who].active && now - g_stash[who].lastPull < IDLE_MS;
}

bool toolId(uint16_t id)
{
    return id == NETMUX_NSK_ICMP_ID || id == NETMUX_PING_ICMP_ID || id == NETMUX_TRACE_ICMP_ID;
}

//  An ICMP message about one of the Network window's probes: an echo reply
//  with one of its ids, or a time-exceeded or unreachable error that quotes
//  an echo request with one (traceroute's answers from the routers on the
//  way).  `icmp` is `len` bytes.
bool toolIcmp(const uint8_t *icmp, size_t len)
{
    if (len < 8)
        return false;
    if (icmp[0] == 0)
        return toolId(get16(icmp + 4));
    if (icmp[0] != 3 && icmp[0] != 11)
        return false;
    //  The error quotes the probe's IP header and its first eight bytes.
    const uint8_t *inner = icmp + 8;
    if (len < 8 + 20)
        return false;
    size_t ihl = (size_t)(inner[0] & 15) * 4;
    if ((inner[0] >> 4) != 4 || inner[9] != 1 || ihl < 20 || len < 8 + ihl + 8)
        return false;
    return inner[ihl] == 8 && toolId(get16(inner + ihl + 4));
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
    size_t total = get16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || total < ihl || total > n - 14 ||
        (get16(ip + 6) & 0x3fff)) return fallback;
    n = 14 + total;
    //  An answer to one of the Network window's probes.
    if (proto == 1 && n >= 14 + ihl + 8 && toolIcmp(ip + ihl, n - 14 - ihl))
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
    g_stash[who].active = true;

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
        g_stats.rx_frames++;
        g_stats.rx_bytes += (uint64_t)n;
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

extern "C" void netmux_forget(int who)
{
    if (who < 0 || who >= NETMUX_CLIENTS) return;
    Stash &s = g_stash[who];
    s.head = s.count = 0; s.active = false;
}

extern "C" NetmuxStats netmux_stats() { return g_stats; }

extern "C" bool netmux_send(const uint8_t *frame, uint32_t len)
{
    //  The kernel refuses only a frame it cannot make sense of; a full
    //  transmit ring loses the frame without saying so.
    int64_t r = r2::raw_syscall(r2::Sys::SendPacket, 0x04, (int64_t)frame, (int64_t)len);
    if (r != 0)
        return false;
    g_stats.tx_frames++;
    g_stats.tx_bytes += len;
    return true;
}

extern "C" void netmux_port_bound(uint16_t port, const char *use)
{
    PortNote *free = nullptr;
    for (PortNote &p : g_ports)
    {
        if (p.use && p.port == port)
        {
            p.use = use;
            return;
        }
        if (!p.use && !free)
            free = &p;
    }
    if (free)
        *free = PortNote{port, use};
}

extern "C" void netmux_port_released(uint16_t port)
{
    for (PortNote &p : g_ports)
        if (p.use && p.port == port)
            p = PortNote{};
}

extern "C" const char *netmux_port_use(uint16_t port)
{
    for (const PortNote &p : g_ports)
        if (p.use && p.port == port)
            return p.use;
    return nullptr;
}

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

//  And its way out, counted with the rest.
void cr2Sink(const uint8_t *frame, uint32_t len) { netmux_send(frame, len); }

} // namespace

void netmux_install_cr2()
{
    net_set_frame_source(cr2Source);
    net_set_frame_sink(cr2Sink);
}
