//
// nsk — see nsk.h.
//

#include "nsk.h"
#include "netmux.h"

#include <r2/libc.hpp>
#include <r2/net.hpp>
#include <r2/syscall.hpp>
#include <r2/time.hpp>

namespace {

//  Probes sent per step(): c/nsk's pace, which yields to the NIC after this
//  many.  A step is one pass of the window's idle loop.
const int PACE = 16;
const int ECHO_DATA = 8;
const size_t ETH_HDR = 14;

const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t ZERO_MAC[6] = {0, 0, 0, 0, 0, 0};

uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

uint32_t toU32(const uint8_t ip[4])
{
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3];
}

void fromU32(uint32_t v, uint8_t ip[4])
{
    ip[0] = (uint8_t)(v >> 24);
    ip[1] = (uint8_t)(v >> 16);
    ip[2] = (uint8_t)(v >> 8);
    ip[3] = (uint8_t)v;
}

bool zeroIp(const uint8_t ip[4]) { return !ip[0] && !ip[1] && !ip[2] && !ip[3]; }

uint16_t cksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += get16(p + i);
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

//  "10.3.4.2" into ip; the character past it, or null.
const char *parseIp(const char *s, uint8_t ip[4])
{
    for (int oct = 0; oct < 4; oct++)
    {
        unsigned v = 0;
        if (*s < '0' || *s > '9')
            return nullptr;
        while (*s >= '0' && *s <= '9' && v <= 255)
            v = v * 10 + (unsigned)(*s++ - '0');
        if (v > 255)
            return nullptr;
        ip[oct] = (uint8_t)v;
        if (oct < 3 && *s++ != '.')
            return nullptr;
    }
    return s;
}

//  "10.3.4.0/30"; a bare address is /32.  Spaces around it are allowed.
bool parseCidr(const char *s, uint8_t ip[4], int *prefix)
{
    while (*s == ' ')
        s++;
    s = parseIp(s, ip);
    if (!s)
        return false;
    *prefix = 32;
    if (*s == '/')
    {
        s++;
        if (*s < '0' || *s > '9')
            return false;
        int v = 0;
        while (*s >= '0' && *s <= '9' && v <= 32)
            v = v * 10 + (*s++ - '0');
        if (v > 32)
            return false;
        *prefix = v;
    }
    while (*s == ' ')
        s++;
    return *s == 0;
}

char *putU(char *p, uint32_t v)
{
    char t[10];
    int n = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        *p++ = t[--n];
    return p;
}

char *putIp(char *p, const uint8_t ip[4])
{
    for (int i = 0; i < 4; i++)
    {
        p = putU(p, ip[i]);
        if (i < 3)
            *p++ = '.';
    }
    return p;
}

void scopy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    while (src[i] && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

//  The address and MAC as the driver published them (syscall 0x3d), or as
//  the kernel reports them (0x38), or as sysinfo has the address.
struct Local
{
    uint8_t ip[4] = {};
    uint8_t mask[4] = {};
    uint8_t mac[6] = {};
    uint8_t gw[4] = {};
    uint8_t gwMac[6] = {};
    bool gwMacKnown = false;
};

Local readLocal()
{
    Local l;
    auto cfg = r2::net::config();
    if (cfg)
    {
        memcpy(l.ip, cfg->ip.octets, 4);
        memcpy(l.mask, cfg->netmask.octets, 4);
        memcpy(l.mac, cfg->mac.octets, 6);
        memcpy(l.gw, cfg->gateway.octets, 4);
        if (cfg->gateway_mac_known)
        {
            memcpy(l.gwMac, cfg->gateway_mac.octets, 6);
            l.gwMacKnown = true;
        }
    }
    auto st = r2::net::status();
    if (st)
    {
        if (!memcmp(l.mac, ZERO_MAC, 6))
            memcpy(l.mac, st->mac.octets, 6);
        if (zeroIp(l.ip))
            memcpy(l.ip, st->ip.octets, 4);
    }
    if (zeroIp(l.ip))
    {
        r2::SysInfo si;
        memset(&si, 0, sizeof(si));
        if (r2::raw_syscall(r2::Sys::SysInfo, 0x01, (int64_t)&si) == 0)
            memcpy(l.ip, si.ip_addr, 4);
    }
    return l;
}

int prefixOf(const uint8_t mask[4])
{
    uint32_t m = toU32(mask);
    int n = 0;
    while (n < 32 && (m & (0x80000000u >> n)))
        n++;
    return n;
}

} // namespace

