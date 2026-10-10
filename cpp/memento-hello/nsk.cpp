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
const int ECHO_DATA = 8;

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
    if (!cap) return;
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

namespace {
NetLink *linkOwner = nullptr;
uint16_t scanGeneration = 0, pingSequence = 0, traceGeneration = 0;
uint32_t get32(const uint8_t *p) { return ((uint32_t)get16(p) << 16) | get16(p + 2); }
void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }

struct Packet
{
    const uint8_t *ip, *body;
    size_t len;
};
bool ipv4(const uint8_t *f, size_t n, Packet &p)
{
    if (n < 34 || get16(f + 12) != 0x0800) return false;
    p.ip = f + 14;
    size_t hl = (p.ip[0] & 15) * 4, total = get16(p.ip + 2);
    if ((p.ip[0] >> 4) != 4 || hl < 20 || total < hl || total > n - 14 ||
        (get16(p.ip + 6) & 0x3fff)) return false;
    p.body = p.ip + hl;
    p.len = total - hl;
    return true;
}
bool arp(const uint8_t *f, size_t n)
{
    return n >= 42 && get16(f + 12) == 0x0806 && get16(f + 14) == 1 &&
           get16(f + 16) == 0x0800 && f[18] == 6 && f[19] == 4;
}
// Router errors quote only the original IP header and eight payload bytes.
bool echoError(const Packet &p, const uint8_t local[4], const uint8_t dst[4], uint16_t id, uint16_t &seq)
{
    if (p.ip[9] != 1 || p.len < 36 || (p.body[0] != 3 && p.body[0] != 11)) return false;
    const uint8_t *inner = p.body + 8;
    size_t hl = (inner[0] & 15) * 4;
    if ((inner[0] >> 4) != 4 || hl < 20 || p.len < 8 + hl + 8 || inner[9] != 1 ||
        memcmp(inner + 12, local, 4) || memcmp(inner + 16, dst, 4) || inner[hl] != 8 ||
        get16(inner + hl + 4) != id) return false;
    seq = get16(inner + hl + 6);
    return true;
}
}

bool NetLink::open(char *why, size_t cap)
{
    if (open_) return true;
    if (linkOwner && linkOwner != this)
    {
        scopy(why, "Tools are running in another Network window.", cap);
        return false;
    }
    driver_ = netmux_take_driver();
    auto st = r2::net::status();
    if (!driver_ && (!st || !st->driver_active))
    {
        scopy(why, "No network card.", cap);
        return false;
    }
    if (!driver_ && !bound_)
    {
        if (!r2::net::bind_port(PORT))
        {
            scopy(why, "No scan port available.", cap);
            return false;
        }
        bound_ = true;
        netmux_port_bound(PORT, "network tools");
    }
    linkOwner = this;
    netmux_forget(NETMUX_NSK);
    netmux_claim(NETMUX_NSK, PORT, PORT);
    refresh();
    open_ = true;
    return true;
}

void NetLink::close()
{
    if (bound_)
    {
        if (r2::raw_syscall(r2::Sys::NetRegister, PORT, 1) != 0) return;
        bound_ = false;
        netmux_port_released(PORT);
    }
    if (linkOwner == this)
    {
        netmux_forget(NETMUX_NSK);
        linkOwner = nullptr;
    }
    open_ = false;
}

