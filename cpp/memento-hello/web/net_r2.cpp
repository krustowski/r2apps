//
//  net_r2.cpp --- ARP, IPv4, ICMP, UDP, DNS and a TCP client, for the browser.
//
//  The shape is go/r2net's, which was measured against this kernel, rewritten
//  so that nothing waits: every call returns at once, and poll() --- run from
//  the window's idle loop --- takes in frames and drives the timers.  The
//  points worth knowing, most of them from go/r2net/README.md:
//
//  - The kernel polls the NIC only for the process registered as the global
//    Ethernet driver, and hands it every frame no bound TCP port claims.  When
//    nobody holds that registration this stack takes it and answers ARP and
//    ping for the machine.  When somebody does, it binds its TCP ports
//    instead; ARP replies and UDP then go to the other process, so the
//    gateway's MAC is assumed (the tap convention of this repository) until a
//    frame from it teaches the real one, and DNS falls back to TCP.
//
//  - Frames are lost in bursts: the kernel takes one per timer tick through a
//    single shared buffer.  So the window advertised is small (a burst is at
//    most a few frames), and a gap in the sequence is answered with three
//    duplicate ACKs at once, which turns the peer's retransmission timer into
//    one round trip (signalGap in go/r2net/tcp.go).
//
//  - Sending is stop-and-wait: one segment in flight.  A browser sends a
//    ClientHello and a request line; it receives the page.
//
//  - Frames longer than 2048 bytes are dropped by the kernel, so the MSS we
//    ask for is 1024 and every frame buffer is 2048 bytes.
//

#include "net_r2.h"

#include <r2/net.hpp>
#include <r2/syscall.hpp>
#include <r2/time.hpp>

namespace web {

namespace {

const size_t FRAME = 2048;
const size_t ETH_HDR = 14;
const size_t IP_HDR = 20;
const size_t TCP_HDR = 20;
const size_t UDP_HDR = 8;

const uint16_t MSS_IN = 1024;
const size_t RX_CAP = 4096;
#ifndef WEB_RX_SEGMENTS
#define WEB_RX_SEGMENTS 2
#endif
//  The most we let the peer have in flight towards us.  The kernel moves one
//  frame per millisecond tick into a single buffer that the next frame
//  overwrites, and Memento's loop is regularly busy for longer than that ---
//  a repaint, a TLS record to decrypt.  Everything of a burst but its last
//  frame that arrives in such a stretch is gone, and every loss costs a round
//  trip at best and a retransmission timeout at worst.  So bursts are kept to
//  what a stall can lose at most one frame of.
const size_t RX_WINDOW = WEB_RX_SEGMENTS * MSS_IN;
const size_t TX_CAP = 4096;
const size_t SEG_MAX = 1024;

const uint64_t RTO_INITIAL = 600;
const uint64_t RTO_MAX = 4000;
const int MAX_RETRIES = 6;
const uint64_t FIN_WAIT_MS = 3000;
const uint64_t ARP_RETRY_MS = 50; // how soon to try again when the next hop's MAC was unknown

const uint16_t PORT_BASE = 47000; // eight local ports, reused round robin
const int PORT_COUNT = 8;
const uint16_t DNS_PORT_LOCAL = 47010;

const int MAX_CONNS = 6;

const uint8_t F_FIN = 0x01, F_SYN = 0x02, F_RST = 0x04, F_PSH = 0x08, F_ACK = 0x10;

//  QEMU's RTL8139 address, and the one this repository's tap is pinned to on
//  the host side (see run_iso_net, and the IRC window, which pre-seeds it for
//  the same reason: without the driver registration, ARP replies go elsewhere).
const uint8_t DEFAULT_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
const uint8_t TAP_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};

uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
bool seqLt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
bool seqLe(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

uint32_t sum16(const uint8_t *p, size_t n, uint32_t sum)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    return sum;
}
uint16_t fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint64_t rdtsc()
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void fmtIp(char *out, size_t cap, const uint8_t ip[4])
{
    out[0] = 0;
    for (int i = 0; i < 4; i++)
    {
        scatInt(out, ip[i], cap);
        if (i < 3)
            scat(out, ".", cap);
    }
}

struct Tcp
{
    enum State
    {
        FREE,
        SYN_SENT,
        ESTABLISHED,
        CLOSE_WAIT, // the peer has sent FIN
        FIN_WAIT,   // we closed; waiting for the peer to finish
    };
    State state;
    bool failed;
    const char *err;

    uint8_t rip[4];
    uint16_t rport, lport;

    uint32_t sndUna, sndNxt, rcvNxt;
    uint16_t peerMss;

    uint8_t *tx;
    size_t txLen;
    size_t inflight;    // bytes of tx sent and not yet acknowledged
    uint8_t pendFlags;  // SYN or FIN in flight
    bool finQueued;
    bool finAcked;
    bool peerFin;
    uint64_t sentAt, rto;
    int retries;

    uint8_t *rx;
    size_t rxLen;
    uint16_t advWnd;    // the window our last segment advertised

    uint32_t gapAt;     // rcvNxt when the last gap was signalled
    uint64_t gapTime;
    uint64_t closeBy;
};

struct DnsEntry
{
    char host[Url::HOST_CAP];
    uint8_t ip[4];
};

class Stack : public NetIf
{
public:
    void poll() override;
    int resolve(const char *host, uint8_t ip[4]) override;
    int connect(const uint8_t ip[4], uint16_t port) override;
    int status(int h) override;
    size_t send(int h, const uint8_t *data, size_t n) override;
    size_t recv(int h, uint8_t *data, size_t n) override;
    void close(int h) override;
    const char *lastError() override { return err_; }

    void describe(char *out, size_t cap);
    uint8_t dns_[4] = {1, 1, 1, 1};