// ─── Starting and stopping ───────────────────────────────────────────────────

void HostScan::suggest(char *cidr)
{
    Local l = readLocal();
    if (zeroIp(l.ip))
    {
        //  Nothing published yet: this repository's guest network.
        scopy(cidr, "10.3.4.0/24", 20);
        return;
    }
    //  No netmask published means c/nsk's guess: the local /24.  A wider one
    //  is more than a scan can take (1024 addresses), so /24 again.
    int prefix = zeroIp(l.mask) ? 24 : prefixOf(l.mask);
    if (prefix < 22)
        prefix = 24;
    uint32_t mask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
    uint8_t net[4];
    fromU32(toU32(l.ip) & mask, net);
    char *p = putIp(cidr, net);
    *p++ = '/';
    p = putU(p, (uint32_t)prefix);
    *p = 0;
}

void HostScan::fail(const char *why)
{
    scopy(error_, why, sizeof(error_));
    state_ = FAILED;
}

bool HostScan::start(const char *cidr)
{
    stop();
    error_[0] = 0;
    n_ = up_ = 0;

    uint8_t base[4];
    int prefix = 32;
    if (!parseCidr(cidr, base, &prefix))
    {
        fail("not a subnet: try 10.3.4.0/24");
        return false;
    }
    uint32_t mask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
    uint32_t network = toU32(base) & mask;
    uint32_t bcast = network | ~mask;
    uint32_t first = network, last = bcast;
    //  /31 and /32 have no network or broadcast address to skip.
    if (prefix <= 30)
    {
        first++;
        last--;
    }
    if (last - first + 1 > (uint32_t)MAX_TARGETS)
    {
        fail("too many addresses: /22 at most");
        return false;
    }

    //  Replies go to the Ethernet driver.  If that is somebody else, ARP
    //  replies still come here as long as a TCP port is bound (nsk.h).
    arpOnly_ = false;
    if (!netmux_take_driver())
    {
        static bool bound = false;
        auto st = r2::net::status();
        if (!st || !st->driver_active)
        {
            fail("no network card");
            return false;
        }
        if (!bound && !r2::net::bind_port(NSK_PORT))
        {
            fail("eth.elf holds the NIC, no port free");
            return false;
        }
        bound = true;
        arpOnly_ = true;
    }

    Local l = readLocal();
    if (zeroIp(l.ip))
    {
        //  Nothing published: this repository's guest address, as the
        //  browser assumes too.
        l.ip[0] = 10;
        l.ip[1] = 3;
        l.ip[2] = 4;
        l.ip[3] = 2;
    }
    memcpy(ip_, l.ip, 4);
    memcpy(mac_, l.mac, 6);
    memcpy(gw_, l.gw, 4);
    memcpy(gwMac_, l.gwMac, 6);
    gwMacKnown_ = l.gwMacKnown;

    base_ = first;
    n_ = (int)(last - first + 1);
    memset(hosts_, 0, sizeof(Host) * (size_t)n_);
    int self = indexOf(ip_);
    if (self >= 0 && !zeroIp(ip_))
    {
        hosts_[self].flags = F_SELF | F_MAC;
        memcpy(hosts_[self].mac, mac_, 6);
        up_ = 1;
    }

    //  ARP reaches the link only.  c/nsk's rule: the range is on it when it
    //  holds our address, or lies inside the local subnet --- the published
    //  netmask's, or a /24 when there is none.
    uint32_t local = toU32(ip_);
    uint32_t lm = zeroIp(l.mask) ? 0xFFFFFF00u : toU32(l.mask);
    onLink_ = (local & mask) == network || ((first & lm) == (local & lm) && (last & lm) == (local & lm));
    if (arpOnly_ && !onLink_)
    {
        fail("off the link: eth.elf gets the pings");
        return false;
    }
    doArp_ = onLink_;
    doIcmp_ = !arpOnly_;

    scopy(cidr_, cidr, sizeof(cidr_));
    t0_ = r2::ticks();
    next_ = 0;
    state_ = doArp_ ? ARP_SWEEP : ICMP_SWEEP;
    return true;
}

void HostScan::stop()
{
    if (busy())
    {
        doneMs_ = r2::ticks();
        state_ = DONE;
    }
}

// ─── Frames ──────────────────────────────────────────────────────────────────

void HostScan::sendFrame(size_t len) { r2::raw_syscall(r2::Sys::SendPacket, 0x04, (int64_t)tx_, (int64_t)len); }