void NetLink::refresh()
{
    Local l = readLocal();
    if (zeroIp(l.ip)) { l.ip[0] = 10; l.ip[1] = 3; l.ip[2] = 4; l.ip[3] = 2; }
    if (zeroIp(l.mask)) { l.mask[0] = l.mask[1] = l.mask[2] = 255; l.mask[3] = 0; }
    // Match the browser's legacy fallback when no driver has published a route.
    if (zeroIp(l.gw)) fromU32((toU32(l.ip) & toU32(l.mask)) | 1u, l.gw);
    memcpy(ip_, l.ip, 4); memcpy(mask_, l.mask, 4); memcpy(mac_, l.mac, 6);
    memcpy(gw_, l.gw, 4); memcpy(gwMac_, l.gwMac, 6); gwMacKnown_ = l.gwMacKnown;
}
bool NetLink::onLink(const uint8_t ip[4]) const
{
    uint32_t m = zeroIp(mask_) ? 0xffffff00u : toU32(mask_);
    return (toU32(ip) & m) == (toU32(ip_) & m);
}
void NetLink::nextHop(const uint8_t dst[4], uint8_t hop[4]) const
{
    memcpy(hop, onLink(dst) || zeroIp(gw_) ? dst : gw_, 4);
}
bool NetLink::knownMac(const uint8_t ip[4], uint8_t mac[6]) const
{
    if (gwMacKnown_ && !memcmp(ip, gw_, 4)) { memcpy(mac, gwMac_, 6); return true; }
    for (const Seen &s : seen_)
        if (s.used && !memcmp(s.ip, ip, 4)) { memcpy(mac, s.mac, 6); return true; }
    return false;
}
void NetLink::learn(const uint8_t ip[4], const uint8_t mac[6])
{
    if (zeroIp(ip)) return;
    for (Seen &s : seen_)
        if (s.used && !memcmp(s.ip, ip, 4)) { memcpy(s.mac, mac, 6); return; }
    Seen &s = seen_[seenNext_++ % 16];
    memcpy(s.ip, ip, 4); memcpy(s.mac, mac, 6); s.used = true;
    if (seenNext_ == 16) seenNext_ = 0;
}
void NetLink::onFrame(const uint8_t *f, size_t n)
{
    if (!arp(f, n) || !memcmp(f + 6, mac_, 6)) return;
    uint16_t op = get16(f + 20);
    if (op != 1 && op != 2) return;
    learn(f + 28, f + 22);
    if (driver_ && op == 1 && !memcmp(f + 38, ip_, 4)) sendArp(2, f + 28, f + 22);
}
void NetLink::sendArp(uint16_t op, const uint8_t tip[4], const uint8_t tmac[6])
{
    uint8_t *f = tx_;
    memcpy(f, op == 1 ? BCAST : tmac, 6); memcpy(f + 6, mac_, 6); put16(f + 12, 0x0806);
    uint8_t *a = f + 14;
    put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, op);
    memcpy(a + 8, mac_, 6); memcpy(a + 14, ip_, 4);
    memcpy(a + 18, op == 1 ? ZERO_MAC : tmac, 6); memcpy(a + 24, tip, 4);
    netmux_send(f, 42);
}
void NetLink::ipHeader(uint8_t *ip, const uint8_t dip[4], uint8_t proto, uint16_t len, uint8_t ttl)
{
    memset(ip, 0, 20); ip[0] = 0x45; put16(ip + 2, len); put16(ip + 4, ++ipId_);
    ip[8] = ttl; ip[9] = proto; memcpy(ip + 12, ip_, 4); memcpy(ip + 16, dip, 4);
    put16(ip + 10, cksum(ip, 20));
}
void NetLink::sendEcho(const uint8_t dmac[6], const uint8_t dip[4], uint16_t id, uint16_t seq, uint8_t ttl)
{
    memcpy(tx_, dmac, 6); memcpy(tx_ + 6, mac_, 6); put16(tx_ + 12, 0x0800);
    ipHeader(tx_ + 14, dip, 1, 36, ttl);
    uint8_t *icmp = tx_ + 34;
    memset(icmp, 0, 8); icmp[0] = 8; put16(icmp + 4, id); put16(icmp + 6, seq);
    memcpy(icmp + 8, "nsk-r2!\n", ECHO_DATA); put16(icmp + 2, cksum(icmp, 16));
    netmux_send(tx_, 50);
}
void NetLink::sendTcp(const uint8_t dmac[6], const uint8_t dip[4], uint16_t sport, uint16_t dport,
                      uint32_t seq, uint32_t ack, uint8_t flags)
{
    memcpy(tx_, dmac, 6); memcpy(tx_ + 6, mac_, 6); put16(tx_ + 12, 0x0800);
    ipHeader(tx_ + 14, dip, 6, 40, 64);
    uint8_t *tcp = tx_ + 34;
    memset(tcp, 0, 20); put16(tcp, sport); put16(tcp + 2, dport);
    put32(tcp + 4, seq); put32(tcp + 8, ack); tcp[12] = 0x50; tcp[13] = flags;
    put16(tcp + 14, 1024);
    uint8_t pseudo[32];
    memcpy(pseudo, ip_, 4); memcpy(pseudo + 4, dip, 4); pseudo[8] = 0; pseudo[9] = 6;
    put16(pseudo + 10, 20); memcpy(pseudo + 12, tcp, 20); put16(tcp + 16, cksum(pseudo, 32));
    netmux_send(tx_, 54);
}
bool NetLink::parseAddress(const char *s, uint8_t ip[4])
{
    while (*s == ' ') s++;
    const char *end = parseIp(s, ip);
    if (!end) return false;
    while (*end == ' ') end++;
    return !*end;
}
void NetLink::formatAddress(const uint8_t ip[4], char *out) { *putIp(out, ip) = 0; }