    //  What the link has been doing, for about:net.  Losses show up as
    //  out-of-order segments (a frame before them never arrived) and as
    //  frames whose length disagrees with their IP header, which is what the
    //  kernel hands back when its one frame buffer was overwritten before
    //  the frame was read.  The longest pause between two polls while a
    //  connection was open says how long the stack was not looking.
    struct Stats
    {
        uint32_t frames, mangled, segments, outOfOrder, duplicates, retransmits;
        uint64_t bytes;
        uint64_t longestPause;
    } stats_ = {};
    uint64_t lastPoll_ = 0;
    uint8_t dns2_[4] = {8, 8, 8, 8};
    uint8_t gw_[4] = {0, 0, 0, 0};
    bool gwSet_ = false;

private:
    bool up_ = false;
    bool driver_ = false;
    uint8_t mac_[6];
    uint8_t ip_[4];
    uint8_t mask_[4] = {255, 255, 255, 0};
    uint16_t ipId_ = 0;
    int nextPort_ = 0;
    uint32_t boundPorts_ = 0;
    char err_[96] = {};

    uint8_t rxFrame_[FRAME];
    uint8_t txFrame_[FRAME];

    struct Arp
    {
        uint8_t ip[4];
        uint8_t mac[6];
        bool valid;
    } arp_[8] = {};
    uint64_t arpAskedAt_ = 0;
    uint8_t arpAsked_[4] = {};

    Tcp conns_[MAX_CONNS] = {};

    //  Name resolution, one name at a time.
    enum DnsPhase
    {
        DNS_IDLE,
        DNS_UDP1,
        DNS_UDP2,
        DNS_TCP,
        DNS_DONE,
        DNS_FAILED,
    } dnsPhase_ = DNS_IDLE;
    char dnsHost_[Url::HOST_CAP] = {};
    uint16_t dnsId_ = 0;
    uint64_t dnsSentAt_ = 0;
    uint8_t dnsAnswer_[4] = {};
    int dnsConn_ = -1;
    uint8_t dnsQuery_[300];
    size_t dnsQueryLen_ = 0;
    uint8_t dnsTcpBuf_[1024];
    size_t dnsTcpLen_ = 0;
    const char *dnsErr_ = nullptr;
    DnsEntry cache_[8] = {};
    int cacheNext_ = 0;

    bool bringUp();
    void setError(const char *what) { scopy(err_, what, sizeof(err_)); }

    bool onLink(const uint8_t ip[4]) const
    {
        for (int i = 0; i < 4; i++)
            if ((ip[i] & mask_[i]) != (ip_[i] & mask_[i]))
                return false;
        return true;
    }
    const uint8_t *nextHop(const uint8_t dst[4]) const { return onLink(dst) ? dst : gw_; }
    bool arpLookup(const uint8_t ip[4], uint8_t mac[6]) const;
    void arpRemember(const uint8_t ip[4], const uint8_t mac[6]);
    void arpAsk(const uint8_t ip[4]);
    void arpSend(uint16_t op, const uint8_t tip[4], const uint8_t tmac[6]);

    bool sendIp(const uint8_t dst[4], uint8_t proto, size_t payloadLen);
    void sendFrame(size_t len) { r2::raw_syscall(r2::Sys::SendPacket, 0x04, (int64_t)txFrame_, (int64_t)len); }

    void onFrame(size_t n);
    void onIcmp(const uint8_t *ip, size_t ipLen, const uint8_t *p, size_t n);
    void onUdp(const uint8_t src[4], const uint8_t *p, size_t n);
    void onTcp(const uint8_t src[4], const uint8_t *p, size_t n);

    int allocConn();
    void freeConn(Tcp &c);
    uint16_t allocPort();
    bool sendSeg(Tcp &c, uint8_t flags, uint32_t seq, const uint8_t *data, size_t len);
    void pump(Tcp &c, uint64_t now);
    void tick(Tcp &c, uint64_t now);
    void segment(Tcp &c, uint8_t flags, uint32_t seq, uint32_t ack, uint16_t wnd, const uint8_t *opt,
                 size_t optLen, const uint8_t *data, size_t len);
    void signalGap(Tcp &c, uint64_t now);
    void sendRst(const uint8_t dst[4], uint16_t lport, uint16_t rport, uint32_t seq, uint32_t ack, uint8_t flags,
                 size_t dataLen);

