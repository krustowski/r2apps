#include "bytes.h"
#include "mem.h"
#include "net.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

/*
 *  nsk — the networking swiss knife
 *
 *  Host discovery for the rou2exOS kernel.  Takes an IPv4 subnet in CIDR
 *  notation (e.g. 10.3.4.0/30), sweeps every address in it with ARP requests
 *  and ICMP echo requests, and lists the machines that answered.
 *
 *  Replies (ARP replies, ICMP echo replies) are delivered by the kernel to
 *  the process registered as the global Ethernet driver only, so nsk claims
 *  that role for the duration of the scan (syscall 0x37).  Do not run eth.elf
 *  at the same time — whoever registers first owns the NIC.  While nsk holds
 *  it, ARP who-has requests for the local address are answered too.
 *
 *  krusty@vxn.dev / Sep 14, 2026
 */

#define NSK_MAX_TARGETS 1024
#define NSK_ICMP_ID 0x4e53 /* 'NS' */
#define NSK_TIMEOUT_MS 1000
#define NSK_PACE_EVERY 16 /* yield to the NIC after this many probes */
#define NSK_ECHO_DATA 8

static uint8_t debug = 0;

static uint8_t my_ip[4] = {10, 3, 4, 2};
static uint8_t my_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

static const uint8_t bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t zero_mac[6] = {0, 0, 0, 0, 0, 0};

/*
 *  type Target_T structure
 *
 *  One scanned address plus everything learnt about it during the sweep.
 */
typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    uint8_t have_mac;
    uint8_t via_arp;
    uint8_t via_icmp;
    uint8_t is_self;
    uint32_t sent_ms;
    uint8_t have_sent;
    uint32_t rtt_ms;
    uint8_t have_rtt;
} Target_T;

static Target_T targets[NSK_MAX_TARGETS];
static uint32_t n_targets = 0;
static uint32_t n_up = 0;
static uint32_t base_u32 = 0; /* address of targets[0] */

static uint8_t rx_frame[1514];
static uint8_t tx_frame[1514];

/* ── Small helpers ───────────────────────────────────────────────── */

static void bzero_buf(uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}

static int str_eq(const uint8_t *a, const uint8_t *b) {
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }

    return *a == *b;
}

static uint8_t ip_eq(const uint8_t a[4], const uint8_t b[4]) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

static uint8_t ip_zero(const uint8_t ip[4]) { return !ip[0] && !ip[1] && !ip[2] && !ip[3]; }

static uint32_t ip_to_u32(const uint8_t ip[4]) { return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | (uint32_t)ip[3]; }

static void u32_to_ip(uint32_t v, uint8_t ip[4]) {
    ip[0] = (uint8_t)(v >> 24);
    ip[1] = (uint8_t)(v >> 16);
    ip[2] = (uint8_t)(v >> 8);
    ip[3] = (uint8_t)v;
}

/* Parse "10.3.4.2" into ip[4]; returns the pointer past the address, or 0. */
static const uint8_t *parse_ip_str(const uint8_t *s, uint8_t ip[4]) {
    for (uint8_t oct = 0; oct < 4; oct++) {
        uint16_t v = 0;
        if (*s < '0' || *s > '9')
            return 0;
        while (*s >= '0' && *s <= '9') v = (uint16_t)(v * 10 + *s++ - '0');
        if (v > 255)
            return 0;
        ip[oct] = (uint8_t)v;
        if (oct < 3 && *s++ != '.')
            return 0;
    }

    return s;
}

/* Parse "10.3.4.0/30" into ip[4] + prefix; a missing /len means /32. */
static uint8_t parse_cidr(const uint8_t *s, uint8_t ip[4], uint8_t *prefix) {
    const uint8_t *p = parse_ip_str(s, ip);

    if (!p)
        return 0;

    if (*p == '\0') {
        *prefix = 32;

        return 1;
    }

    if (*p != '/')
        return 0;
    p++;

    if (*p < '0' || *p > '9')
        return 0;

    uint16_t v = 0;
    while (*p >= '0' && *p <= '9') v = (uint16_t)(v * 10 + *p++ - '0');

    if (*p != '\0' || v > 32)
        return 0;

    *prefix = (uint8_t)v;

    return 1;
}

static uint32_t parse_u32(const uint8_t *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');

    return v;
}