void HostScan::suggest(char *cidr)
{
    Local l = readLocal();
    if (zeroIp(l.ip)) { scopy(cidr, "10.3.4.0/24", 20); return; }
    int prefix = zeroIp(l.mask) ? 24 : prefixOf(l.mask);
    if (prefix < 22) prefix = 22;
    uint8_t net[4]; fromU32(toU32(l.ip) & (0xffffffffu << (32 - prefix)), net);
    char *p = putIp(cidr, net); *p++ = '/'; *putU(p, (uint32_t)prefix) = 0;
}
void HostScan::fail(const char *why) { scopy(error_, why, sizeof(error_)); state_ = FAILED; }
bool HostScan::start(NetLink &link, const char *cidr)
{
    stop(); error_[0] = 0; n_ = up_ = 0; doneMs_ = 0;
    uint8_t base[4]; int prefix;
    if (!parseCidr(cidr, base, &prefix)) { fail("Not a subnet: try 10.3.4.0/24."); return false; }
    if (prefix < 22) { fail("Too many addresses: /22 at most."); return false; }
    uint32_t mask = 0xffffffffu << (32 - prefix), network = toU32(base) & mask;
    uint32_t first = network, last = network | ~mask;
    if (prefix <= 30) { first++; last--; }
    if (!link.open(error_, sizeof(error_))) { state_ = FAILED; return false; }
    link_ = &link; arpOnly_ = !link.driver();
    uint8_t a[4], b[4]; fromU32(first, a); fromU32(last, b);
    onLink_ = link.onLink(a) && link.onLink(b);
    if (arpOnly_ && !onLink_) { fail("Off the local link: ICMP is held by the driver."); return false; }
    base_ = first; n_ = (int)(last - first + 1); memset(hosts_, 0, sizeof(Host) * (size_t)n_);
    int self = indexOf(link.ip());
    if (self >= 0) { hosts_[self].flags = F_SELF | F_MAC; memcpy(hosts_[self].mac, link.mac(), 6); up_ = 1; }
    doArp_ = onLink_; doIcmp_ = !arpOnly_; arpPhase_ = doArp_;
    seqBase_ = (uint16_t)((++scanGeneration & 7) << 13);
    t0_ = roundStart_ = r2::ticks(); next_ = sent_ = 0; round_ = 1; state_ = SWEEP;
    return true;
}
void HostScan::stop() { if (busy()) { doneMs_ = r2::ticks(); state_ = DONE; } }
int HostScan::indexOf(const uint8_t ip[4]) const
{
    uint32_t v = toU32(ip); return v < base_ || v - base_ >= (uint32_t)n_ ? -1 : (int)(v - base_);
}
void HostScan::addressOf(int i, uint8_t ip[4]) const { fromU32(base_ + (uint32_t)i, ip); }
int HostScan::tries() const { return arpPhase_ ? ARP_TRIES : (onLink_ ? PING_TRIES_ON_LINK : PING_TRIES_OFF_LINK); }
void HostScan::sendEcho(int idx)
{
    uint8_t dip[4], hop[4], mac[6]; addressOf(idx, dip); link_->nextHop(dip, hop);
    if (!link_->knownMac(hop, mac)) memcpy(mac, BCAST, 6);
    uint16_t seq = (uint16_t)(seqBase_ + (round_ - 1) * MAX_TARGETS + idx);
    link_->sendEcho(mac, dip, NETMUX_NSK_ICMP_ID, seq, 64);
}
void HostScan::markUp(int idx, bool viaArp, const uint8_t *mac, bool current)
{
    Host &h = hosts_[idx]; bool first = !isUp(idx);
    h.flags |= viaArp ? F_ARP : F_ICMP;
    if (mac) { memcpy(h.mac, mac, 6); h.flags |= F_MAC; }
    if (current && (h.flags & F_SENT) && !(h.flags & F_RTT))
    {
        h.rttMs = (uint32_t)r2::ticks() - h.sentMs; h.flags |= F_RTT; h.answeredOn = h.tries;
    }
    if (first) up_++;
}
bool HostScan::onFrame(const uint8_t *f, size_t n)
{
    if (!busy() || n < 14 || !memcmp(f + 6, link_->mac(), 6)) return false;
    if (arp(f, n))
    {
        uint16_t op = get16(f + 20); int idx = indexOf(f + 28);
        if (idx < 0 || (op != 1 && op != 2)) return false;
        markUp(idx, true, f + 22, arpPhase_ && op == 2 && !memcmp(f + 38, link_->ip(), 4));
        return true;
    }
    Packet p;
    if (!ipv4(f, n, p) || p.ip[9] != 1 || p.len < 8 || memcmp(p.ip + 16, link_->ip(), 4) ||
        p.body[0] != 0 || p.body[1] != 0 || get16(p.body + 4) != NETMUX_NSK_ICMP_ID) return false;
    int idx = indexOf(p.ip + 12);
    uint16_t seq = (uint16_t)(get16(p.body + 6) - seqBase_);
    if (idx < 0 || seq % MAX_TARGETS != idx || seq / MAX_TARGETS >= (unsigned)round_ ||
        !hosts_[idx].tries || arpPhase_) return false;
    bool current = seq / MAX_TARGETS == (unsigned)(round_ - 1);
    markUp(idx, false, link_->onLink(p.ip + 12) ? f + 6 : nullptr, current);
    return true;
}
bool HostScan::step(uint64_t now)
{
    if (!busy()) return false;
    if (state_ == WAIT)
    {
        if (now < waitUntil_) return false;
        if (round_ < tries()) round_++;
        else if (arpPhase_ && doIcmp_) { arpPhase_ = false; round_ = 1; }
        else { state_ = DONE; doneMs_ = now; return true; }
        next_ = sent_ = 0; roundStart_ = now; state_ = SWEEP;
    }
    int budget = (int)((now - roundStart_) * PROBES_PER_MS + 1 - sent_);
    if (budget > PACE) budget = PACE;
    bool changed = false;
    while (next_ < n_)
    {
        if (isUp(next_)) { next_++; changed = true; continue; }
        if (budget <= 0) break;
        Host &h = hosts_[next_]; h.sentMs = (uint32_t)now; h.flags |= F_SENT; h.tries++;
        if (arpPhase_) { uint8_t dip[4]; addressOf(next_, dip); link_->sendArp(1, dip, ZERO_MAC); }
        else sendEcho(next_);
        next_++; sent_++; budget--; changed = true;
    }
    if (next_ == n_)
    {
        state_ = WAIT;
        bool final = round_ == tries() && (!arpPhase_ || !doIcmp_);
        waitUntil_ = now + (final ? LISTEN_MS : GAP_MS);
        changed = true;
    }
    return changed;
}
void HostScan::describe(char *out, size_t cap) const
{
    if (state_ == IDLE) { scopy(out, "Enter a subnet and press Scan.", cap); return; }
    if (state_ == FAILED) { scopy(out, error_, cap); return; }
    char buf[96]; char *p = buf;
    if (busy())
    {
        const char *s = arpPhase_ ? "ARP " : "ping "; while (*s) *p++ = *s++;
        p = putU(p, round_); *p++ = '/'; p = putU(p, tries());
        s = state_ == WAIT ? ": listening, " : ": "; while (*s) *p++ = *s++;
        if (state_ == SWEEP) { p = putU(p, next_); *p++ = '/'; p = putU(p, n_); *p++ = ','; *p++ = ' '; }
    }
    else if (arpOnly_) { const char *s = "ARP only: "; while (*s) *p++ = *s++; }
    p = putU(p, up_); const char *s = " up / "; while (*s) *p++ = *s++; p = putU(p, n_);
    if (!busy()) { s = " in "; while (*s) *p++ = *s++; p = putU(p, (uint32_t)(doneMs_ - t0_)); s = " ms"; while (*s) *p++ = *s++; }
    *p = 0; scopy(out, buf, cap);
}