    bool dnsBuild(const char *host);
    bool dnsSendUdp(const uint8_t server[4]);
    bool dnsSent_ = false;
    bool dnsParse(const uint8_t *m, size_t n);
    void dnsStep(uint64_t now);
};

// ─── Bringing it up ──────────────────────────────────────────────────────────

bool Stack::bringUp()
{
    if (up_)
        return true;

    auto st = r2::net::status();
    if (!st)
    {
        setError("the kernel reports no network");
        return false;
    }
    driver_ = false;
    if (!st->driver_active)
    {
        //  Nobody drives the NIC: take the job, which also starts the card.
        if (r2::net::register_driver())
        {
            driver_ = true;
            st = r2::net::status();
        }
    }

    memcpy(mac_, DEFAULT_MAC, 6);
    memset(ip_, 0, 4);
    if (st)
    {
        bool zero = true;
        for (int i = 0; i < 6; i++)
            zero = zero && st->mac.octets[i] == 0;
        if (!zero)
            memcpy(mac_, st->mac.octets, 6);
        memcpy(ip_, st->ip.octets, 4);
    }
    if (!ip_[0])
    {
        r2::SysInfo si;
        memset(&si, 0, sizeof(si));
        if (r2::raw_syscall(r2::Sys::SysInfo, 0x01, (int64_t)&si) == 0 && si.ip_addr[0])
            memcpy(ip_, si.ip_addr, 4);
    }
    if (!ip_[0])
    {
        //  This repository's guest address; r2 has no DHCP client.
        ip_[0] = 10;
        ip_[1] = 3;
        ip_[2] = 4;
        ip_[3] = 2;
    }
    if (!gwSet_)
    {
        for (int i = 0; i < 4; i++)
            gw_[i] = ip_[i] & mask_[i];
        gw_[3] |= 1;
    }
    if (!driver_)
        arpRemember(gw_, TAP_MAC);

    ipId_ = (uint16_t)rdtsc();
    nextPort_ = (int)(rdtsc() % PORT_COUNT);
    up_ = true;
    return true;
}

void Stack::describe(char *out, size_t cap)
{
    char a[20];
    out[0] = 0;
    if (!up_)
    {
        scopy(out, "not started yet: the first page load brings the network up", cap);
        return;
    }
    fmtIp(a, sizeof(a), ip_);
    scat(out, a, cap);
    scat(out, " via ", cap);
    fmtIp(a, sizeof(a), gw_);
    scat(out, a, cap);
    scat(out, ", DNS ", cap);
    fmtIp(a, sizeof(a), dns_);
    scat(out, a, cap);
    scat(out, driver_ ? ", Ethernet driver" : ", bound ports (UDP to the driver)", cap);
    scat(out, ". Since start: ", cap);
    scatInt(out, (long)stats_.frames, cap);
    scat(out, " frames, ", cap);
    scatInt(out, (long)stats_.mangled, cap);
    scat(out, " mangled; ", cap);
    scatInt(out, (long)stats_.segments, cap);
    scat(out, " segments (", cap);
    scatInt(out, (long)(stats_.bytes / 1024), cap);
    scat(out, " KiB), ", cap);
    scatInt(out, (long)stats_.outOfOrder, cap);
    scat(out, " out of order, ", cap);
    scatInt(out, (long)stats_.duplicates, cap);
    scat(out, " repeated; ", cap);
    scatInt(out, (long)stats_.retransmits, cap);
    scat(out, " retransmits of ours; longest pause between polls ", cap);
    scatInt(out, (long)stats_.longestPause, cap);
    scat(out, " ms", cap);
}

// ─── Link layer ──────────────────────────────────────────────────────────────

bool Stack::arpLookup(const uint8_t ip[4], uint8_t mac[6]) const
{
    for (const Arp &a : arp_)
        if (a.valid && !memcmp(a.ip, ip, 4))
        {
            memcpy(mac, a.mac, 6);
            return true;
        }
    return false;
}

void Stack::arpRemember(const uint8_t ip[4], const uint8_t mac[6])
{
    if (!ip[0] && !ip[1] && !ip[2] && !ip[3])
        return;
    if ((mac[0] & 1) || !memcmp(mac, mac_, 6))
        return; // broadcast/multicast, or our own frames coming back
    int slot = -1;
    for (int i = 0; i < 8; i++)
        if (arp_[i].valid && !memcmp(arp_[i].ip, ip, 4))
            slot = i;
    for (int i = 0; slot < 0 && i < 8; i++)
        if (!arp_[i].valid)
            slot = i;
    if (slot < 0)
        slot = (int)(rdtsc() % 8);
    memcpy(arp_[slot].ip, ip, 4);
    memcpy(arp_[slot].mac, mac, 6);
    arp_[slot].valid = true;
}

void Stack::arpSend(uint16_t op, const uint8_t tip[4], const uint8_t tmac[6])
{
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t *f = txFrame_;
    memcpy(f, op == 1 ? bcast : tmac, 6);
    memcpy(f + 6, mac_, 6);
    put16(f + 12, 0x0806);
    uint8_t *a = f + ETH_HDR;
    put16(a, 1);
    put16(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, mac_, 6);
    memcpy(a + 14, ip_, 4);
    if (op == 1)
        memset(a + 18, 0, 6);
    else
        memcpy(a + 18, tmac, 6);
    memcpy(a + 24, tip, 4);
    sendFrame(ETH_HDR + 28);
}

void Stack::arpAsk(const uint8_t ip[4])
{
    uint64_t now = now_ms();
    if (!memcmp(arpAsked_, ip, 4) && now - arpAskedAt_ < ARP_RETRY_MS)
        return;
    memcpy(arpAsked_, ip, 4);
    arpAskedAt_ = now;
    arpSend(1, ip, nullptr);
}

//  The payload is already at txFrame_ + ETH_HDR + IP_HDR.  Returns false when
//  the next hop's MAC is not known yet; an ARP request has then gone out and
//  the caller's retransmission timer will try again.
bool Stack::sendIp(const uint8_t dst[4], uint8_t proto, size_t payloadLen)
{
    uint8_t dmac[6];
    const uint8_t *hop = nextHop(dst);
    if (!arpLookup(hop, dmac))
    {
        arpAsk(hop);
        return false;
    }
    uint8_t *f = txFrame_;
    memcpy(f, dmac, 6);
    memcpy(f + 6, mac_, 6);
    put16(f + 12, 0x0800);
    uint8_t *ip = f + ETH_HDR;
    size_t total = IP_HDR + payloadLen;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)total);
    put16(ip + 4, ++ipId_);
    ip[6] = 0x40; // don't fragment: nothing here reassembles
    ip[7] = 0;
    ip[8] = 64;
    ip[9] = proto;
    ip[10] = ip[11] = 0;
    memcpy(ip + 12, ip_, 4);
    memcpy(ip + 16, dst, 4);
    put16(ip + 10, fold(sum16(ip, IP_HDR, 0)));
    sendFrame(ETH_HDR + total);
    return true;
}