void HostScan::sendArp(uint16_t op, const uint8_t tip[4], const uint8_t tmac[6])
{
    uint8_t *f = tx_;
    memcpy(f, op == 1 ? BCAST : tmac, 6);
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
    memcpy(a + 18, op == 1 ? ZERO_MAC : tmac, 6);
    memcpy(a + 24, tip, 4);
    sendFrame(ETH_HDR + 28);
}

void HostScan::sendEcho(int idx)
{
    Host &h = hosts_[idx];
    uint8_t dip[4];
    addressOf(idx, dip);

    //  Unicast to a MAC the ARP sweep found; off the link, to the gateway when
    //  its MAC is known; otherwise broadcast, and the host bridge routes it
    //  (c/nsk does the same).
    const uint8_t *dmac = BCAST;
    if (h.flags & F_MAC)
        dmac = h.mac;
    else if (!onLink_ && gwMacKnown_)
        dmac = gwMac_;

    const size_t icmpLen = 8 + ECHO_DATA;
    uint8_t *f = tx_;
    memset(f, 0, ETH_HDR + 20 + icmpLen);
    memcpy(f, dmac, 6);
    memcpy(f + 6, mac_, 6);
    put16(f + 12, 0x0800);

    uint8_t *ip = f + ETH_HDR;
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(20 + icmpLen));
    put16(ip + 4, (uint16_t)(0x4e00 + (idx & 0xFF)));
    ip[8] = 64;
    ip[9] = 1;
    memcpy(ip + 12, ip_, 4);
    memcpy(ip + 16, dip, 4);
    put16(ip + 10, cksum(ip, 20));

    uint8_t *icmp = ip + 20;
    icmp[0] = 8; // echo request
    put16(icmp + 4, NETMUX_NSK_ICMP_ID);
    put16(icmp + 6, (uint16_t)idx);
    memcpy(icmp + 8, "nsk-r2!\n", ECHO_DATA);
    put16(icmp + 2, cksum(icmp, icmpLen));

    h.sentMs = (uint32_t)r2::ticks();
    h.flags = (uint8_t)((h.flags | F_SENT) & ~F_RTT);
    sendFrame(ETH_HDR + 20 + icmpLen);
}

int HostScan::indexOf(const uint8_t ip[4]) const
{
    uint32_t v = toU32(ip);
    if (v < base_ || v - base_ >= (uint32_t)n_)
        return -1;
    return (int)(v - base_);
}

void HostScan::addressOf(int i, uint8_t ip[4]) const { fromU32(base_ + (uint32_t)i, ip); }

void HostScan::markUp(int idx, bool viaArp, const uint8_t *mac, bool solicited)
{
    Host &h = hosts_[idx];
    bool first = !(h.flags & (F_ARP | F_ICMP | F_SELF));
    h.flags |= viaArp ? F_ARP : F_ICMP;
    if (mac && !(h.flags & F_MAC))
    {
        memcpy(h.mac, mac, 6);
        h.flags |= F_MAC;
    }
    //  Only a reply to one of our own probes carries a round trip.
    if (solicited && (h.flags & F_SENT) && !(h.flags & F_RTT))
    {
        uint32_t now = (uint32_t)r2::ticks();
        h.rttMs = now >= h.sentMs ? now - h.sentMs : 0;
        h.flags |= F_RTT;
    }
    if (first)
        up_++;
}

bool HostScan::onFrame(const uint8_t *f, size_t n)
{
    if (n < ETH_HDR || !memcmp(f + 6, mac_, 6))
        return false; // our own probe, looped back by the host bridge
    uint16_t type = get16(f + 12);

    if (type == 0x0806)
    {
        if (n < ETH_HDR + 28)
            return false;
        const uint8_t *a = f + ETH_HDR;
        if (get16(a) != 1 || get16(a + 2) != 0x0800)
            return false;
        uint16_t op = get16(a + 6);
        int idx = indexOf(a + 14);
        //  A reply is an answer; a request is a host asking, alive as well.
        //  The browser answers requests for our address when it is running,
        //  but it may not be: while the scan holds the NIC, so does this.
        if (op == 1 && !memcmp(a + 24, ip_, 4) && !zeroIp(ip_) && netmux_is_driver())
        {
            uint8_t sip[4], smac[6];
            memcpy(sip, a + 14, 4);
            memcpy(smac, a + 8, 6);
            sendArp(2, sip, smac);
        }
        if (idx < 0 || (op != 1 && op != 2))
            return false;
        markUp(idx, true, a + 8, op == 2);
        return true;
    }
    if (type != 0x0800 || n < ETH_HDR + 20)
        return false;

    const uint8_t *ip = f + ETH_HDR;
    size_t hl = (size_t)(ip[0] & 0x0F) * 4;
    size_t total = get16(ip + 2);
    if ((ip[0] >> 4) != 4 || hl < 20 || total < hl + 8 || total > n - ETH_HDR || ip[9] != 1)
        return false;
    const uint8_t *icmp = ip + hl;
    //  netmux gives this client echo replies with its id and nothing else.
    if (icmp[0] != 0 || get16(icmp + 4) != NETMUX_NSK_ICMP_ID)
        return false;
    int idx = indexOf(ip + 12);
    if (idx < 0)
        return false;
    //  Off the link the frame's sender is the gateway, not the host.
    markUp(idx, false, onLink_ ? f + 6 : nullptr, true);
    return true;
}