bool Pinger::start(NetLink &link, const uint8_t dst[4])
{
    stop(); error_[0] = 0; mode_ = NONE; nLines_ = sent_ = back_ = 0; minMs_ = maxMs_ = sumMs_ = 0;
    if (zeroIp(dst)) { scopy(error_, "Enter a nonzero IPv4 address.", sizeof(error_)); return false; }
    if (!link.open(error_, sizeof(error_))) return false;
    link_ = &link; memcpy(dst_, dst, 4); link.nextHop(dst, hop_);
    mode_ = link.driver() ? ICMP : (link.onLink(dst) ? ARP : TCP);
    haveMac_ = link.knownMac(hop_, dmac_); memset(pend_, 0, sizeof(pend_));
    seq_ = 0; seqBase_ = pingSequence; nextSend_ = r2::ticks(); resolveAt_ = resolveUntil_ = 0;
    busy_ = true; return true;
}
void Pinger::stop() { busy_ = false; memset(pend_, 0, sizeof(pend_)); }
void Pinger::add(uint16_t seq, Kind kind, uint8_t ttl, uint32_t ms, const uint8_t from[4])
{
    Line &l = log_[nLines_++ % LOG]; l.seq = seq; l.kind = kind; l.ttl = ttl; l.ms = ms; memcpy(l.from, from, 4);
    if (kind == L_REPLY || kind == L_OPEN || kind == L_CLOSED)
    {
        if (!back_ || ms < minMs_) minMs_ = ms;
        if (ms > maxMs_) maxMs_ = ms;
        sumMs_ += ms; back_++;
    }
}
const Pinger::Line &Pinger::line(int i) const { return log_[((nLines_ > LOG ? nLines_ - LOG : 0) + i) % LOG]; }
Pinger::Probe *Pinger::pending(uint16_t seq)
{
    for (Probe &p : pend_) if (p.live && p.seq == seq) return &p;
    return nullptr;
}
void Pinger::answer(Probe &p, Kind kind, uint8_t ttl, const uint8_t from[4])
{
    add(p.seq, kind, ttl, (uint32_t)r2::ticks() - p.sentMs, from); p.live = false;
}
bool Pinger::step(uint64_t now)
{
    if (!busy_) return false;
    bool changed = false;
    for (Probe &p : pend_)
        if (p.live && (uint32_t)now - p.sentMs >= TIMEOUT_MS) { answer(p, L_TIMEOUT, 0, dst_); changed = true; }
    if (now < nextSend_) return changed;
    if (mode_ != ARP && !haveMac_) haveMac_ = link_->knownMac(hop_, dmac_);
    if (mode_ != ARP && !haveMac_)
    {
        if (!resolveUntil_) { resolveUntil_ = now + TIMEOUT_MS; resolveAt_ = now; }
        if (now >= resolveUntil_)
        {
            sent_++; add(++seq_, L_NOARP, 0, TIMEOUT_MS, hop_); resolveUntil_ = 0;
            nextSend_ = now + INTERVAL_MS; return true;
        }
        if (now >= resolveAt_) { link_->sendArp(1, hop_, ZERO_MAC); resolveAt_ = now + 500; }
        return changed;
    }
    resolveUntil_ = 0;
    for (Probe &p : pend_) if (!p.live)
    {
        p.live = true; p.seq = ++seq_; p.sentMs = (uint32_t)now;
        // Continue wire sequences across runs so a late reply cannot become
        // the first answer of a restarted ping. Display sequences reset.
        pingSequence = (uint16_t)(seqBase_ + p.seq);
        p.tcpSeq = 0x50470000u ^ ((uint32_t)(uint16_t)(seqBase_ + p.seq) << 8) ^ (uint32_t)now;
        sent_++; nextSend_ = now + INTERVAL_MS;
        if (mode_ == ARP) link_->sendArp(1, dst_, ZERO_MAC);
        else if (mode_ == ICMP) link_->sendEcho(dmac_, dst_, NETMUX_PING_ICMP_ID, (uint16_t)(seqBase_ + p.seq), 64);
        else link_->sendTcp(dmac_, dst_, NetLink::PORT, TCP_PORT, p.tcpSeq, 0, 2);
        return true;
    }
    return changed;
}
bool Pinger::onFrame(const uint8_t *f, size_t n)
{
    if (!busy_) return false;
    if (mode_ == ARP)
    {
        if (!arp(f, n) || get16(f + 20) != 2 || memcmp(f + 28, dst_, 4) || memcmp(f + 38, link_->ip(), 4)) return false;
        Probe *oldest = nullptr;
        for (Probe &p : pend_) if (p.live && (!oldest || (uint16_t)(p.seq - oldest->seq) > 0x8000)) oldest = &p;
        if (!oldest) return false;
        answer(*oldest, L_REPLY, 0, dst_); return true;
    }
    Packet p;
    if (!ipv4(f, n, p) || memcmp(p.ip + 16, link_->ip(), 4)) return false;
    if (mode_ == ICMP && p.ip[9] == 1 && p.len >= 8)
    {
        uint16_t seq;
        if (p.body[0] == 0 && p.body[1] == 0 && get16(p.body + 4) == NETMUX_PING_ICMP_ID && !memcmp(p.ip + 12, dst_, 4))
            seq = get16(p.body + 6);
        else if (!echoError(p, link_->ip(), dst_, NETMUX_PING_ICMP_ID, seq)) return false;
        Probe *probe = pending((uint16_t)(seq - seqBase_)); if (!probe) return false;
        answer(*probe, p.body[0] == 0 ? L_REPLY : L_UNREACH, p.ip[8], p.ip + 12); return true;
    }
    if (mode_ != TCP || p.ip[9] != 6 || p.len < 20 || memcmp(p.ip + 12, dst_, 4) ||
        get16(p.body) != TCP_PORT || get16(p.body + 2) != NetLink::PORT ||
        (p.body[12] >> 4) < 5 || (size_t)(p.body[12] >> 4) * 4 > p.len) return false;
    uint8_t flags = p.body[13];
    if (!(flags & 16) || (!(flags & 4) && (flags & 0x12) != 0x12)) return false;
    for (Probe &probe : pend_) if (probe.live && get32(p.body + 8) == probe.tcpSeq + 1)
    {
        if (!(flags & 4)) link_->sendTcp(dmac_, dst_, NetLink::PORT, TCP_PORT, get32(p.body + 8), 0, 4);
        answer(probe, flags & 4 ? L_CLOSED : L_OPEN, p.ip[8], dst_); return true;
    }
    return false;
}
void Pinger::describe(char *out, size_t cap) const
{
    if (error_[0]) { scopy(out, error_, cap); return; }
    const char *s = mode_ == ICMP ? "ICMP echo" : mode_ == ARP ? "ARP ping (ICMP held by network driver)" :
                    mode_ == TCP ? "TCP port 80 (ICMP held by network driver)" : "Enter an IPv4 address and press Ping.";
    scopy(out, s, cap);
}
void Pinger::summary(char *out, size_t cap) const
{
    char buf[128]; char *p = putU(buf, sent_); const char *s = " sent, "; while (*s) *p++ = *s++;
    p = putU(p, back_); s = " replies, "; while (*s) *p++ = *s++;
    int pendingCount = 0; for (const Probe &probe : pend_) if (probe.live) pendingCount++;
    int finished = sent_ - pendingCount;
    p = putU(p, finished ? (uint32_t)((uint64_t)(finished - back_) * 100 / finished) : 0);
    s = "% lost"; while (*s) *p++ = *s++;
    if (back_)
    {
        s = ", min/avg/max "; while (*s) *p++ = *s++;
        p = putU(p, minMs_); *p++ = '/'; p = putU(p, (uint32_t)(sumMs_ / back_)); *p++ = '/'; p = putU(p, maxMs_);
        s = " ms"; while (*s) *p++ = *s++;
    }
    *p = 0; scopy(out, buf, cap);
}