void Stack::poll()
{
    if (!bringUp())
        return;

    //  A burst is taken in one go, up to a limit, so that the timers below
    //  still run when the queue is busy.
    for (int k = 0; k < 16; k++)
    {
        int64_t n = r2::raw_syscall(r2::Sys::ReceivePort, 0, (int64_t)rxFrame_);
        if (n <= 0)
            break;
        if ((size_t)n > FRAME)
            continue;
        stats_.frames++;
        onFrame((size_t)n);
    }

    uint64_t now = now_ms();
    bool open = false;
    for (Tcp &c : conns_)
        if (c.state != Tcp::FREE)
        {
            open = true;
            tick(c, now);
        }
    if (open && lastPoll_ && now - lastPoll_ > stats_.longestPause)
        stats_.longestPause = now - lastPoll_;
    lastPoll_ = now;
    dnsStep(now);
}

void Stack::onFrame(size_t n)
{
    if (n < ETH_HDR)
        return;
    const uint8_t *f = rxFrame_;
    const uint8_t *smac = f + 6;
    if (!memcmp(smac, mac_, 6))
        return; // our own, looped back by the host bridge
    uint16_t type = get16(f + 12);

    if (type == 0x0806)
    {
        if (n < ETH_HDR + 28)
            return;
        const uint8_t *a = f + ETH_HDR;
        if (get16(a) != 1 || get16(a + 2) != 0x0800)
            return;
        arpRemember(a + 14, a + 8);
        if (get16(a + 6) == 1 && !memcmp(a + 24, ip_, 4))
        {
            uint8_t sip[4], smac2[6];
            memcpy(sip, a + 14, 4);
            memcpy(smac2, a + 8, 6);
            arpSend(2, sip, smac2);
        }
        return;
    }
    if (type != 0x0800)
        return;

    const uint8_t *ip = f + ETH_HDR;
    size_t avail = n - ETH_HDR;
    if (avail < IP_HDR || (ip[0] >> 4) != 4)
        return;
    size_t hl = (size_t)(ip[0] & 0x0F) * 4;
    size_t total = get16(ip + 2);
    if (hl < IP_HDR || total < hl || total > avail)
    {
        stats_.mangled++;
        return;
    }
    if ((ip[6] & 0x1F) || ip[7])
        return; // a fragment
    const uint8_t *src = ip + 12;
    const uint8_t *dst = ip + 16;
    if (memcmp(dst, ip_, 4) && !(dst[0] == 255 && dst[1] == 255 && dst[2] == 255 && dst[3] == 255))
        return;

    //  The sender's MAC comes free with every frame: for an address on the
    //  link it is that host's, for anything else it is the gateway's.
    uint8_t smacCopy[6];
    memcpy(smacCopy, smac, 6);
    arpRemember(onLink(src) ? src : gw_, smacCopy);

    uint8_t srcCopy[4];
    memcpy(srcCopy, src, 4);
    const uint8_t *p = ip + hl;
    size_t pl = total - hl;
    switch (ip[9])
    {
    case 1:
        onIcmp(ip, total, p, pl);
        break;
    case 6:
        onTcp(srcCopy, p, pl);
        break;
    case 17:
        onUdp(srcCopy, p, pl);
        break;
    }
}

void Stack::onIcmp(const uint8_t *ip, size_t ipLen, const uint8_t *p, size_t n)
{
    //  Echo request.  Only the driver is ever asked, but answering costs
    //  nothing either way.
    if (n < 8 || p[0] != 8 || n > FRAME - ETH_HDR - IP_HDR)
        return;
    (void)ipLen;
    uint8_t src[4];
    memcpy(src, ip + 12, 4);
    uint8_t *out = txFrame_ + ETH_HDR + IP_HDR;
    memmove(out, p, n);
    out[0] = 0;
    out[2] = out[3] = 0;
    put16(out + 2, fold(sum16(out, n, 0)));
    sendIp(src, 1, n);
}

// ─── UDP and DNS ─────────────────────────────────────────────────────────────

bool Stack::dnsBuild(const char *host)
{
    uint8_t *q = dnsQuery_;
    dnsId_ = (uint16_t)(rdtsc() ^ (rdtsc() >> 16));
    put16(q, dnsId_);
    put16(q + 2, 0x0100); // standard query, recursion desired
    put16(q + 4, 1);
    put16(q + 6, 0);
    put16(q + 8, 0);
    put16(q + 10, 0);
    size_t k = 12;
    const char *s = host;
    while (*s)
    {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (!l || l > 63 || k + l + 2 > sizeof(dnsQuery_) - 6)
            return false;
        q[k++] = (uint8_t)l;
        memcpy(q + k, s, l);
        k += l;
        s += l;
        if (*s == '.')
            s++;
    }
    q[k++] = 0;
    put16(q + k, 1); // A
    put16(q + k + 2, 1); // IN
    dnsQueryLen_ = k + 4;
    return true;
}

bool Stack::dnsSendUdp(const uint8_t server[4])
{
    uint8_t *u = txFrame_ + ETH_HDR + IP_HDR;
    size_t len = UDP_HDR + dnsQueryLen_;
    put16(u, DNS_PORT_LOCAL);
    put16(u + 2, 53);
    put16(u + 4, (uint16_t)len);
    put16(u + 6, 0);
    memcpy(u + UDP_HDR, dnsQuery_, dnsQueryLen_);
    uint32_t sum = sum16(ip_, 4, 0);
    sum = sum16(server, 4, sum);
    sum += 17 + (uint32_t)len;
    uint16_t ck = fold(sum16(u, len, sum));
    put16(u + 6, ck ? ck : 0xFFFF);
    dnsSent_ = sendIp(server, 17, len);
    dnsSentAt_ = now_ms();
    return dnsSent_;
}

static size_t skipName(const uint8_t *m, size_t n, size_t off)
{
    while (off < n)
    {
        uint8_t l = m[off];
        if ((l & 0xC0) == 0xC0)
            return off + 2;
        if (!l)
            return off + 1;
        off += 1 + (size_t)l;
    }
    return n + 1;
}