static uint8_t *str_u8(uint8_t v, uint8_t *p) {
    if (v >= 100)
        *p++ = (uint8_t)('0' + v / 100);
    if (v >= 10)
        *p++ = (uint8_t)('0' + (v / 10) % 10);
    *p++ = (uint8_t)('0' + v % 10);

    return p;
}

static void fmt_ip(const uint8_t ip[4], uint8_t *out) {
    uint8_t *p = out;

    for (uint8_t i = 0; i < 4; i++) {
        p = str_u8(ip[i], p);
        if (i < 3)
            *p++ = '.';
    }
    *p = '\0';
}

/* Print <s> padded with spaces up to <width> columns. */
static void print_col(const uint8_t *s, uint32_t width) {
    uint32_t len = strlen(s);

    print(s);
    for (uint32_t i = len; i < width; i++) print((const uint8_t *)" ");
}

static void print_ip(const uint8_t ip[4]) {
    uint8_t buf[16];

    fmt_ip(ip, buf);
    print(buf);
}

static void print_mac(const uint8_t mac[6]) {
    static const uint8_t hexd[] = "0123456789abcdef";
    uint8_t buf[18];
    uint8_t *p = buf;

    for (uint8_t i = 0; i < 6; i++) {
        *p++ = hexd[mac[i] >> 4];
        *p++ = hexd[mac[i] & 0x0f];
        if (i < 5)
            *p++ = ':';
    }
    *p = '\0';

    print(buf);
}

/* ── Frame builders ──────────────────────────────────────────────── */

static void build_eth(uint8_t *frame, const uint8_t dst[6], uint16_t etype) {
    EthHdr_T *eth = (EthHdr_T *)frame;

    memcpy(eth->dst, dst, 6);
    memcpy(eth->src, my_mac, 6);
    eth->ethertype = htons(etype);
}

static void build_ipv4(uint8_t *buf, uint8_t proto, const uint8_t src[4], const uint8_t dst[4], uint16_t payload_len, uint16_t ident) {
    Ipv4Header_T *h = (Ipv4Header_T *)buf;

    h->version = 0x45;
    h->dscp_ecn = 0;
    h->total_length = htons((uint16_t)(20 + payload_len));
    h->identification = htons(ident);
    h->flags_fragment_offset = 0;
    h->ttl = 64;
    h->protocol = proto;
    h->header_checksum = 0;
    memcpy(h->source_addr, src, 4);
    memcpy(h->destination_addr, dst, 4);
    h->header_checksum = htons(inet_cksum(buf, 20));
}

