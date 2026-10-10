#include "platform.h"
#include "crypto.h"
#include "entropy.h"
#include "syscall.h"
#include <string.h>

#define SC_TUNNEL WGD_SYSCALL
#define TUNNEL_CAPABILITY WGD_CAPABILITY
static NetConfig_T network;
static struct wgd_config tunnel;
static uint8_t frame[2048], tx[1514], injected[1514];
static uint32_t refreshed, arp_at;
static uint64_t clock_ticks, clock_unix, tai_last;
static bool attached;
struct neighbour { uint8_t ip[4], mac[6]; bool valid; };
static struct neighbour neighbours[8];
static unsigned neighbour_next;
static char events[768], counters[160], log_text[1024];
static unsigned event_len;
static void save_log(void) {
    memset(log_text, 0, sizeof(log_text));
    memcpy(log_text, events, event_len);
    memcpy(log_text + event_len, counters, strlen(counters));
    /* The legacy write_file syscall always writes 512 bytes. Use the range
     * API for complete text, recreating the snapshot when its length shrinks. */
    const uint8_t *path = (const uint8_t *)"/mnt/tmp/WGD.LOG";
    delete_file(path);
    write_file_at(path, (const uint8_t *)log_text, 0, event_len + strlen(counters));
}

void wgd_event(const char *text) {
    size_t n = strlen(text);
    if (n > 220) n = 220;
    if (event_len + n + 6 >= sizeof(events)) { memset(events, 0, sizeof(events)); event_len = 0; }
    memcpy(events + event_len, "wgd: ", 5); event_len += 5;
    memcpy(events + event_len, text, n); event_len += n; events[event_len++] = '\n';
    save_log();
    print((const uint8_t *)"wgd: "); print((const uint8_t *)text); print((const uint8_t *)"\n");
    /* Port I/O goes through the kernel ABI, including QEMU debugcon. */
    const char *prefix = "wgd: ";
    while (*prefix) write_port(0xe9, (uint8_t)*prefix++);
    while (*text) write_port(0xe9, (uint8_t)*text++);
    write_port(0xe9, '\n');
}