bool Stack::dnsParse(const uint8_t *m, size_t n)
{
    if (n < 12 || get16(m) != dnsId_ || !(m[2] & 0x80))
        return false;
    int rcode = m[3] & 0x0F;
    if (rcode)
    {
        dnsErr_ = rcode == 3 ? "no such host" : "the name server returned an error";
        dnsPhase_ = DNS_FAILED;
        return true;
    }
    size_t off = 12;
    for (int i = get16(m + 4); i > 0; i--)
        off = skipName(m, n, off) + 4;
    for (int i = get16(m + 6); i > 0 && off < n; i--)
    {
        off = skipName(m, n, off);
        if (off + 10 > n)
            break;
        uint16_t type = get16(m + off), cls = get16(m + off + 2), rdl = get16(m + off + 8);
        off += 10;
        if (off + rdl > n)
            break;
        if (type == 1 && cls == 1 && rdl == 4)
        {
            memcpy(dnsAnswer_, m + off, 4);
            dnsPhase_ = DNS_DONE;
            return true;
        }
        off += rdl;
    }
    dnsErr_ = "the name has no IPv4 address";
    dnsPhase_ = DNS_FAILED;
    return true;
}

void Stack::onUdp(const uint8_t src[4], const uint8_t *p, size_t n)
{
    if (n < UDP_HDR)
        return;
    if (get16(p + 2) != DNS_PORT_LOCAL || get16(p) != 53)
        return;
    if (dnsPhase_ != DNS_UDP1 && dnsPhase_ != DNS_UDP2)
        return;
    (void)src;
    size_t len = get16(p + 4);
    if (len < UDP_HDR || len > n)
        len = n;
    dnsParse(p + UDP_HDR, len - UDP_HDR);
}

void Stack::dnsStep(uint64_t now)
{
    switch (dnsPhase_)
    {
    case DNS_UDP1:
        //  The query may not have left at all: the gateway's MAC was not
        //  known yet, and an ARP request went instead.  Try again soon rather
        //  than waiting out the answer timeout for nothing.
        if (!dnsSent_ && now - dnsSentAt_ >= ARP_RETRY_MS)
        {
            dnsSendUdp(dns_);
            return;
        }
        if (now - dnsSentAt_ > 1500)
        {
            dnsPhase_ = DNS_UDP2;
            dnsSendUdp(dns2_);
        }
        return;
    case DNS_UDP2:
        if (!dnsSent_ && now - dnsSentAt_ >= ARP_RETRY_MS)
        {
            dnsSendUdp(dns2_);
            return;
        }
        if (now - dnsSentAt_ > 1500)
        {
            //  No answer over UDP: most likely another process holds the
            //  driver registration and has the replies.  TCP comes to us.
            dnsConn_ = connect(dns_, 53);
            dnsTcpLen_ = 0;
            dnsSentAt_ = now;
            if (dnsConn_ < 0)
            {
                dnsErr_ = "could not reach the name server";
                dnsPhase_ = DNS_FAILED;
                return;
            }
            uint8_t lenPrefix[2];
            put16(lenPrefix, (uint16_t)dnsQueryLen_);
            send(dnsConn_, lenPrefix, 2);
            send(dnsConn_, dnsQuery_, dnsQueryLen_);
            dnsPhase_ = DNS_TCP;
        }
        return;
    case DNS_TCP:
    {
        size_t got = recv(dnsConn_, dnsTcpBuf_ + dnsTcpLen_, sizeof(dnsTcpBuf_) - dnsTcpLen_);
        dnsTcpLen_ += got;
        if (dnsTcpLen_ >= 2 && dnsTcpLen_ >= 2 + (size_t)get16(dnsTcpBuf_))
        {
            if (!dnsParse(dnsTcpBuf_ + 2, get16(dnsTcpBuf_)))
            {
                dnsErr_ = "the name server's answer did not make sense";
                dnsPhase_ = DNS_FAILED;
            }
        }
        else if (status(dnsConn_) == FAILED || status(dnsConn_) == PEER_CLOSED || now - dnsSentAt_ > 6000)
        {
            dnsErr_ = "the name server did not answer";
            dnsPhase_ = DNS_FAILED;
        }
        if (dnsPhase_ != DNS_TCP)
        {
            close(dnsConn_);
            dnsConn_ = -1;
        }
        return;
    }
    default:
        return;
    }
}

int Stack::resolve(const char *host, uint8_t ip[4])
{
    if (!bringUp())
        return -1;
    if (parseIPv4(host, ip))
        return 1;
    for (const DnsEntry &e : cache_)
        if (e.host[0] && !strcmp(e.host, host))
        {
            memcpy(ip, e.ip, 4);
            return 1;
        }

    if (strcmp(dnsHost_, host) || dnsPhase_ == DNS_IDLE)
    {
        //  A new name.  Whatever was being looked up before is abandoned.
        if (dnsConn_ >= 0)
        {
            close(dnsConn_);
            dnsConn_ = -1;
        }
        scopy(dnsHost_, host, sizeof(dnsHost_));
        if (!dnsBuild(host))
        {
            setError("not a valid host name");
            dnsPhase_ = DNS_IDLE;
            return -1;
        }
        dnsErr_ = nullptr;
        dnsPhase_ = DNS_UDP1;
        dnsSendUdp(dns_);
        return 0;
    }

    switch (dnsPhase_)
    {
    case DNS_DONE:
    {
        memcpy(ip, dnsAnswer_, 4);
        DnsEntry &e = cache_[cacheNext_];
        cacheNext_ = (cacheNext_ + 1) % 8;
        scopy(e.host, host, sizeof(e.host));
        memcpy(e.ip, dnsAnswer_, 4);
        dnsPhase_ = DNS_IDLE;
        return 1;
    }
    case DNS_FAILED:
        setError(dnsErr_ ? dnsErr_ : "lookup failed");
        dnsPhase_ = DNS_IDLE;
        return -1;
    default:
        return 0;
    }
}