void Tracer::fail(const char *why) { scopy(error_, why, sizeof(error_)); state_ = FAILED; }
bool Tracer::start(NetLink &link, const uint8_t dst[4])
{
    stop(); error_[0] = 0; memset(hops_, 0, sizeof(hops_)); sentTo_ = 0; end_ = MAX_HOPS;
    if (zeroIp(dst)) { fail("Enter a nonzero IPv4 address."); return false; }
    if (!link.open(error_, sizeof(error_))) { state_ = FAILED; return false; }
    if (!link.driver()) { fail("Traceroute needs ICMP; another process holds the driver."); return false; }
    link_ = &link; memcpy(dst_, dst, 4); link.nextHop(dst, hop_);
    seqBase_ = (uint16_t)(++traceGeneration * 128); round_ = 0; ttl_ = 1; arpTries_ = 0;
    nextAt_ = r2::ticks(); state_ = link.knownMac(hop_, dmac_) ? SEND : RESOLVE; return true;
}
void Tracer::stop() { if (busy()) state_ = DONE; }
bool Tracer::roundAnswered() const
{
    for (int i = 0; i < end_; i++) if (!(hops_[i].answered & (1 << round_))) return false;
    return true;
}
bool Tracer::step(uint64_t now)
{
    if (!busy()) return false;
    if (state_ == RESOLVE)
    {
        if (link_->knownMac(hop_, dmac_)) { state_ = SEND; nextAt_ = now; }
        else if (now >= nextAt_)
        {
            if (arpTries_ == 3) { fail("Next hop did not answer ARP."); return true; }
            link_->sendArp(1, hop_, ZERO_MAC); arpTries_++; nextAt_ = now + 600;
        }
        return true;
    }
    if (state_ == WAIT)
    {
        if (now < nextAt_ && !roundAnswered()) return false;
        if (++round_ == ROUNDS) { state_ = DONE; return true; }
        ttl_ = 1; state_ = SEND; nextAt_ = now;
    }
    if (now < nextAt_) return false;
    if (ttl_ <= end_)
    {
        Hop &h = hops_[ttl_ - 1]; h.sentMs[round_] = (uint32_t)now;
        link_->sendEcho(dmac_, dst_, NETMUX_TRACE_ICMP_ID, (uint16_t)(seqBase_ + round_ * MAX_HOPS + ttl_), (uint8_t)ttl_);
        if (ttl_ > sentTo_) sentTo_ = ttl_;
        ttl_++; nextAt_ = now + SPACING_MS;
    }
    if (ttl_ > end_) { state_ = WAIT; nextAt_ = now + ROUND_WAIT_MS; }
    return true;
}
bool Tracer::onFrame(const uint8_t *f, size_t n)
{
    if (!busy() || state_ == RESOLVE) return false;
    Packet p;
    if (!ipv4(f, n, p) || p.ip[9] != 1 || p.len < 8 || memcmp(p.ip + 16, link_->ip(), 4)) return false;
    uint16_t seq;
    bool dest = p.body[0] == 0 && p.body[1] == 0 && get16(p.body + 4) == NETMUX_TRACE_ICMP_ID && !memcmp(p.ip + 12, dst_, 4);
    if (dest) seq = get16(p.body + 6);
    else if (!echoError(p, link_->ip(), dst_, NETMUX_TRACE_ICMP_ID, seq)) return false;
    unsigned v = (uint16_t)(seq - seqBase_ - 1), round = v / MAX_HOPS, idx = v % MAX_HOPS;
    if (round >= ROUNDS || round > (unsigned)round_ || (round == (unsigned)round_ && idx + 1 >= (unsigned)ttl_) || idx >= (unsigned)end_) return false;
    Hop &h = hops_[idx];
    if (h.answered & (1 << round)) return false;
    h.answered |= (uint8_t)(1 << round); h.flags |= H_ADDR; memcpy(h.ip, p.ip + 12, 4);
    h.ms[round] = (uint32_t)r2::ticks() - h.sentMs[round];
    if (dest || p.body[0] == 3)
    {
        h.flags |= dest ? H_DEST : H_UNREACH; h.code = p.body[1]; end_ = (int)idx + 1;
    }
    return true;
}
int Tracer::hops() const { return sentTo_ < end_ ? sentTo_ : end_; }
void Tracer::describe(char *out, size_t cap) const
{
    if (state_ == FAILED) { scopy(out, error_, cap); return; }
    if (state_ == IDLE) { scopy(out, "Enter an IPv4 address and press Trace.", cap); return; }
    if (state_ == RESOLVE) { scopy(out, "Resolving the next hop by ARP...", cap); return; }
    char buf[96]; char *p = buf;
    if (busy())
    {
        const char *s = "Round "; while (*s) *p++ = *s++;
        p = putU(p, round_ + 1); *p++ = '/'; p = putU(p, ROUNDS);
        s = state_ == WAIT ? ", listening" : ", hop "; while (*s) *p++ = *s++;
        if (state_ == SEND) p = putU(p, ttl_);
    }
    else
    {
        p = putIp(p, dst_); const char *s = " - "; while (*s) *p++ = *s++;
        s = hops_[end_ - 1].flags & H_DEST ? "reached in " : hops_[end_ - 1].flags & H_UNREACH ? "unreachable at hop " : "stopped after ";
        while (*s) *p++ = *s++;
        p = putU(p, hops());
    }
    *p = 0; scopy(out, buf, cap);
}

bool NetTools::step()
{
    if (!busy()) { link.close(); return false; }
    bool changed = false;
    // Drain replies before advancing deadlines. Delayed answers still count
    // even when the UI could not poll at their expected arrival time.
    for (int k = 0; k < 64; k++)
    {
        int64_t n = netmux_pull(NETMUX_NSK, rx_, sizeof(rx_)); if (n <= 0) break;
        link.onFrame(rx_, (size_t)n);
        changed |= scan.onFrame(rx_, (size_t)n); changed |= ping.onFrame(rx_, (size_t)n); changed |= trace.onFrame(rx_, (size_t)n);
    }
    uint64_t now = r2::ticks();
    changed |= scan.step(now); changed |= ping.step(now); changed |= trace.step(now);
    if (!busy()) link.close();
    return changed;
}
void NetTools::stopAll() { scan.stop(); ping.stop(); trace.stop(); link.close(); }