// ─── The sweep ───────────────────────────────────────────────────────────────

bool HostScan::step()
{
    if (!busy())
        return false;

    bool changed = false;
    for (int k = 0; k < 64; k++)
    {
        int64_t n = netmux_pull(NETMUX_NSK, rx_, sizeof(rx_));
        if (n <= 0)
            break;
        changed |= onFrame(rx_, (size_t)n);
    }

    uint64_t now = r2::ticks();
    switch (state_)
    {
    case ARP_SWEEP:
    case ICMP_SWEEP:
    {
        bool arp = state_ == ARP_SWEEP;
        for (int sent = 0; next_ < n_ && sent < PACE; next_++)
        {
            Host &h = hosts_[next_];
            if (h.flags & F_SELF)
                continue;
            //  The echo sweep asks only those the ARP sweep did not hear.
            if (!arp && (h.flags & (F_ARP | F_ICMP)))
                continue;
            if (arp)
            {
                uint8_t dip[4];
                addressOf(next_, dip);
                h.sentMs = (uint32_t)now;
                h.flags |= F_SENT;
                sendArp(1, dip, ZERO_MAC);
            }
            else
                sendEcho(next_);
            sent++;
        }
        if (next_ >= n_)
        {
            state_ = arp ? ARP_WAIT : ICMP_WAIT;
            waitUntil_ = now + TIMEOUT_MS;
        }
        return true; // the progress moved
    }
    case ARP_WAIT:
    case ICMP_WAIT:
        if (now < waitUntil_)
            return changed;
        if (state_ == ARP_WAIT && doIcmp_)
        {
            next_ = 0;
            state_ = ICMP_SWEEP;
        }
        else
        {
            doneMs_ = now;
            state_ = DONE;
        }
        return true;
    default:
        return changed;
    }
}

void HostScan::describe(char *out, size_t cap) const
{
    char buf[64];
    char *p = buf;
    switch (state_)
    {
    case IDLE:
        scopy(out, "Enter a subnet and press Scan.", cap);
        return;
    case FAILED:
        scopy(out, error_, cap);
        return;
    case ARP_SWEEP:
    case ICMP_SWEEP:
    {
        const char *what = state_ == ARP_SWEEP ? "arp " : "ping ";
        while (*what)
            *p++ = *what++;
        p = putU(p, (uint32_t)next_);
        *p++ = '/';
        p = putU(p, (uint32_t)n_);
        break;
    }
    case ARP_WAIT:
    case ICMP_WAIT:
    {
        const char *what = state_ == ARP_WAIT ? "arp: listening" : "ping: listening";
        while (*what)
            *p++ = *what++;
        break;
    }
    case DONE:
        if (arpOnly_)
        {
            const char *what = "arp only: ";
            while (*what)
                *p++ = *what++;
        }
        break;
    }
    const char *mid = state_ == DONE ? "" : ", ";
    while (*mid)
        *p++ = *mid++;
    p = putU(p, (uint32_t)up_);
    const char *upOf = " up / ";
    while (*upOf)
        *p++ = *upOf++;
    p = putU(p, (uint32_t)n_);
    if (state_ == DONE)
    {
        const char *in = " in ";
        while (*in)
            *p++ = *in++;
        p = putU(p, (uint32_t)(doneMs_ - t0_));
        *p++ = ' ';
        *p++ = 'm';
        *p++ = 's';
    }
    *p = 0;
    scopy(out, buf, cap);
}