// ─── TCP ─────────────────────────────────────────────────────────────────────

int Stack::allocConn()
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns_[i].state == Tcp::FREE)
            return i;
    //  All taken: the oldest closing connection gives way.
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns_[i].state == Tcp::FIN_WAIT)
        {
            freeConn(conns_[i]);
            return i;
        }
    return -1;
}

void Stack::freeConn(Tcp &c)
{
    //  From the big pool: the frames that cross the syscall boundary are
    //  txFrame_ and rxFrame_, and these buffers are only ever copied through.
    if (c.tx)
        big_free(c.tx);
    if (c.rx)
        big_free(c.rx);
    c = Tcp{};
}

uint16_t Stack::allocPort()
{
    //  Round robin over a few ports, skipping any still in use: the kernel's
    //  port registry holds sixteen entries for the whole machine.
    for (int k = 0; k < PORT_COUNT; k++)
    {
        uint16_t port = (uint16_t)(PORT_BASE + (nextPort_ + k) % PORT_COUNT);
        bool used = false;
        for (const Tcp &c : conns_)
            if (c.state != Tcp::FREE && c.lport == port)
                used = true;
        if (used)
            continue;
        nextPort_ = (nextPort_ + k + 1) % PORT_COUNT;
        int bit = port - PORT_BASE;
        if (!driver_ && !(boundPorts_ & (1u << bit)))
        {
            if (!r2::net::bind_port(port))
                continue;
            boundPorts_ |= 1u << bit;
        }
        return port;
    }
    return 0;
}

void Stack::segment(Tcp &c, uint8_t flags, uint32_t seq, uint32_t ack, uint16_t wnd, const uint8_t *opt,
                    size_t optLen, const uint8_t *data, size_t len)
{
    uint8_t *t = txFrame_ + ETH_HDR + IP_HDR;
    size_t hl = TCP_HDR + optLen;
    put16(t, c.lport);
    put16(t + 2, c.rport);
    put32(t + 4, seq);
    put32(t + 8, ack);
    t[12] = (uint8_t)((hl / 4) << 4);
    t[13] = flags;
    put16(t + 14, wnd);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (optLen)
        memcpy(t + TCP_HDR, opt, optLen);
    if (len)
        memcpy(t + hl, data, len);
    uint32_t sum = sum16(ip_, 4, 0);
    sum = sum16(c.rip, 4, sum);
    sum += 6 + (uint32_t)(hl + len);
    put16(t + 16, fold(sum16(t, hl + len, sum)));
    sendIp(c.rip, 6, hl + len);
}

bool Stack::sendSeg(Tcp &c, uint8_t flags, uint32_t seq, const uint8_t *data, size_t len)
{
    //  The window is what is left of the receive buffer.  It is remembered,
    //  because recv() has to tell the peer when reading has reopened it.
    size_t room = RX_CAP - c.rxLen;
    uint16_t wnd = (uint16_t)(room < RX_WINDOW ? room : RX_WINDOW);
    c.advWnd = wnd;
    if (flags & F_SYN)
    {
        uint8_t opt[4] = {2, 4, (uint8_t)(MSS_IN >> 8), (uint8_t)MSS_IN};
        segment(c, flags, seq, c.rcvNxt, wnd, opt, 4, nullptr, 0);
    }
    else
        segment(c, flags, seq, c.rcvNxt, wnd, nullptr, 0, data, len);
    uint8_t dmac[6];
    return arpLookup(nextHop(c.rip), dmac);
}

//  Sends the next thing that is waiting, when nothing is in flight.
void Stack::pump(Tcp &c, uint64_t now)
{
    if (c.inflight || c.pendFlags || c.state == Tcp::SYN_SENT || c.failed)
        return;
    if (c.txLen)
    {
        size_t n = c.txLen;
        size_t seg = c.peerMss < SEG_MAX ? c.peerMss : SEG_MAX;
        if (n > seg)
            n = seg;
        c.inflight = n;
        bool sent = sendSeg(c, F_ACK | F_PSH, c.sndUna, c.tx, n);
        c.sndNxt = c.sndUna + (uint32_t)n;
        c.retries = 0;
        c.rto = RTO_INITIAL;
        //  An unresolved next hop: try again as soon as ARP has had a chance.
        c.sentAt = sent ? now : now - RTO_INITIAL + ARP_RETRY_MS;
        return;
    }
    if (c.finQueued && !c.finAcked && (c.state == Tcp::FIN_WAIT))
    {
        c.pendFlags = F_FIN;
        sendSeg(c, F_FIN | F_ACK, c.sndUna, nullptr, 0);
        c.sndNxt = c.sndUna + 1;
        c.sentAt = now;
        c.retries = 0;
        c.rto = RTO_INITIAL;
    }
}

void Stack::tick(Tcp &c, uint64_t now)
{
    if (c.state == Tcp::FIN_WAIT && now > c.closeBy)
    {
        freeConn(c);
        return;
    }
    if (c.failed)
        return;

    bool waiting = c.inflight || c.pendFlags || c.state == Tcp::SYN_SENT;
    if (waiting && now - c.sentAt >= c.rto)
    {
        if (++c.retries > MAX_RETRIES)
        {
            c.failed = true;
            c.err = c.state == Tcp::SYN_SENT ? "no answer from the server" : "the connection timed out";
            if (c.state == Tcp::FIN_WAIT)
                freeConn(c);
            return;
        }
        c.rto = c.rto * 2 > RTO_MAX ? RTO_MAX : c.rto * 2;
        stats_.retransmits++;
        bool sent;
        if (c.state == Tcp::SYN_SENT)
            sent = sendSeg(c, F_SYN, c.sndUna, nullptr, 0);
        else if (c.pendFlags & F_FIN)
            sent = sendSeg(c, F_FIN | F_ACK, c.sndUna, nullptr, 0);
        else
            sent = sendSeg(c, F_ACK | F_PSH, c.sndUna, c.tx, c.inflight);
        c.sentAt = sent ? now : now - c.rto + ARP_RETRY_MS;
        return;
    }
    pump(c, now);
}