void wireguard_random_bytes(void *out, size_t n) {
    if (!wgd_entropy_read(out, n)) {
        crypto_zero(out, n); wgd_platform_detach();
        wgd_entropy_stop();
        wgd_event("random source failed its health checks; stopped"); r2_exit(0, 1);
    }
}
uint32_t wireguard_sys_now(void) { return (uint32_t)get_ticks(); }
bool wireguard_is_under_load(void) { return false; } /* engine.c bounds handshake work */
void wireguard_tai64n_now(uint8_t *out) {
    uint64_t ms = get_ticks() - clock_ticks;
    if (ms <= tai_last) ms = tai_last + 1;
    tai_last = ms;
    U64TO8_BIG(out, clock_unix + ms / 1000 + UINT64_C(0x400000000000000a));
    U32TO8_BIG(out + 8, (uint32_t)(ms % 1000) * 1000000);
}
static bool leap(unsigned y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
bool wgd_platform_start(const struct wgd_config *cfg, const char **error) {
    *error = "kernel lacks the userspace tunnel ABI; rebuild the kernel";
    if (syscall(SC_TUNNEL, 0, 0, 0) != TUNNEL_CAPABILITY) return false;
    if (!wgd_entropy_start(true, wgd_event, error)) return false;
    RTC_T rtc;
    *error = "valid UTC RTC time is required";
    if (!read_rtc(&rtc) || rtc.year < 2000 || rtc.year > 2199 || rtc.month < 1 || rtc.month > 12 ||
        rtc.day < 1 || rtc.hours > 23 || rtc.minutes > 59 || rtc.seconds > 59) return false;
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (rtc.day > days[rtc.month-1] + (rtc.month == 2 && leap(rtc.year))) return false;
    uint64_t day = 0;
    for (unsigned y = 1970; y < rtc.year; ++y) day += leap(y) ? 366 : 365;
    for (unsigned m = 1; m < rtc.month; ++m) day += days[m-1] + (m == 2 && leap(rtc.year));
    clock_unix = ((day + rtc.day - 1) * 24 + rtc.hours) * 3600 + rtc.minutes * 60 + rtc.seconds;
    clock_ticks = get_ticks();
    *error = "start eth first and wait for an IPv4 address";
    NetStatus_T status;
    uint64_t wait_until = get_ticks() + 10000;
    while (get_net_status(&status) || !status.drv_active || get_net_config(&network) || !network.source || !network.ip[0]) {
        if (get_ticks() >= wait_until) return false;
        sleep_ms(10);
    }
    *error = "tunnel address must differ from the physical IPv4 address";
    if (!memcmp(cfg->address, network.ip, 4)) return false;
    tunnel = *cfg; crypto_zero(tunnel.private_key, 32); crypto_zero(tunnel.preshared_key, 32);
    *error = NULL; return true;
}
bool wgd_platform_attach(const struct wgd_config *cfg) {
    struct wgd_registration registration = {0};
    memcpy(registration.local, cfg->address, 4);
    registration.port = cfg->listen_port; registration.mtu = WGD_MTU;
    registration.count = cfg->route_count; memcpy(registration.routes, cfg->routes, sizeof(registration.routes));
    attached = syscall(SC_TUNNEL, 1, (int64_t)&registration, 0) == 0; return attached;
}
void wgd_platform_detach(void) { if (attached) syscall(SC_TUNNEL, 2, 0, 0); attached = false; }
int wgd_read_config(const char *path, char *text, size_t cap) {
    return (int)read_file_at((const uint8_t *)path, (uint8_t *)text, 0, cap);
}

static void learn(const uint8_t ip[4], const uint8_t mac[6]) {
    if (!ip[0] || (mac[0] & 1)) return;
    for (unsigned i = 0; i < 8; ++i) if (neighbours[i].valid && !memcmp(neighbours[i].ip, ip, 4)) {
        memcpy(neighbours[i].mac, mac, 6); return;
    }
    struct neighbour *n = &neighbours[neighbour_next++ % 8];
    memcpy(n->ip, ip, 4); memcpy(n->mac, mac, 6); n->valid = true;
}
static void arp(const uint8_t ip[4]) {
    uint32_t now = wireguard_sys_now();
    if (arp_at && (uint32_t)(now - arp_at) < 1000) return;
    arp_at = now; memset(tx, 0, 42); memset(tx, 255, 6); memcpy(tx + 6, network.mac, 6);
    tx[12] = 8; tx[13] = 6;
    uint8_t *p = tx + 14; p[1] = 1; p[2] = 8; p[4] = 6; p[5] = 4; p[7] = 1;
    memcpy(p + 8, network.mac, 6); memcpy(p + 14, network.ip, 4); memcpy(p + 24, ip, 4);
    send_eth_frame(tx, 42);
}
static bool mac_for(const uint8_t ip[4], uint8_t mac[6]) {
    bool onlink = true;
    for (unsigned i = 0; i < 4; ++i) if ((ip[i] & network.netmask[i]) != (network.ip[i] & network.netmask[i])) onlink = false;
    const uint8_t *hop = onlink ? ip : network.gateway;
    if (!onlink && memcmp(network.gateway_mac, "\0\0\0\0\0\0", 6)) { memcpy(mac, network.gateway_mac, 6); return true; }
    for (unsigned i = 0; i < 8; ++i) if (neighbours[i].valid && !memcmp(neighbours[i].ip, hop, 4)) { memcpy(mac, neighbours[i].mac, 6); return true; }
    if (hop[0]) arp(hop);
    return false;
}
static uint16_t udp_sum(const uint8_t *ip, const uint8_t *udp, size_t n) {
    uint8_t scratch[12 + WGD_DATAGRAM_MAX + 8];
    memcpy(scratch, ip + 12, 8); scratch[8] = 0; scratch[9] = 17;
    U16TO8_BIG(scratch + 10, n); memcpy(scratch + 12, udp, n);
    return wgd_checksum(scratch, n + 12);
}
bool wgd_platform_send(const uint8_t ip[4], uint16_t port, const uint8_t *data, size_t n) {
    if (n > WGD_DATAGRAM_MAX || !mac_for(ip, tx)) return false;
    memcpy(tx + 6, network.mac, 6); tx[12] = 8; tx[13] = 0;
    uint8_t *p = tx + 14; memset(p, 0, 28); p[0] = 0x45; p[8] = 64; p[9] = 17;
    U16TO8_BIG(p + 2, 28 + n); memcpy(p + 12, network.ip, 4); memcpy(p + 16, ip, 4);
    U16TO8_BIG(p + 10, wgd_checksum(p, 20));
    uint8_t *udp = p + 20; U16TO8_BIG(udp, tunnel.listen_port); U16TO8_BIG(udp + 2, port);
    U16TO8_BIG(udp + 4, n + 8); memcpy(udp + 8, data, n);
    uint16_t sum = udp_sum(p, udp, n + 8); U16TO8_BIG(udp + 6, sum ? sum : 65535);
    return send_eth_frame(tx, 42 + n) == 0;
}
bool wgd_platform_inject(const uint8_t *ip, size_t n) {
    if (n > WGD_MTU) return false;
    memcpy(injected, network.mac, 6); memset(injected + 6, 0, 6);
    injected[12] = 8; injected[13] = 0; memcpy(injected + 14, ip, n);
    return syscall(SC_TUNNEL, 3, (int64_t)injected, 14 + n) == 0;
}
void wgd_platform_poll(struct wgd *wg) {
    uint32_t now = wireguard_sys_now();
    if ((uint32_t)(now - refreshed) >= 1000) {
        NetConfig_T cfg;
        if (!get_net_config(&cfg) && cfg.source && cfg.ip[0]) network = cfg;
        refreshed = now;
    }
    for (unsigned count = 0; count < 32; ++count) {
        int64_t n = receive_data_nb(4, frame);
        if (n <= 0 || n > (int64_t)sizeof(frame)) break;
        if (n >= 42 && frame[12] == 8 && frame[13] == 6 && frame[14] == 0 && frame[15] == 1 &&
            frame[16] == 8 && frame[17] == 0 && frame[18] == 6 && frame[19] == 4 && frame[20] == 0 && frame[21] == 2) {
            learn(frame + 28, frame + 22); continue;
        }
        if (n < 34 || frame[12] != 8 || frame[13] != 0) continue;
        uint8_t *p = frame + 14; size_t total, ihl;
        if (!wgd_ipv4(p, (size_t)n - 14, &total, &ihl)) continue;
        if (!memcmp(p + 12, tunnel.address, 4) && wgd_allowed(&tunnel, p + 16)) { wgd_send_ip(wg, p, total); continue; }
        if (p[9] != 17 || memcmp(p + 16, network.ip, 4) || total < ihl + 8) continue;
        uint8_t *udp = p + ihl; size_t len = ((size_t)udp[4] << 8) | udp[5];
        uint16_t port = ((uint16_t)udp[2] << 8) | udp[3];
        if (port != tunnel.listen_port || len < 8 || len != total - ihl || len - 8 > WGD_DATAGRAM_MAX ||
            ((udp[6] || udp[7]) && udp_sum(p, udp, len) != 0)) continue;
        learn(p + 12, frame + 6);
        wgd_receive(wg, p + 12, ((uint16_t)udp[0] << 8) | udp[1], udp + 8, len - 8);
    }
}
void wgd_platform_sleep(void) { sleep_ms(1); }

void wgd_stats(const struct wgd *wg) {
    char *text = counters; memset(counters, 0, sizeof(counters)); size_t at = 0;
    const char *names[] = {"handshakes=", " rx=", " tx=", " drops=", " replays=", " session="};
    uint32_t values[] = {wg->handshakes, wg->rx_packets, wg->tx_packets, wg->drops, wg->replays, wg->established};
    for (unsigned i = 0; i < 6; ++i) {
        const char *s = names[i]; while (*s) text[at++] = (uint8_t)*s++;
        char digits[10]; unsigned n = 0; uint32_t v = values[i];
        do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) text[at++] = (uint8_t)digits[--n];
    }
    text[at] = '\n';
    save_log();
}
