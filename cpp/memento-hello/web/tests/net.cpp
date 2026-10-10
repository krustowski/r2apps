// Exercise the production stack against the kernel's shared 16-port registry.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>
#include "../net_r2.cpp"

namespace {
int process = 1;
bool driver = false, refuseRelease = false, gatewayKnown = true;
uint64_t clockMs = 1000;
int failAllocation = 0;
size_t liveAllocations = 0;
std::map<uint16_t, int> bindings;
std::map<int, std::deque<std::vector<uint8_t>>> incoming;
std::vector<uint8_t> lastSent;
const uint8_t remote[4] = {1, 1, 1, 1};

void check(bool ok, const char *message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void select(int pid, uint16_t base)
{
    process = pid;
    web::r2NetSetPortBase(base);
}
void reply(uint8_t flags, uint32_t seq, uint32_t ack)
{
    check(lastSent.size() >= 54, "TCP frame sent");
    std::vector<uint8_t> frame(54);
    frame[6] = 0x02; // A peer MAC distinct from ours.
    web::put16(frame.data() + 12, 0x0800);
    uint8_t *ip = frame.data() + 14, *tcp = ip + 20;
    ip[0] = 0x45; ip[9] = 6;
    web::put16(ip + 2, 40);
    std::memcpy(ip + 12, lastSent.data() + 30, 4);
    std::memcpy(ip + 16, lastSent.data() + 26, 4);
    web::put16(tcp, web::get16(lastSent.data() + 36));
    web::put16(tcp + 2, web::get16(lastSent.data() + 34));
    web::put32(tcp + 4, seq); web::put32(tcp + 8, ack);
    tcp[12] = 0x50; tcp[13] = flags;
    web::put16(tcp + 14, 32768);
    incoming[process].push_back(frame);
}
int established(web::Stack &stack)
{
    int h = stack.connect(remote, 443);
    check(h >= 0, "connection allocated");
    reply(web::F_SYN | web::F_ACK, 100, web::get32(lastSent.data() + 38) + 1);
    stack.poll();
    check(stack.status(h) == web::NetIf::OPEN, "SYN/ACK established connection");
    return h;
}
// Reply to the query as it was sent, even if another loader has since asked
// for a different host. This models replies arriving between idle ticks.
void dnsReply(const std::vector<uint8_t> &query, int rcode = 0)
{
    check(query.size() >= 54 && query[23] == 17, "DNS query sent over UDP");
    size_t qlen = query.size() - 42;
    std::vector<uint8_t> frame(42 + qlen + (rcode ? 0 : 16));
    frame[6] = 0x02; web::put16(frame.data() + 12, 0x0800);
    uint8_t *ip = frame.data() + 14, *udp = ip + 20, *dns = udp + 8;
    ip[0] = 0x45; ip[9] = 17; web::put16(ip + 2, (uint16_t)(frame.size() - 14));
    std::memcpy(ip + 12, query.data() + 30, 4);
    std::memcpy(ip + 16, query.data() + 26, 4);
    web::put16(udp, 53); web::put16(udp + 2, web::get16(query.data() + 34));
    web::put16(udp + 4, (uint16_t)(frame.size() - 34));
    std::memcpy(dns, query.data() + 42, qlen);
    web::put16(dns + 2, (uint16_t)(0x8180 | rcode));
    web::put16(dns + 6, rcode ? 0 : 1);
    if (!rcode) {
        uint8_t *a = dns + qlen;
        web::put16(a, 0xc00c); web::put16(a + 2, 1); web::put16(a + 4, 1);
        web::put32(a + 6, 60); web::put16(a + 10, 4);
        std::memcpy(a + 12, remote, 4);
    }
    incoming[process].push_back(frame);
}
void unresolvedGateway()
{
    select(6, 50100);
    gatewayKnown = false;
    web::Stack stack;
    uint8_t ip[4];
    check(stack.resolve("missing-arp.test", ip) == 0, "lookup waits for gateway ARP");
    // Retrying a UDP send must not keep resetting the phase deadline. An
    // unreachable gateway must leave the shared resolver available again.
    for (int i = 0; i < 210; ++i) { clockMs += 50; stack.poll(); }
    check(stack.resolve("missing-arp.test", ip) == -1, "missing ARP cannot wedge the resolver forever");
    check(bindings.size() == 10 && liveAllocations == 0, "unreachable gateway releases DNS socket");
    gatewayKnown = true;
}
void concurrentDns()
{
    select(5, 50000);
    web::Stack stack;
    uint8_t ip[4];
    check(stack.resolve("r2.n0p.cz", ip) == 0, "page lookup starts");
    auto first = lastSent;
    for (int i = 0; i < 20; ++i) {
        check(stack.resolve("umami.vxn.dev", ip) == 0, "analytics waits for page lookup");
        check(stack.resolve("fonts.googleapis.com", ip) == 0, "stylesheet waits for page lookup");
    }
    dnsReply(first); stack.poll();
    // A different caller notices completion first: preserve the original
    // answer for its owner rather than throwing it away.
    check(stack.resolve("umami.vxn.dev", ip) == 0, "next lookup starts after page answer");
    auto second = lastSent;
    check(stack.resolve("r2.n0p.cz", ip) == 1 && !std::memcmp(ip, remote, 4), "concurrent lookups preserve page answer");
    dnsReply(second, 3); stack.poll();
    check(stack.resolve("fonts.googleapis.com", ip) == 0, "failure permits next lookup");
    auto third = lastSent;
    check(stack.resolve("umami.vxn.dev", ip) == -1, "negative answer reaches original caller");
    dnsReply(third); stack.poll();
    check(stack.resolve("fonts.googleapis.com", ip) == 1, "stylesheet lookup finishes");

    // Navigation cancels the caller, while the shared DNS/TCP query is
    // already underway. It must time out and release its socket, then allow
    // a new host to resolve even if the old caller never polls again.
    check(stack.resolve("abandoned.test", ip) == 0, "abandoned lookup starts");
    clockMs += 1601; stack.poll(); clockMs += 1601; stack.poll();
    check(liveAllocations == 2, "DNS fallback owns TCP buffers");
    check(stack.resolve("next-page.test", ip) == 0, "navigation waits without cancelling DNS TCP");
    clockMs += 6001; stack.poll();
    check(stack.resolve("next-page.test", ip) == 0, "navigation starts after abandoned lookup timeout");
    auto next = lastSent;
    dnsReply(next); stack.poll();
    check(stack.resolve("next-page.test", ip) == 1, "navigation recovers after failed DNS TCP");
    check(bindings.size() == 10 && liveAllocations == 0, "DNS timeout releases ports and buffers");

    check(stack.resolve("tcp-dns.test", ip) == 0, "TCP lookup starts over UDP");
    auto query = lastSent;
    clockMs += 1601; stack.poll(); clockMs += 1601; stack.poll();
    check(stack.resolve("waiting.test", ip) == 0, "another host preserves DNS TCP handshake");
    reply(web::F_SYN | web::F_ACK, 100, web::get32(lastSent.data() + 38) + 1);
    stack.poll();
    check(lastSent.size() > 54, "DNS query sent over established TCP");
    uint32_t ack = web::get32(lastSent.data() + 38) + (uint32_t)lastSent.size() - 54;
    dnsReply(query);
    auto answer = incoming[process].back(); incoming[process].pop_back();
    reply(web::F_ACK | web::F_PSH, 101, ack);
    auto &tcp = incoming[process].back();
    size_t len = answer.size() - 42;
    tcp.resize(56 + len);
    web::put16(tcp.data() + 16, (uint16_t)(tcp.size() - 14));
    web::put16(tcp.data() + 54, (uint16_t)len);
    std::memcpy(tcp.data() + 56, answer.data() + 42, len);
    stack.poll();
    check(stack.resolve("waiting.test", ip) == 0, "next host starts after DNS TCP answer");
    auto waiting = lastSent;
    check(stack.resolve("tcp-dns.test", ip) == 1, "DNS TCP answer preserved for original caller");
    dnsReply(waiting); stack.poll();
    check(stack.resolve("waiting.test", ip) == 1, "host waiting for DNS TCP finishes");
    clockMs += 3001; stack.poll();
    check(bindings.size() == 10 && liveAllocations == 0, "DNS TCP completion eventually releases port");

    // Negative caching coordinates waiters without poisoning future retries.
    check(stack.resolve("umami.vxn.dev", ip) == 0, "expired negative answer can be retried");
    auto retry = lastSent;
    dnsReply(retry); stack.poll();
    check(stack.resolve("umami.vxn.dev", ip) == 1, "failed host can recover without restarting browser");
}
}

namespace r2 {
uint64_t ticks() noexcept { return clockMs; }
int64_t raw_syscall(Sys number, int64_t arg1, int64_t arg2, int64_t) noexcept
{
    if (number == Sys::NetRegister && arg2 == 1) {
        auto it = bindings.find((uint16_t)arg1);
        if (refuseRelease || (it != bindings.end() && it->second != process)) return 0xfa;
        if (it != bindings.end()) bindings.erase(it);
        return 0;
    }
    if (number == Sys::SendPacket) {
        const auto *frame = (const uint8_t *)arg2;
        if (web::get16(frame + 12) == 0x0800)
            lastSent.assign(frame, frame + 14 + web::get16(frame + 16));
        return 0;
    }
    return 0xfc;
}
namespace net {
std::optional<Status> status()
{
    return Status{{{0x52, 0x54, 0, 0x12, 0x34, 0x56}}, {{10, 3, 4, 2}}};
}
std::optional<Config> config()
{
    return Config{{{10, 3, 4, 2}}, {{255, 255, 255, 0}}, {{10, 3, 4, 1}}, {{1, 1, 1, 1}},
                  {{0x52, 0x54, 0, 0x12, 0x34, 0x56}}, {{0x52, 0x54, 0, 0x12, 0x34, 0x57}}, gatewayKnown};
}
bool bind_port(uint16_t port)
{
    auto it = bindings.find(port);
    if (it != bindings.end()) return it->second == process;
    if (bindings.size() == 16) return false;
    bindings[port] = process;
    return true;
}
}
}
extern "C" bool netmux_take_driver() { return driver; }
extern "C" void netmux_claim(int, uint16_t, uint16_t) {}
extern "C" bool netmux_send(const uint8_t *frame, uint32_t len)
{ return r2::raw_syscall(r2::Sys::SendPacket, 4, (int64_t)frame, len) == 0; }
extern "C" void netmux_port_bound(uint16_t, const char *) {}
extern "C" void netmux_port_released(uint16_t) {}
extern "C" int64_t netmux_pull(int, uint8_t *out, uint32_t cap)
{
    auto &queue = incoming[process];
    if (queue.empty()) return 0;
    auto frame = queue.front(); queue.pop_front();
    check(frame.size() <= cap, "receive capacity");
    std::memcpy(out, frame.data(), frame.size());
    return frame.size();
}
namespace web {
void *alloc(size_t n) { return std::malloc(n); }
void *realloc(void *p, size_t n) { return std::realloc(p, n); }
void free(void *p) { std::free(p); }
void *big_alloc(size_t n)
{
    if (failAllocation && --failAllocation == 0) return nullptr;
    void *p = std::malloc(n);
    if (p) ++liveAllocations;
    return p;
}
void *big_realloc(void *p, size_t n)
{
    if (!p) return big_alloc(n);
    return std::realloc(p, n);
}
void big_free(void *p) { if (p) { --liveAllocations; std::free(p); } }
uint64_t now_ms() { return clockMs; }
}

int main()
{
    // Unrelated services retain ten entries, as Chat/IRC and other apps can.
    select(9, 47000);
    for (uint16_t p = 10000; p < 10010; ++p) check(r2::net::bind_port(p), "service binding");
    web::Stack desktop, browser;
    select(1, 47000); int desktopLive = established(desktop);
    select(2, 48000); int browserLive = established(browser);
    for (int i = 0; i < 100; ++i) {
        select(1, 47000);
        int h = desktop.connect(remote, 53);
        check(h >= 0, "Memento DNS connection allocated"); desktop.close(h);
        select(2, 48000);
        h = browser.connect(remote, 53);
        check(h >= 0, "browser DNS connection allocated"); browser.close(h);
        select(3, 40000);
        check(r2::net::bind_port(40000), "Spotify can still bind its DNS/TCP port");
        check(r2::raw_syscall(r2::Sys::NetRegister, 40000, 1) == 0, "Spotify releases DNS port");
    }
    check(bindings.size() == 12, "closed connections do not consume registry entries");
    select(1, 47000); check(desktop.status(desktopLive) == web::NetIf::OPEN, "desktop connection preserved");
    select(2, 48000); check(browser.status(browserLive) == web::NetIf::OPEN, "browser connection preserved");

    // Both buffer allocations can fail after a successful bind.
    for (int allocation = 1; allocation <= 2; ++allocation) {
        failAllocation = allocation;
        check(browser.connect(remote, 443) < 0, "allocation failure reported");
        check(bindings.size() == 12 && liveAllocations == 4, "allocation failure releases port and buffers");
    }
    browser.close(browserLive);
    check(bindings.size() == 12, "FIN_WAIT keeps binding until shutdown finishes");
    reply(web::F_FIN | web::F_ACK, 101, web::get32(lastSent.data() + 38) + 1);
    browser.poll();
    check(bindings.size() == 11, "completed FIN handshake releases port");
    browser.close(browserLive);
    check(bindings.size() == 11, "repeated close is harmless");

    int h = established(browser);
    browser.close(h);
    clockMs += 3001; browser.poll();
    check(bindings.size() == 11, "FIN timeout releases port");
    h = established(browser);
    reply(web::F_RST, 101, 0); browser.poll(); browser.close(h);
    check(bindings.size() == 11, "reset connection releases port");

    // A rejected release must leave the cached binding available for reuse.
    h = browser.connect(remote, 443); check(h >= 0, "connection before rejected release");
    refuseRelease = true; browser.close(h); refuseRelease = false;
    check(bindings.size() == 12, "failed release retains registry entry");
    for (int i = 0; i < 8; ++i) {
        h = browser.connect(remote, 443); check(h >= 0, "cached binding can be reused"); browser.close(h);
    }
    check(bindings.size() == 11, "reused binding eventually releases");
    select(1, 47000); desktop.close(desktopLive); clockMs += 3001; desktop.poll();
    check(bindings.size() == 10 && liveAllocations == 0, "only unrelated services remain");

    select(9, 47000);
    for (uint16_t p = 10010; p < 10016; ++p) check(r2::net::bind_port(p), "fill registry");
    select(2, 48000);
    check(browser.connect(remote, 443) < 0, "full registry reports connection failure");
    check(bindings.size() == 16 && liveAllocations == 0, "full registry preserves other owners");
    select(9, 47000);
    for (uint16_t p = 10010; p < 10016; ++p)
        check(r2::raw_syscall(r2::Sys::NetRegister, p, 1) == 0, "release filler binding");

    // This machine, through the kernel's loopback device: localhost is
    // 127.0.0.1, a connection there goes to our own address, the kernel
    // answers ARP for it with the MAC 00:00:00:00:00:00, and replies come
    // back from that MAC.
    select(4, 49000);
    web::Stack local;
    uint8_t lo[4];
    check(local.resolve("localhost", lo) == 1 && !std::memcmp(lo, "\x7f\0\0\x01", 4), "localhost is 127.0.0.1");
    lastSent.clear();
    h = local.connect(lo, 80);
    check(h >= 0 && lastSent.empty(), "own address asked for by ARP first");
    std::vector<uint8_t> arp(42);
    std::memcpy(arp.data(), "\x52\x54\0\x12\x34\x56", 6);
    web::put16(arp.data() + 12, 0x0806);
    std::memcpy(arp.data() + 14, "\0\x01\x08\0\x06\x04\0\x02", 8);
    std::memcpy(arp.data() + 28, "\x0a\x03\x04\x02", 4);
    std::memcpy(arp.data() + 32, "\x52\x54\0\x12\x34\x56", 6);
    std::memcpy(arp.data() + 38, "\x0a\x03\x04\x02", 4);
    incoming[process].push_back(arp);
    local.poll();
    clockMs += 50; local.poll();
    check(lastSent.size() >= 54 && !std::memcmp(lastSent.data(), "\0\0\0\0\0\0", 6), "SYN to the loopback MAC");
    check(!std::memcmp(lastSent.data() + 26, "\x0a\x03\x04\x02\x0a\x03\x04\x02", 8), "127.0.0.1 goes to our own address");
    check(web::get16(lastSent.data() + 36) == 80, "SYN to port 80");
    reply(web::F_SYN | web::F_ACK, 100, web::get32(lastSent.data() + 38) + 1);
    std::memset(incoming[process].back().data() + 6, 0, 6);
    local.poll();
    check(local.status(h) == web::NetIf::OPEN, "looped SYN/ACK established connection");
    local.close(h); clockMs += 3001; local.poll();
    check(bindings.size() == 10 && liveAllocations == 0, "loopback connection released");

    concurrentDns();
    unresolvedGateway();

    driver = true;
    web::Stack driverStack;
    h = driverStack.connect(remote, 443); check(h >= 0, "global driver connection"); driverStack.close(h);
    check(bindings.size() == 10 && liveAllocations == 0, "global driver does not bind ports");
    std::puts("Network tests passed (ports, concurrent DNS, TCP fallback, timeouts and recovery).");
}