void Stack::signalGap(Tcp &c, uint64_t now)
{
    //  Three at once, the fast-retransmit threshold --- but only once per hole
    //  and round trip, or every frame of a lost burst would repeat it.
    int n = (c.gapAt == c.rcvNxt && now - c.gapTime < 300) ? 1 : 3;
    c.gapAt = c.rcvNxt;
    c.gapTime = now;
    for (int i = 0; i < n; i++)
        sendSeg(c, F_ACK, c.sndNxt, nullptr, 0);
}

void Stack::sendRst(const uint8_t dst[4], uint16_t lport, uint16_t rport, uint32_t seq, uint32_t ack, uint8_t flags,
                    size_t dataLen)
{
    Tcp tmp = {};
    memcpy(tmp.rip, dst, 4);
    tmp.lport = lport;
    tmp.rport = rport;
    if (flags & F_ACK)
        segment(tmp, F_RST, ack, 0, 0, nullptr, 0, nullptr, 0);
    else
    {
        uint32_t used = (uint32_t)dataLen + ((flags & (F_SYN | F_FIN)) ? 1 : 0);
        segment(tmp, F_RST | F_ACK, 0, seq + used, 0, nullptr, 0, nullptr, 0);
    }
}

void Stack::onTcp(const uint8_t src[4], const uint8_t *p, size_t n)
{
    if (n < TCP_HDR)
        return;
    uint16_t sport = get16(p), dport = get16(p + 2);
    uint32_t seq = get32(p + 4), ack = get32(p + 8);
    size_t hl = (size_t)(p[12] >> 4) * 4;
    uint8_t flags = p[13];
    if (hl < TCP_HDR || hl > n)
        return;
    const uint8_t *data = p + hl;
    size_t len = n - hl;
    uint64_t now = now_ms();

    Tcp *cp = nullptr;
    for (Tcp &c : conns_)
        if (c.state != Tcp::FREE && c.lport == dport && c.rport == sport && !memcmp(c.rip, src, 4))
            cp = &c;
    if (!cp)
    {
        if (!(flags & F_RST) && dport >= PORT_BASE && dport < PORT_BASE + PORT_COUNT)
            sendRst(src, dport, sport, seq, ack, flags, len);
        return;
    }
    Tcp &c = *cp;

    if (flags & F_RST)
    {
        if (c.state == Tcp::FIN_WAIT)
        {
            freeConn(c);
            return;
        }
        c.failed = true;
        c.err = c.state == Tcp::SYN_SENT ? "connection refused" : "connection reset by the server";
        return;
    }

    if (c.state == Tcp::SYN_SENT)
    {
        if ((flags & (F_SYN | F_ACK)) != (F_SYN | F_ACK) || ack != c.sndUna + 1)
            return;
        c.rcvNxt = seq + 1;
        c.sndUna = c.sndNxt = ack;
        c.state = Tcp::ESTABLISHED;
        //  The peer's MSS, if it said.
        c.peerMss = 536;
        for (size_t o = TCP_HDR; o + 1 < hl;)
        {
            uint8_t kind = p[o];
            if (kind == 0)
                break;
            if (kind == 1)
            {
                o++;
                continue;
            }
            uint8_t ol = p[o + 1];
            if (ol < 2 || o + ol > hl)
                break;
            if (kind == 2 && ol == 4)
                c.peerMss = get16(p + o + 2);
            o += ol;
        }
        sendSeg(c, F_ACK, c.sndNxt, nullptr, 0);
        pump(c, now);
        return;
    }

    if ((flags & F_ACK) && seqLt(c.sndUna, ack) && seqLe(ack, c.sndNxt))
    {
        uint32_t acked = ack - c.sndUna;
        if (c.pendFlags & F_FIN)
        {
            if (ack == c.sndNxt)
            {
                c.pendFlags = 0;
                c.finAcked = true;
                c.sndUna = ack;
            }
        }
        else if (acked <= c.inflight)
        {
            memmove(c.tx, c.tx + acked, c.txLen - acked);
            c.txLen -= acked;
            c.inflight -= acked;
            c.sndUna = ack;
            c.retries = 0;
            c.rto = RTO_INITIAL;
        }
    }

    if (len)
    {
        if (c.peerFin || c.state == Tcp::FIN_WAIT)
            ; // nothing more is wanted; the ACK below is all it gets
        else if (seq == c.rcvNxt || (seqLt(seq, c.rcvNxt) && seqLt(c.rcvNxt, seq + (uint32_t)len)))
        {
            stats_.segments++;
            stats_.bytes += len;
            size_t skip = c.rcvNxt - seq;
            size_t take = len - skip;
            size_t room = RX_CAP - c.rxLen;
            if (take > room)
                take = room; // what does not fit is acknowledged later, after a read
            memcpy(c.rx + c.rxLen, data + skip, take);
            c.rxLen += take;
            c.rcvNxt += (uint32_t)take;
            sendSeg(c, F_ACK, c.sndNxt, nullptr, 0);
        }
        else if (seqLt(c.rcvNxt, seq))
        {
            stats_.outOfOrder++;
            signalGap(c, now);
        }
        else
        {
            stats_.duplicates++;
            sendSeg(c, F_ACK, c.sndNxt, nullptr, 0); // an old duplicate
        }
    }

    if (flags & F_FIN)
    {
        if (seq + (uint32_t)len == c.rcvNxt)
        {
            c.rcvNxt++;
            c.peerFin = true;
            sendSeg(c, F_ACK, c.sndNxt, nullptr, 0);
            if (c.state == Tcp::ESTABLISHED)
                c.state = Tcp::CLOSE_WAIT;
        }
        else if (seqLt(c.rcvNxt, seq + (uint32_t)len))
            signalGap(c, now);
    }

    if (c.state == Tcp::FIN_WAIT && c.finAcked && c.peerFin)
    {
        freeConn(c);
        return;
    }
    pump(c, now);
}