static void send_arp_request(const uint8_t dst_ip[4]) {
    uint8_t frame[ETH_HDR_LEN + ARP_PKT_LEN];

    build_eth(frame, bcast_mac, ETYPE_ARP);

    ArpPkt_T *arp = (ArpPkt_T *)(frame + ETH_HDR_LEN);
    arp->htype = htons(1);
    arp->ptype = htons(ETYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(1);
    memcpy(arp->sha, my_mac, 6);
    memcpy(arp->spa, my_ip, 4);
    memcpy(arp->tha, zero_mac, 6);
    memcpy(arp->tpa, dst_ip, 4);

    send_eth_frame(frame, (uint32_t)(ETH_HDR_LEN + ARP_PKT_LEN));
}

static void send_arp_reply(const uint8_t dst_mac[6], const uint8_t dst_ip[4]) {
    uint8_t frame[ETH_HDR_LEN + ARP_PKT_LEN];

    build_eth(frame, dst_mac, ETYPE_ARP);

    ArpPkt_T *arp = (ArpPkt_T *)(frame + ETH_HDR_LEN);
    arp->htype = htons(1);
    arp->ptype = htons(ETYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(2);
    memcpy(arp->sha, my_mac, 6);
    memcpy(arp->spa, my_ip, 4);
    memcpy(arp->tha, dst_mac, 6);
    memcpy(arp->tpa, dst_ip, 4);

    send_eth_frame(frame, (uint32_t)(ETH_HDR_LEN + ARP_PKT_LEN));
}

static void send_icmp_echo(const uint8_t dst_ip[4], const uint8_t dst_mac[6], uint16_t seq) {
    uint8_t frame[ETH_HDR_LEN + 20 + 8 + NSK_ECHO_DATA];
    uint32_t icmp_len = 8 + NSK_ECHO_DATA;

    bzero_buf(frame, sizeof(frame));
    build_eth(frame, dst_mac, ETYPE_IPV4);
    build_ipv4(frame + ETH_HDR_LEN, 1, my_ip, dst_ip, (uint16_t)icmp_len, (uint16_t)(0x4e00 + (seq & 0xff)));

    uint8_t *icmp = frame + ETH_HDR_LEN + 20;
    icmp[0] = 8; /* echo request */
    icmp[1] = 0;
    icmp[2] = 0;
    icmp[3] = 0;
    icmp[4] = (uint8_t)(NSK_ICMP_ID >> 8);
    icmp[5] = (uint8_t)(NSK_ICMP_ID & 0xff);
    icmp[6] = (uint8_t)(seq >> 8);
    icmp[7] = (uint8_t)(seq & 0xff);
    memcpy(icmp + 8, (const uint8_t *)"nsk-r2!\n", NSK_ECHO_DATA);

    uint16_t ck = inet_cksum(icmp, icmp_len);
    icmp[2] = (uint8_t)(ck >> 8);
    icmp[3] = (uint8_t)(ck & 0xff);

    send_eth_frame(frame, (uint32_t)sizeof(frame));
}

/* Turn a received echo request around and send it back to its sender. */
static void send_icmp_reply(const uint8_t dst_mac[6], const uint8_t *ip_pkt, uint16_t hdr_len, const uint8_t *icmp, uint32_t icmp_len) {
    uint32_t frame_len = (uint32_t)ETH_HDR_LEN + hdr_len + icmp_len;

    if (frame_len > sizeof(tx_frame))
        return;

    build_eth(tx_frame, dst_mac, ETYPE_IPV4);

    uint8_t *rip = tx_frame + ETH_HDR_LEN;
    memcpy(rip, ip_pkt, hdr_len);

    Ipv4Header_T *h = (Ipv4Header_T *)rip;
    memcpy(h->destination_addr, h->source_addr, 4);
    memcpy(h->source_addr, my_ip, 4);
    h->total_length = htons((uint16_t)(hdr_len + icmp_len));
    h->ttl = 64;
    h->header_checksum = 0;
    h->header_checksum = htons(inet_cksum(rip, hdr_len));

    uint8_t *ricmp = rip + hdr_len;
    memcpy(ricmp, icmp, (uint16_t)icmp_len);
    ricmp[0] = 0; /* echo reply */
    ricmp[2] = 0;
    ricmp[3] = 0;

    uint16_t ck = inet_cksum(ricmp, icmp_len);
    ricmp[2] = (uint8_t)(ck >> 8);
    ricmp[3] = (uint8_t)(ck & 0xff);

    send_eth_frame(tx_frame, frame_len);
}

/* ── Result bookkeeping ──────────────────────────────────────────── */

/* Index of <ip> in the target list, or -1 when it is out of the scanned range. */
static int32_t target_index(const uint8_t ip[4]) {
    uint32_t v = ip_to_u32(ip);

    if (v < base_u32)
        return -1;

    uint32_t idx = v - base_u32;

    if (idx >= n_targets || !ip_eq(targets[idx].ip, ip))
        return -1;

    return (int32_t)idx;
}

static void mark_up(int32_t idx, uint8_t via_arp, const uint8_t mac[6], uint8_t solicited) {
    Target_T *t = &targets[idx];
    uint8_t first = !t->via_arp && !t->via_icmp;

    if (via_arp)
        t->via_arp = 1;
    else
        t->via_icmp = 1;

    if (mac && !t->have_mac) {
        memcpy(t->mac, mac, 6);
        t->have_mac = 1;
    }

    /* Only a reply to one of our own probes carries a meaningful round trip. */
    if (solicited && !t->have_rtt && t->have_sent) {
        uint32_t now = (uint32_t)get_ticks();
        t->rtt_ms = (now >= t->sent_ms) ? now - t->sent_ms : 0;
        t->have_rtt = 1;
    }

    if (!first)
        return;

    n_up++;

    print((const uint8_t *)"<< ");
    print_ip(t->ip);
    print((const uint8_t *)" is up");

    if (t->have_mac) {
        print((const uint8_t *)"  ");
        print_mac(t->mac);
    }

    printf((const uint8_t *)"  (%s", via_arp ? (const uint8_t *)"arp" : (const uint8_t *)"icmp");

    if (t->have_rtt)
        printf((const uint8_t *)", %u ms", t->rtt_ms);

    print((const uint8_t *)")\n");
}

/* ── Receive path ────────────────────────────────────────────────── */

static void on_arp(const ArpPkt_T *arp) {
    uint16_t oper = htons(arp->oper);

    if (oper == 2) {
        /* Reply — the sender is alive. */
        int32_t idx = target_index(arp->spa);
        if (idx >= 0)
            mark_up(idx, 1, arp->sha, 1);
        else if (debug) {
            print((const uint8_t *)">> arp reply from out-of-range ");
            print_ip(arp->spa);
            print((const uint8_t *)"\n");
        }

        return;
    }

    if (oper != 1)
        return;

    /* Request — a host that asks for our address is alive as well. */
    int32_t idx = target_index(arp->spa);
    if (idx >= 0)
        mark_up(idx, 1, arp->sha, 0);

    if (ip_eq(arp->tpa, my_ip) && !ip_zero(my_ip))
        send_arp_reply(arp->sha, arp->spa);
}

static void on_ipv4(const uint8_t *ip_pkt, uint32_t ip_len, const EthHdr_T *eth) {
    Ipv4Header_T ipv4;
    uint16_t hdr_len = parse_ipv4_packet(ip_pkt, &ipv4);

    if (!hdr_len)
        return;

    uint32_t total = (uint32_t)htons(ipv4.total_length);

    if (total > ip_len || total < (uint32_t)hdr_len)
        return;

    if (ipv4.protocol != 1)
        return;

    uint32_t icmp_len = total - hdr_len;
    const uint8_t *icmp = ip_pkt + hdr_len;

    if (icmp_len < 8)
        return;

    if (icmp[0] == 0) {
        /* Echo reply — ours only. */
        uint16_t id = (uint16_t)((icmp[4] << 8) | icmp[5]);
        if (id != NSK_ICMP_ID)
            return;

        int32_t idx = target_index(ipv4.source_addr);
        if (idx >= 0)
            mark_up(idx, 0, eth->src, 1);

        return;
    }

    if (icmp[0] == 8) {
        /* Somebody is pinging us — the sender is alive, and the box should
         * stay pingable while nsk holds the NIC. */
        int32_t idx = target_index(ipv4.source_addr);
        if (idx >= 0)
            mark_up(idx, 0, eth->src, 0);

        if (ip_eq(ipv4.destination_addr, my_ip) && !ip_zero(my_ip))
            send_icmp_reply(eth->src, ip_pkt, hdr_len, icmp, icmp_len);

        return;
    }

    if (debug && icmp[0] == 3) {
        print((const uint8_t *)">> icmp unreachable from ");
        print_ip(ipv4.source_addr);
        print((const uint8_t *)"\n");
    }
}

static void on_frame(const uint8_t *buf, uint32_t len) {
    if (len < ETH_HDR_LEN)
        return;

    const EthHdr_T *eth = (const EthHdr_T *)buf;
    uint16_t etype = htons(eth->ethertype);

    if (memcmp(eth->src, my_mac, 6) == 0)
        return; /* our own probe looped back by the host bridge */

    if (etype == ETYPE_ARP) {
        if (len < ETH_HDR_LEN + ARP_PKT_LEN)
            return;

        on_arp((const ArpPkt_T *)(buf + ETH_HDR_LEN));
    } else if (etype == ETYPE_IPV4) {
        on_ipv4(buf + ETH_HDR_LEN, len - (uint32_t)ETH_HDR_LEN, eth);
    }
}

/* Drain the kernel RX queue; returns the number of frames processed. */
static uint32_t nsk_drain(void) {
    uint32_t seen = 0;

    for (uint32_t guard = 0; guard < 64; guard++) {
        int64_t n = receive_data_nb(RECV_ETH, rx_frame);

        if (n <= 0)
            break;

        seen++;
        on_frame(rx_frame, (uint32_t)n);
    }

    return seen;
}

/* Keep draining for <ms> milliseconds, yielding the CPU when idle. */
static void nsk_wait(uint32_t ms) {
    uint64_t t0 = get_ticks();

    for (;;) {
        if (get_ticks() - t0 >= (uint64_t)ms)
            return;

        if (!nsk_drain())
            sleep_ms(1);
    }
}

/* ── Sweeps ──────────────────────────────────────────────────────── */

static void sweep_arp(void) {
    print((const uint8_t *)">> arp sweep\n");

    for (uint32_t i = 0; i < n_targets; i++) {
        if (targets[i].is_self)
            continue;

        targets[i].sent_ms = (uint32_t)get_ticks();
        targets[i].have_sent = 1;
        send_arp_request(targets[i].ip);
        nsk_drain();

        if ((i % NSK_PACE_EVERY) == (NSK_PACE_EVERY - 1))
            sleep_ms(1);
    }
}

static void sweep_icmp(uint8_t all) {
    print((const uint8_t *)">> icmp sweep\n");

    for (uint32_t i = 0; i < n_targets; i++) {
        Target_T *t = &targets[i];

        if (t->is_self)
            continue;
        if (!all && (t->via_arp || t->via_icmp))
            continue;

        /* Unicast when the MAC is known, broadcast otherwise — the host
         * bridge routes it and the owner of the address answers. */
        const uint8_t *dst_mac = t->have_mac ? t->mac : bcast_mac;

        t->sent_ms = (uint32_t)get_ticks();
        t->have_sent = 1;
        t->have_rtt = 0;
        send_icmp_echo(t->ip, dst_mac, (uint16_t)i);
        nsk_drain();

        if ((i % NSK_PACE_EVERY) == (NSK_PACE_EVERY - 1))
            sleep_ms(1);
    }
}

/* ── Report ──────────────────────────────────────────────────────── */

static void report(uint32_t elapsed_ms) {
    print((const uint8_t *)"\n-> live hosts:\n");

    if (!n_up) {
        print((const uint8_t *)"   (none)\n");
    }

    for (uint32_t i = 0; i < n_targets; i++) {
        Target_T *t = &targets[i];

        if (!t->is_self && !t->via_arp && !t->via_icmp)
            continue;

        uint8_t ip_str[16];
        fmt_ip(t->ip, ip_str);

        print((const uint8_t *)"   ");
        print_col(ip_str, 17);

        if (t->have_mac)
            print_mac(t->mac);
        else
            print((const uint8_t *)"                 ");

        if (t->is_self)
            print((const uint8_t *)"  self");
        else if (t->via_arp && t->via_icmp)
            print((const uint8_t *)"  arp+icmp");
        else if (t->via_arp)
            print((const uint8_t *)"  arp");
        else
            print((const uint8_t *)"  icmp");

        if (t->have_rtt)
            printf((const uint8_t *)"  %u ms", t->rtt_ms);

        print((const uint8_t *)"\n");
    }

    printf((const uint8_t *)"\n-> %u up / %u scanned in %u ms\n", n_up, n_targets, elapsed_ms);
}

static void usage(void) {
    print((const uint8_t *)"Usage: nsk [options] <CIDR>\n"
                           "\n"
                           "  --arp            ARP sweep only, even off-link\n"
                           "  --icmp           ICMP echo sweep only\n"
                           "  --timeout <ms>   reply wait per sweep, default 1000\n"
                           "  --ip <addr>      override the local IPv4 address\n"
                           "  debug            verbose mode\n"
                           "\n"
                           "Example: nsk 10.3.4.0/30\n");
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const uint8_t *cidr = 0;
    uint32_t timeout_ms = NSK_TIMEOUT_MS;
    uint8_t arp_only = 0;
    uint8_t icmp_only = 0;
    uint8_t ip_override = 0;

    for (int i = 1; i < argc; i++) {
        const uint8_t *a = (const uint8_t *)argv[i];

        if (str_eq(a, (const uint8_t *)"debug")) {
            debug = 1;
        } else if (str_eq(a, (const uint8_t *)"--arp")) {
            arp_only = 1;
        } else if (str_eq(a, (const uint8_t *)"--icmp")) {
            icmp_only = 1;
        } else if (str_eq(a, (const uint8_t *)"--timeout") && i + 1 < argc) {
            timeout_ms = parse_u32((const uint8_t *)argv[++i]);
        } else if (str_eq(a, (const uint8_t *)"--ip") && i + 1 < argc) {
            uint8_t tmp_ip[4];

            /* Only adopt the address once it has parsed in full. */
            if (parse_ip_str((const uint8_t *)argv[++i], tmp_ip)) {
                memcpy(my_ip, tmp_ip, 4);
                ip_override = 1;
            }
        } else if (str_eq(a, (const uint8_t *)"--help") || str_eq(a, (const uint8_t *)"-h")) {
            usage();

            return 0;
        } else if (!cidr && a[0] >= '0' && a[0] <= '9') {
            cidr = a;
        }
    }

    if (!cidr) {
        usage();

        return 1;
    }

    uint8_t base[4];
    uint8_t prefix = 32;

    if (!parse_cidr(cidr, base, &prefix)) {
        printf((const uint8_t *)"-> nsk: cannot parse '%s' as CIDR\n", cidr);

        return 1;
    }

    uint32_t mask = prefix ? (0xffffffffu << (32 - prefix)) : 0;
    uint32_t network = ip_to_u32(base) & mask;
    uint32_t bcast = network | ~mask;
    uint32_t first = network;
    uint32_t last = bcast;

    /* /31 and /32 have no network/broadcast address to skip. */
    if (prefix <= 30) {
        first = network + 1;
        last = bcast - 1;
    }

    uint32_t count = last - first + 1;

    if (count > NSK_MAX_TARGETS) {
        printf((const uint8_t *)"-> nsk: %u addresses is too many, the limit is %u (use /22 or narrower)\n", count, (uint32_t)NSK_MAX_TARGETS);

        return 1;
    }

    /* Local address: --ip wins, then sysinfo (published by the ETH driver). */
    if (!ip_override) {
        SysInfo_T si;
        si.system_path[0] = si.system_path[31] = '\0';
        read_sysinfo(&si);
        si.system_path[31] = '\0';

        if (!ip_zero(si.ip_addr))
            memcpy(my_ip, si.ip_addr, 4);
    }

    NetStatus_T ns;
    uint8_t drv_was_active = 0;

    if (get_net_status(&ns) >= 0) {
        drv_was_active = ns.drv_active;

        if (memcmp(ns.mac, zero_mac, 6) != 0)
            memcpy(my_mac, ns.mac, 6);
        if (!ip_override && ip_zero(my_ip) && !ip_zero(ns.ip))
            memcpy(my_ip, ns.ip, 4);
    }

    net_register();

    base_u32 = first;
    n_targets = count;

    for (uint32_t i = 0; i < count; i++) {
        u32_to_ip(first + i, targets[i].ip);
        targets[i].is_self = ip_eq(targets[i].ip, my_ip);

        if (targets[i].is_self) {
            memcpy(targets[i].mac, my_mac, 6);
            targets[i].have_mac = 1;
            n_up++;
        }
    }

    /* ARP only reaches the local link.  The scanned prefix is not the local
     * netmask (r2 publishes no netmask), so treat the range as local when it
     * contains our address, or when the whole range shares our /24. */
    uint32_t local_u32 = ip_to_u32(my_ip);
    uint8_t same_subnet = (local_u32 & mask) == network;

    if (!same_subnet) {
        uint32_t l24 = 0xffffff00u;
        same_subnet = ((local_u32 & l24) == (first & l24)) && ((local_u32 & l24) == (last & l24));
    }

    printf((const uint8_t *)"-> nsk: scanning %s (%u address%s)\n", cidr, count, count == 1 ? (const uint8_t *)"" : (const uint8_t *)"es");
    print((const uint8_t *)"-> local ");
    print_ip(my_ip);
    print((const uint8_t *)"  ");
    print_mac(my_mac);
    print((const uint8_t *)"\n");

    if (drv_was_active)
        print((const uint8_t *)"-> warning: another ETH driver is registered, replies may not reach nsk\n");

    uint8_t do_arp = !icmp_only || arp_only;
    uint8_t do_icmp = !arp_only || icmp_only;

    if (do_arp && !same_subnet && !arp_only) {
        print((const uint8_t *)"-> target range is off-link, skipping the arp sweep\n");
        do_arp = 0;
        do_icmp = 1;
    }

    uint64_t t_start = get_ticks();

    if (do_arp) {
        sweep_arp();
        nsk_wait(timeout_ms);
    }

    if (do_icmp) {
        sweep_icmp(icmp_only);
        nsk_wait(timeout_ms);
    }

    report((uint32_t)(get_ticks() - t_start));

    return 0;
}