int Stack::connect(const uint8_t ip[4], uint16_t port)
{
    if (!bringUp())
        return -1;
    int h = allocConn();
    if (h < 0)
    {
        setError("too many connections");
        return -1;
    }
    uint16_t lport = allocPort();
    if (!lport)
    {
        setError("no local port could be bound");
        return -1;
    }
    Tcp &c = conns_[h];
    c = Tcp{};
    c.tx = (uint8_t *)big_alloc(TX_CAP);
    c.rx = (uint8_t *)big_alloc(RX_CAP);
    if (!c.tx || !c.rx)
    {
        freeConn(c);
        setError("out of memory");
        return -1;
    }
    memcpy(c.rip, ip, 4);
    c.rport = port;
    c.lport = lport;
    c.state = Tcp::SYN_SENT;
    //  A sequence number that moves between runs: the same few local ports
    //  come back every time, and the peer may still remember the last use.
    c.sndUna = c.sndNxt = (uint32_t)(r2::ticks() * 1000) ^ (uint32_t)rdtsc();
    c.peerMss = 536;
    c.rto = RTO_INITIAL;
    uint64_t now = now_ms();
    bool sent = sendSeg(c, F_SYN, c.sndUna, nullptr, 0);
    c.sndNxt = c.sndUna + 1;
    c.sentAt = sent ? now : now - RTO_INITIAL + ARP_RETRY_MS;
    return h;
}

int Stack::status(int h)
{
    if (h < 0 || h >= MAX_CONNS)
        return FAILED;
    Tcp &c = conns_[h];
    if (c.failed)
    {
        setError(c.err ? c.err : "connection failed");
        return FAILED;
    }
    switch (c.state)
    {
    case Tcp::SYN_SENT:
        return CONNECTING;
    case Tcp::ESTABLISHED:
        return OPEN;
    case Tcp::CLOSE_WAIT:
        return PEER_CLOSED;
    default:
        setError("connection closed");
        return FAILED;
    }
}

size_t Stack::send(int h, const uint8_t *data, size_t n)
{
    if (h < 0 || h >= MAX_CONNS)
        return 0;
    Tcp &c = conns_[h];
    if (c.failed || (c.state != Tcp::ESTABLISHED && c.state != Tcp::CLOSE_WAIT && c.state != Tcp::SYN_SENT))
        return 0;
    size_t room = TX_CAP - c.txLen;
    if (n > room)
        n = room;
    memcpy(c.tx + c.txLen, data, n);
    c.txLen += n;
    pump(c, now_ms());
    return n;
}

size_t Stack::recv(int h, uint8_t *data, size_t n)
{
    if (h < 0 || h >= MAX_CONNS)
        return 0;
    Tcp &c = conns_[h];
    if (!c.rx || !c.rxLen)
        return 0;
    if (n > c.rxLen)
        n = c.rxLen;
    memcpy(data, c.rx, n);
    memmove(c.rx, c.rx + n, c.rxLen - n);
    c.rxLen -= n;
    //  Every segment is acknowledged the moment it arrives, when the loader
    //  has not read it yet, so the windows those ACKs carry shrink: after two
    //  full segments the peer is told 4096 - 2 * 1460 = 1176, which is less
    //  than a segment, and a sender that avoids silly windows will not send
    //  into it.  It waits for its persist timer instead --- half a second,
    //  then seconds, doubling --- and a few KiB take tens of seconds.  So once
    //  reading has reopened a real part of the window, the peer hears of it
    //  (RFC 1122, 4.2.3.3, the receiver's half of SWS avoidance).
    size_t open = RX_CAP - c.rxLen;
    if (open > RX_WINDOW)
        open = RX_WINDOW;
    if (!c.failed && c.state == Tcp::ESTABLISHED && open >= (size_t)c.advWnd + MSS_IN)
        sendSeg(c, F_ACK, c.sndNxt, nullptr, 0);
    return n;
}

void Stack::close(int h)
{
    if (h < 0 || h >= MAX_CONNS)
        return;
    Tcp &c = conns_[h];
    if (c.state == Tcp::FREE)
        return;
    if (c.failed || c.state == Tcp::SYN_SENT)
    {
        if (!c.failed)
            sendSeg(c, F_RST, c.sndUna, nullptr, 0);
        freeConn(c);
        return;
    }
    //  Whatever is still unsent goes first, then the FIN; the slot is taken
    //  back when the peer has finished, or after FIN_WAIT_MS regardless.
    c.state = Tcp::FIN_WAIT;
    c.finQueued = true;
    c.rxLen = 0;
    c.closeBy = now_ms() + FIN_WAIT_MS;
    pump(c, now_ms());
}

Stack g_stack;

} // namespace

NetIf &r2Net() { return g_stack; }

void r2NetDescribe(char *out, size_t cap) { g_stack.describe(out, cap); }

bool r2NetSetDns(const char *ip)
{
    uint8_t a[4];
    if (!parseIPv4(ip, a))
        return false;
    memcpy(g_stack.dns_, a, 4);
    memcpy(g_stack.dns2_, a, 4);
    return true;
}

bool r2NetSetGateway(const char *ip)
{
    uint8_t a[4];
    if (!parseIPv4(ip, a))
        return false;
    memcpy(g_stack.gw_, a, 4);
    g_stack.gwSet_ = true;
    return true;
}

} // namespace web
