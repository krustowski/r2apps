#include "bytes.h"
#include "mem.h"
#include "net.h"
#include "printf.h"
#include "syscall.h"

/*
 *  eth — userland Ethernet driver for r2OS
 *
 *  Receives raw Ethernet frames from the kernel via receive_data(RECV_ETH),
 *  handles ARP (who-has → reply) and ICMP echo requests.
 *
 *  Addressing:
 *    eth                  DHCP (DISCOVER → OFFER → REQUEST → ACK, renewed at T1).  If no server
 *                         answers within a few seconds it uses the fallback address meanwhile
 *                         (sysinfo.ip_addr if set, else 10.3.4.2) and keeps asking.
 *    eth --ip X.X.X.X     static address, no DHCP
 *    eth --no-dhcp        static fallback address, no DHCP (the old behaviour)
 *    --gw/--mask/--dns X  for a static address; without them a /24 with the gateway at .1
 *
 *  Whichever it is, the address goes out with its netmask, gateway, the gateway's MAC
 *  (resolved here by ARP) and DNS through syscall 0x3d, where every network stack reads it.
 *
 *  After the IP is resolved it is written to sysinfo so every other process
 *  (GARN /info, TNT ts, etc.) can read the current address.
 *
 */

/* The card's MAC, asked of the kernel once the card is up (net_register);
 * this is only what QEMU gives an RTL8139 unless told otherwise. */
static uint8_t MY_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static uint8_t my_ip[4] = {10, 3, 4, 2}; /* default; matches libcr2 eth_my_ip */

/* The rest of the configuration published with the address (syscall 0x3d),
 * so that every network stack uses the same netmask, gateway and DNS. */
static uint8_t net_mask[4];
static uint8_t net_gw[4];
static uint8_t net_dns[4];
static uint8_t net_source = NET_SOURCE_NONE;
/* The gateway's MAC, resolved here: ARP replies come to the global driver,
 * so no other process can ask for it itself. */
static uint8_t gw_mac[6];
static uint8_t gw_mac_known = 0;
static uint64_t gw_arp_at = 0;

uint8_t debug = 0;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void bzero_buf(uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        p[i] = 0;
}

static void print_mac(const uint8_t mac[6]) { printf((const uint8_t *)"%x:%x:%x:%x:%x:%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); }

static void print_ip(const uint8_t ip[4]) { printf((const uint8_t *)"%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]); }

static uint8_t ip_eq(const uint8_t a[4], const uint8_t b[4]) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

static uint8_t ip_zero(const uint8_t ip[4]) { return !ip[0] && !ip[1] && !ip[2] && !ip[3]; }

/* Parse "10.3.4.2" into ip[4]; returns 1 on success. */
static uint8_t parse_ip_str(const uint8_t *s, uint8_t ip[4]) {
    for (uint8_t oct = 0; oct < 4; oct++) {
        uint16_t v = 0;
        if (*s < '0' || *s > '9')
            return 0;
        while (*s >= '0' && *s <= '9')
            v = (uint16_t)(v * 10 + *s++ - '0');
        ip[oct] = (uint8_t)v;
        if (oct < 3) {
            if (*s++ != '.')
                return 0;
        }
    }
    return 1;
}

/* Build a minimal IPv4 header at buf[0..19].  Caller owns checksum after. */
static void build_ipv4(uint8_t *buf, uint8_t proto, const uint8_t src[4], const uint8_t dst[4], uint16_t payload_len) {
    Ipv4Header_T *h = (Ipv4Header_T *)buf;
    h->version = 0x45;
    h->dscp_ecn = 0;
    h->total_length = htons((uint16_t)(20 + payload_len));
    h->identification = htons(0x1234);
    h->flags_fragment_offset = 0;
    h->ttl = 64;
    h->protocol = proto;
    h->header_checksum = 0;
    memcpy(h->source_addr, src, 4);
    memcpy(h->destination_addr, dst, 4);
    h->header_checksum = htons(inet_cksum(buf, 20));
}

/* Write the resolved IP to sysinfo, which is where GARN, TNT and libcr2 look
 * for it.  The kernel answers Busy while another process holds the system
 * configuration, so try again for a while rather than lose the address. */
static void publish_ip(const uint8_t ip[4]) {
    SysInfo_T si;
    bzero_buf((uint8_t *)&si, sizeof(si));
    memcpy(si.ip_addr, ip, 4);
    for (int tries = 0; tries < 50; tries++) {
        if (write_sysinfo(&si))
            return;
        sleep_ms(10);
    }
    print((const uint8_t *)"-> eth: could not publish the IP address to sysinfo\n");
}

/* Publishes the whole configuration (syscall 0x3d), the IP included, which
 * the kernel also puts into sysinfo.  Busy is retried as for publish_ip();
 * a kernel without the syscall gets the address through sysinfo alone. */
static void publish_config(void) {
    NetConfig_T cfg;
    bzero_buf((uint8_t *)&cfg, sizeof(cfg));
    memcpy(cfg.ip, my_ip, 4);
    memcpy(cfg.netmask, net_mask, 4);
    memcpy(cfg.gateway, net_gw, 4);
    memcpy(cfg.dns, net_dns, 4);
    if (gw_mac_known)
        memcpy(cfg.gateway_mac, gw_mac, 6);
    cfg.source = ip_zero(my_ip) ? NET_SOURCE_NONE : net_source;

    for (int tries = 0; tries < 50; tries++) {
        int64_t r = set_net_config(&cfg);
        if (r == 0)
            return;
        if (r != 0xfa) /* not Busy: an older kernel */
            break;
        sleep_ms(10);
    }
    publish_ip(my_ip);
}

/* Sets the gateway; a different one has to have its MAC resolved again. */
static void set_gateway(const uint8_t gw[4]) {
    if (!ip_eq(gw, net_gw) || !gw_mac_known) {
        memcpy(net_gw, gw, 4);
        gw_mac_known = 0;
        gw_arp_at = 0;
    }
}

/* Netmask and gateway for a static or fallback address the user gave none
 * for: a /24 with the gateway at .1, what this repository's networks use. */
static void default_mask_gw(void) {
    if (ip_zero(net_mask)) {
        net_mask[0] = net_mask[1] = net_mask[2] = 255;
        net_mask[3] = 0;
    }
    if (ip_zero(net_gw)) {
        uint8_t gw[4];
        for (int i = 0; i < 4; i++)
            gw[i] = my_ip[i] & net_mask[i];
        gw[3] |= 1;
        set_gateway(gw);
    }
}

/* ── ARP cache ───────────────────────────────────────────────────── */

#define ARP_CACHE_SIZE 8

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    uint8_t valid;
} ArpEntry_T;

static ArpEntry_T arp_cache[ARP_CACHE_SIZE];

static void arp_cache_update(const uint8_t ip[4], const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid || ip_eq(arp_cache[i].ip, ip)) {
            memcpy(arp_cache[i].ip, ip, 4);
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return;
        }
    }
    memcpy(arp_cache[0].ip, ip, 4);
    memcpy(arp_cache[0].mac, mac, 6);
    arp_cache[0].valid = 1;
}

static void arp_reply(const ArpPkt_T *req, const uint8_t src_mac[6]) {
    uint8_t frame[ETH_HDR_LEN + ARP_PKT_LEN];

    EthHdr_T *eth = (EthHdr_T *)frame;
    memcpy(eth->dst, src_mac, 6);
    memcpy(eth->src, MY_MAC, 6);
    eth->ethertype = htons(ETYPE_ARP);

    ArpPkt_T *arp = (ArpPkt_T *)(frame + ETH_HDR_LEN);
    arp->htype = htons(1);
    arp->ptype = htons(ETYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(2);
    memcpy(arp->sha, MY_MAC, 6);
    memcpy(arp->spa, my_ip, 4);
    memcpy(arp->tha, req->sha, 6);
    memcpy(arp->tpa, req->spa, 4);

    send_eth_frame(frame, (uint32_t)(ETH_HDR_LEN + ARP_PKT_LEN));

    if (debug) {
        print((const uint8_t *)"<< ARP reply to ");
        print_mac(req->sha);
        print((const uint8_t *)"\n");
    }
}

/* Asks for the gateway's MAC, once a second until it answers. */
static void gw_poll(void) {
    if (gw_mac_known || ip_zero(my_ip) || ip_zero(net_gw))
        return;
    uint64_t now = get_ticks();
    if (now < gw_arp_at)
        return;
    gw_arp_at = now + 1000;

    uint8_t frame[ETH_HDR_LEN + ARP_PKT_LEN];
    EthHdr_T *eth = (EthHdr_T *)frame;
    for (int i = 0; i < 6; i++)
        eth->dst[i] = 0xff;
    memcpy(eth->src, MY_MAC, 6);
    eth->ethertype = htons(ETYPE_ARP);

    ArpPkt_T *arp = (ArpPkt_T *)(frame + ETH_HDR_LEN);
    arp->htype = htons(1);
    arp->ptype = htons(ETYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(1);
    memcpy(arp->sha, MY_MAC, 6);
    memcpy(arp->spa, my_ip, 4);
    bzero_buf(arp->tha, 6);
    memcpy(arp->tpa, net_gw, 4);

    send_eth_frame(frame, (uint32_t)(ETH_HDR_LEN + ARP_PKT_LEN));
}

/* ── DHCP client ─────────────────────────────────────────────────── */

/*
 *  RFC 2131, the parts a single host needs: DISCOVER → OFFER → REQUEST → ACK, renewal at T1
 *  (half the lease), starting over on a NAK or when the lease runs out.  Retries back off from
 *  1 s to 16 s.  Everything is timed from get_ticks(), so the receive loop never blocks.
 */

typedef struct {
    uint16_t src_port, dst_port, length, checksum;
} __attribute__((packed)) UdpHdr_T;

typedef enum {
    DHCP_OFF, /* static address, no DHCP at all */
    DHCP_DISCOVERING,
    DHCP_REQUESTING,
    DHCP_BOUND,
    DHCP_RENEWING
} DhcpState_T;

#define DHCP_OPT_LEN 96
#define DHCP_PKT_LEN (236 + 4 + DHCP_OPT_LEN) /* fixed(236) + magic(4) + opts */
#define DHCP_FRAME_LEN (ETH_HDR_LEN + 20 + 8 + DHCP_PKT_LEN)
#define DHCP_RETRY_MIN_MS 1000u
#define DHCP_RETRY_MAX_MS 16000u
#define DHCP_FALLBACK_MS 6000u /* no lease by then: use the fallback address */

static DhcpState_T dhcp_state = DHCP_OFF;
static uint32_t dhcp_xid;
static uint8_t dhcp_offered[4];
static uint8_t dhcp_server[4];
static uint8_t dhcp_mask[4];
static uint8_t dhcp_router[4];
static uint8_t dhcp_dns[4];
static uint32_t dhcp_lease_s;

static uint64_t dhcp_started_ms; /* when this DISCOVER round began */
static uint64_t dhcp_resend_ms;  /* when to send again if nothing comes */
static uint32_t dhcp_backoff_ms;
static uint64_t dhcp_renew_ms;  /* T1 */
static uint64_t dhcp_expire_ms; /* end of the lease */

/* Used until a lease arrives, and for good if none does. */
static uint8_t fallback_ip[4];
static uint8_t on_fallback = 0;

static uint8_t hostname[32];
static uint8_t hostname_len = 0;

static uint8_t dhcp_frame[DHCP_FRAME_LEN]; /* shared send buffer */

static const uint8_t bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t zero_ip[4] = {0, 0, 0, 0};
static const uint8_t bcast_ip[4] = {255, 255, 255, 255};

static void set_ip(const uint8_t ip[4]);

/* The hostname sent in option 12: the system name from sysinfo, cut down to
 * what a hostname may hold (letters, digits, '-'), or "r2". */
static void dhcp_init_hostname(void) {
    SysInfo_T si;
    bzero_buf((uint8_t *)&si, sizeof(si));
    read_sysinfo(&si);
    hostname_len = 0;
    for (uint32_t i = 0; i < sizeof(si.system_name) && si.system_name[i]; i++) {
        uint8_t c = si.system_name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')
            hostname[hostname_len++] = c;
    }
    if (!hostname_len) {
        hostname[0] = 'r';
        hostname[1] = '2';
        hostname_len = 2;
    }
}

/* Fill eth+ip+udp+dhcp fixed headers; return pointer to options area.
 * `ciaddr` is our address when renewing a lease, and zero otherwise. */
static uint8_t *dhcp_build_frame(const uint8_t ciaddr[4]) {
    bzero_buf(dhcp_frame, DHCP_FRAME_LEN);

    EthHdr_T *eth = (EthHdr_T *)dhcp_frame;
    memcpy(eth->dst, bcast_mac, 6);
    memcpy(eth->src, MY_MAC, 6);
    eth->ethertype = htons(ETYPE_IPV4);

    uint16_t udp_len = (uint16_t)(8 + DHCP_PKT_LEN);
    build_ipv4(dhcp_frame + ETH_HDR_LEN, 17, ciaddr, bcast_ip, udp_len);

    UdpHdr_T *udp = (UdpHdr_T *)(dhcp_frame + ETH_HDR_LEN + 20);
    udp->src_port = htons(68);
    udp->dst_port = htons(67);
    udp->length = htons(udp_len);
    udp->checksum = 0; /* RFC 768 allows zero UDP checksum */

    uint8_t *d = dhcp_frame + ETH_HDR_LEN + 20 + 8;
    d[0] = 1; /* op: BOOTREQUEST */
    d[1] = 1; /* htype: Ethernet */
    d[2] = 6; /* hlen */
    d[3] = 0; /* hops */
    d[4] = (uint8_t)(dhcp_xid >> 24);
    d[5] = (uint8_t)(dhcp_xid >> 16);
    d[6] = (uint8_t)(dhcp_xid >> 8);
    d[7] = (uint8_t)(dhcp_xid);
    /* secs: zero */
    if (ip_zero(ciaddr))
        d[10] = 0x80; /* flags: broadcast replies, we cannot take unicast yet */
    memcpy(d + 12, ciaddr, 4);
    memcpy(d + 28, MY_MAC, 6); /* chaddr */
    d[236] = 99;
    d[237] = 130;
    d[238] = 83;
    d[239] = 99; /* magic cookie */

    return d + 240; /* options area */
}

/* The options every message carries: type, client id, hostname, and the
 * parameters we want.  Returns the next free index. */
static uint8_t dhcp_common_opts(uint8_t *opt, uint8_t msg_type) {
    uint8_t i = 0;
    opt[i++] = 53;
    opt[i++] = 1;
    opt[i++] = msg_type;
    opt[i++] = 61;
    opt[i++] = 7;
    opt[i++] = 1; /* client id: Ethernet + MAC */
    memcpy(opt + i, MY_MAC, 6);
    i += 6;
    opt[i++] = 12;
    opt[i++] = hostname_len; /* hostname */
    memcpy(opt + i, hostname, hostname_len);
    i += hostname_len;
    opt[i++] = 55;
    opt[i++] = 6; /* parameter request list */
    opt[i++] = 1;
    opt[i++] = 3;
    opt[i++] = 6;
    opt[i++] = 15;
    opt[i++] = 51;
    opt[i++] = 58;
    return i;
}

static void dhcp_arm_retry(uint64_t now) {
    dhcp_resend_ms = now + dhcp_backoff_ms;
    dhcp_backoff_ms = dhcp_backoff_ms * 2 > DHCP_RETRY_MAX_MS ? DHCP_RETRY_MAX_MS : dhcp_backoff_ms * 2;
}

static void dhcp_send_discover(uint64_t now) {
    uint8_t *opt = dhcp_build_frame(zero_ip);
    uint8_t i = dhcp_common_opts(opt, 1);
    opt[i++] = 255; /* END */

    send_eth_frame(dhcp_frame, DHCP_FRAME_LEN);
    dhcp_state = DHCP_DISCOVERING;
    dhcp_arm_retry(now);
    if (debug)
        print((const uint8_t *)">> DHCP DISCOVER\n");
}

/* A fresh round: new transaction id, backoff from the start. */
static void dhcp_start(uint64_t now) {
    dhcp_xid = (uint32_t)now ^ ((uint32_t)MY_MAC[4] << 24) ^ ((uint32_t)MY_MAC[5] << 16) ^ 0x5a5a1234u;
    dhcp_started_ms = now;
    dhcp_backoff_ms = DHCP_RETRY_MIN_MS;
    dhcp_send_discover(now);
}

/* REQUEST: in answer to an OFFER (`renew` = 0), or to extend the lease we
 * hold (`renew` = 1, from our own address, which the server knows). */
static void dhcp_send_request(uint64_t now, uint8_t renew) {
    uint8_t *opt = dhcp_build_frame(renew ? my_ip : zero_ip);
    uint8_t i = dhcp_common_opts(opt, 3);
    if (!renew) {
        opt[i++] = 50;
        opt[i++] = 4; /* requested IP */
        memcpy(opt + i, dhcp_offered, 4);
        i += 4;
        opt[i++] = 54;
        opt[i++] = 4; /* server identifier */
        memcpy(opt + i, dhcp_server, 4);
        i += 4;
    }
    opt[i++] = 255; /* END */

    send_eth_frame(dhcp_frame, DHCP_FRAME_LEN);
    dhcp_state = renew ? DHCP_RENEWING : DHCP_REQUESTING;
    dhcp_arm_retry(now);
    if (debug) {
        print((const uint8_t *)(renew ? ">> DHCP REQUEST (renew) " : ">> DHCP REQUEST for "));
        print_ip(renew ? my_ip : dhcp_offered);
        print((const uint8_t *)"\n");
    }
}

/* Tells the LAN (switches, the router's ARP table) where our address now lives. */
static void send_gratuitous_arp(void) {
    uint8_t frame[ETH_HDR_LEN + ARP_PKT_LEN];

    EthHdr_T *eth = (EthHdr_T *)frame;
    memcpy(eth->dst, bcast_mac, 6);
    memcpy(eth->src, MY_MAC, 6);
    eth->ethertype = htons(ETYPE_ARP);

    ArpPkt_T *arp = (ArpPkt_T *)(frame + ETH_HDR_LEN);
    arp->htype = htons(1);
    arp->ptype = htons(ETYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(1);
    memcpy(arp->sha, MY_MAC, 6);
    memcpy(arp->spa, my_ip, 4);
    bzero_buf(arp->tha, 6);
    memcpy(arp->tpa, my_ip, 4);

    send_eth_frame(frame, (uint32_t)(ETH_HDR_LEN + ARP_PKT_LEN));
}

static void dhcp_on_ack(uint64_t now, const uint8_t yiaddr[4]) {
    uint8_t changed = !ip_eq(my_ip, yiaddr) || on_fallback || !ip_eq(net_gw, dhcp_router);
    on_fallback = 0;
    memcpy(net_mask, dhcp_mask, 4);
    memcpy(net_dns, dhcp_dns, 4);
    set_gateway(dhcp_router);
    net_source = NET_SOURCE_DHCP;
    set_ip(yiaddr);
    dhcp_state = DHCP_BOUND;

    uint32_t lease = dhcp_lease_s ? dhcp_lease_s : 3600;
    if (lease == 0xFFFFFFFFu) {
        dhcp_renew_ms = dhcp_expire_ms = (uint64_t)-1; /* infinite */
    } else {
        dhcp_renew_ms = now + (uint64_t)lease * 500; /* T1 = lease / 2 */
        dhcp_expire_ms = now + (uint64_t)lease * 1000;
    }

    if (!changed) {
        if (debug)
            print((const uint8_t *)"-> eth: DHCP lease renewed\n");
        return;
    }
    send_gratuitous_arp();

    print((const uint8_t *)"-> eth: DHCP bound ");
    print_ip(my_ip);
    print((const uint8_t *)"  mask ");
    print_ip(dhcp_mask);
    print((const uint8_t *)"  router ");
    print_ip(dhcp_router);
    print((const uint8_t *)"  dns ");
    print_ip(dhcp_dns);
    printf((const uint8_t *)"  lease %ds\n", (int32_t)lease);
}

static void on_dhcp_packet(const uint8_t *d, uint32_t len) {
    if (len < 240)
        return;
    if (d[0] != 2)
        return; /* BOOTREPLY only */
    if (d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99)
        return; /* missing magic cookie */

    uint32_t xid = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | (uint32_t)d[7];
    if (xid != dhcp_xid || memcmp((uint8_t *)d + 28, (uint8_t *)MY_MAC, 6) != 0)
        return;

    /* Walk options */
    uint8_t msg_type = 0;
    uint8_t server_ip[4] = {0, 0, 0, 0};
    uint8_t mask[4] = {0, 0, 0, 0};
    uint8_t router[4] = {0, 0, 0, 0};
    uint8_t dns[4] = {0, 0, 0, 0};
    uint32_t lease = 0;
    uint32_t i = 240;
    while (i < len) {
        uint8_t opt = d[i++];
        if (opt == 255)
            break;
        if (opt == 0)
            continue;
        if (i >= len)
            break;
        uint8_t olen = d[i++];
        if (i + olen > len)
            break;
        if (opt == 53 && olen == 1)
            msg_type = d[i];
        if (opt == 54 && olen == 4)
            memcpy(server_ip, d + i, 4);
        if (opt == 1 && olen == 4)
            memcpy(mask, d + i, 4);
        if (opt == 3 && olen >= 4)
            memcpy(router, d + i, 4); /* first router */
        if (opt == 6 && olen >= 4)
            memcpy(dns, d + i, 4); /* first DNS */
        if (opt == 51 && olen == 4)
            lease = ((uint32_t)d[i] << 24) | ((uint32_t)d[i + 1] << 16) | ((uint32_t)d[i + 2] << 8) | (uint32_t)d[i + 3];
        i += olen;
    }

    uint64_t now = get_ticks();

    if (msg_type == 2 && dhcp_state == DHCP_DISCOVERING) {
        /* OFFER */
        memcpy(dhcp_offered, d + 16, 4); /* yiaddr */
        memcpy(dhcp_server, server_ip, 4);
        if (debug) {
            print((const uint8_t *)"<< DHCP OFFER ");
            print_ip(dhcp_offered);
            print((const uint8_t *)" from ");
            print_ip(dhcp_server);
            print((const uint8_t *)"\n");
        }
        dhcp_backoff_ms = DHCP_RETRY_MIN_MS;
        dhcp_send_request(now, 0);
    } else if (msg_type == 5 && (dhcp_state == DHCP_REQUESTING || dhcp_state == DHCP_RENEWING)) {
        /* ACK */
        if (!ip_zero(server_ip))
            memcpy(dhcp_server, server_ip, 4);
        memcpy(dhcp_mask, mask, 4);
        memcpy(dhcp_router, router, 4);
        memcpy(dhcp_dns, dns, 4);
        dhcp_lease_s = lease;
        dhcp_on_ack(now, d + 16);
    } else if (msg_type == 6 && (dhcp_state == DHCP_REQUESTING || dhcp_state == DHCP_RENEWING)) {
        /* NAK: the address is not ours to have.  Start over. */
        print((const uint8_t *)"-> eth: DHCP NAK, starting over\n");
        dhcp_start(now);
    }
}

/* Timeouts: resend, fall back to the static address, renew, expire.
 * Called on every pass of the receive loop. */
static void dhcp_poll(void) {
    if (dhcp_state == DHCP_OFF)
        return;
    uint64_t now = get_ticks();

    switch (dhcp_state) {
    case DHCP_DISCOVERING:
    case DHCP_REQUESTING:
        if (!on_fallback && ip_zero(my_ip) && now - dhcp_started_ms >= DHCP_FALLBACK_MS) {
            on_fallback = 1;
            memcpy(my_ip, fallback_ip, 4);
            default_mask_gw();
            net_source = NET_SOURCE_FALLBACK;
            set_ip(fallback_ip);
            print((const uint8_t *)"-> eth: no DHCP answer yet, using ");
            print_ip(my_ip);
            print((const uint8_t *)" meanwhile and still asking (see `nic`)\n");
        }
        if (now >= dhcp_resend_ms) {
            /* An unanswered REQUEST goes back to DISCOVER: the offer may be gone. */
            if (dhcp_state == DHCP_REQUESTING && dhcp_backoff_ms >= 8000u)
                dhcp_start(now);
            else if (dhcp_state == DHCP_REQUESTING)
                dhcp_send_request(now, 0);
            else
                dhcp_send_discover(now);
        }
        break;
    case DHCP_BOUND:
        if (now >= dhcp_renew_ms) {
            dhcp_backoff_ms = DHCP_RETRY_MIN_MS;
            dhcp_send_request(now, 1);
        }
        break;
    case DHCP_RENEWING:
        if (now >= dhcp_expire_ms) {
            print((const uint8_t *)"-> eth: DHCP lease expired\n");
            set_ip(zero_ip);
            dhcp_start(now);
        } else if (now >= dhcp_resend_ms) {
            dhcp_send_request(now, 1);
        }
        break;
    default:
        break;
    }
}

/* ── Ethernet frame dispatcher ───────────────────────────────────── */

static void on_eth_frame(const uint8_t *buf, uint32_t len) {
    if (len < ETH_HDR_LEN)
        return;

    const EthHdr_T *eth = (const EthHdr_T *)buf;
    uint16_t etype = htons(eth->ethertype);

    if (etype == ETYPE_ARP) {
        if (len < ETH_HDR_LEN + ARP_PKT_LEN)
            return;
        if (ip_zero(my_ip))
            return; /* not bound yet — ignore ARP */

        const ArpPkt_T *arp = (const ArpPkt_T *)(buf + ETH_HDR_LEN);

        /* Anything the gateway says (a reply to our question, or a question
         * of its own) tells its MAC, which the other processes need. */
        if (!ip_zero(net_gw) && ip_eq(arp->spa, net_gw) &&
            (!gw_mac_known || memcmp((uint8_t *)gw_mac, (uint8_t *)arp->sha, 6) != 0)) {
            memcpy(gw_mac, arp->sha, 6);
            gw_mac_known = 1;
            publish_config();
            if (debug) {
                print((const uint8_t *)"<< gateway ");
                print_ip(net_gw);
                print((const uint8_t *)" is at ");
                print_mac(gw_mac);
                print((const uint8_t *)"\n");
            }
        }

        if (htons(arp->oper) != 1)
            return; /* requests only */
        if (!ip_eq(arp->tpa, my_ip))
            return; /* not for us */

        arp_cache_update(arp->spa, eth->src);

        if (debug) {
            print((const uint8_t *)">> ARP who-has ");
            print_ip(arp->tpa);
            print((const uint8_t *)" tell ");
            print_ip(arp->spa);
            print((const uint8_t *)"\n");
        }
        arp_reply(arp, eth->src);

    } else if (etype == ETYPE_IPV4) {
        const uint8_t *ip_payload = buf + ETH_HDR_LEN;
        uint32_t ip_len = len - (uint32_t)ETH_HDR_LEN;

        Ipv4Header_T ipv4;
        uint16_t ipv4_hdr_len = parse_ipv4_packet(ip_payload, &ipv4);
        if (!ipv4_hdr_len)
            return;

        uint32_t ip_total = (uint32_t)htons(ipv4.total_length);
        if (ip_total > ip_len || ip_total < (uint32_t)ipv4_hdr_len)
            return;

        if (ipv4.protocol == 1) {
            /* ICMP — only reply when bound */
            if (ip_zero(my_ip))
                return;

            uint32_t icmp_len = ip_total - ipv4_hdr_len;
            if (icmp_len < 8)
                return;

            const uint8_t *icmp_pkt = ip_payload + ipv4_hdr_len;
            if (icmp_pkt[0] != 8 || icmp_pkt[1] != 0)
                return;

            if (debug)
                print((const uint8_t *)">> ICMP echo — replying\n");

            uint32_t frame_len = (uint32_t)ETH_HDR_LEN + ip_total;
            if (frame_len > 1514)
                return;
            uint8_t reply[1514];

            EthHdr_T *reth = (EthHdr_T *)reply;
            memcpy(reth->dst, eth->src, 6);
            memcpy(reth->src, MY_MAC, 6);
            reth->ethertype = htons(ETYPE_IPV4);

            uint8_t *rip = reply + ETH_HDR_LEN;
            memcpy(rip, ip_payload, ipv4_hdr_len);
            Ipv4Header_T *rip_hdr = (Ipv4Header_T *)rip;
            uint8_t tmp[4];
            memcpy(tmp, rip_hdr->source_addr, 4);
            memcpy(rip_hdr->source_addr, rip_hdr->destination_addr, 4);
            memcpy(rip_hdr->destination_addr, tmp, 4);
            rip_hdr->header_checksum = 0;
            rip_hdr->header_checksum = htons(inet_cksum(rip, ipv4_hdr_len));

            uint8_t *ricmp = rip + ipv4_hdr_len;
            memcpy(ricmp, icmp_pkt, icmp_len);
            ricmp[0] = 0;
            ricmp[2] = 0;
            ricmp[3] = 0;
            uint16_t ck = inet_cksum(ricmp, icmp_len);
            ricmp[2] = (uint8_t)(ck >> 8);
            ricmp[3] = (uint8_t)(ck & 0xff);

            send_eth_frame(reply, frame_len);

        } else if (ipv4.protocol == 17) {
            /* UDP — only DHCP client port 68 is relevant here */
            const uint8_t *udp_pkt = ip_payload + ipv4_hdr_len;
            uint32_t udp_len = ip_total - ipv4_hdr_len;
            if (udp_len < 8)
                return;
            uint16_t dst_port = (uint16_t)((udp_pkt[2] << 8) | udp_pkt[3]);
            if (dst_port == 68)
                on_dhcp_packet(udp_pkt + 8, udp_len - 8);
        }
    }
}

/* Adopts `ip` as ours and tells everyone else: the configuration (syscall
 * 0x3d) and sysinfo. */
static void set_ip(const uint8_t ip[4]) {
    memcpy(my_ip, ip, 4);
    publish_config();
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    uint8_t use_static = 0;
    uint8_t no_dhcp = 0;

    for (int i = 1; i < argc; i++) {
        if (memcmp((uint8_t *)argv[i], (uint8_t *)"debug", 6) == 0) {
            debug = 1;
        } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"--no-dhcp", 10) == 0) {
            no_dhcp = 1;
        } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"--ip", 5) == 0 && i + 1 < argc) {
            if (parse_ip_str((const uint8_t *)argv[i + 1], my_ip))
                use_static = 1;
            else
                print((const uint8_t *)"-> eth: bad --ip address, ignoring it\n");
            i++;
        } else if (i + 1 < argc && (memcmp((uint8_t *)argv[i], (uint8_t *)"--gw", 5) == 0 ||
                                    memcmp((uint8_t *)argv[i], (uint8_t *)"--mask", 7) == 0 ||
                                    memcmp((uint8_t *)argv[i], (uint8_t *)"--dns", 6) == 0)) {
            /* For a static address (--ip, --no-dhcp); DHCP brings its own. */
            uint8_t *dst = argv[i][2] == 'g' ? net_gw : argv[i][2] == 'm' ? net_mask : net_dns;
            if (!parse_ip_str((const uint8_t *)argv[i + 1], dst)) {
                print((const uint8_t *)"-> eth: bad address after ");
                print((const uint8_t *)argv[i]);
                print((const uint8_t *)", ignoring it\n");
                bzero_buf(dst, 4);
            }
            i++;
        }
    }

    if (!use_static) {
        /* An address already in sysinfo (eth started before) beats the built-in default. */
        SysInfo_T si;
        si.system_path[0] = si.system_path[31] = '\0';
        read_sysinfo(&si);
        si.system_path[31] = '\0';
        if (!ip_zero(si.ip_addr))
            memcpy(my_ip, si.ip_addr, 4);
    }

    int64_t reg = net_register();
    if (debug)
        printf((const uint8_t *)"-> net_register() = %d\n", (int32_t)reg);
    if (reg != 0) {
        print((const uint8_t *)"-> eth: no supported network card (RTL8139, Intel e1000/e1000e/PCH)\n");
        return 1;
    }

    NetStatus_T ns;
    bzero_buf((uint8_t *)&ns, sizeof(ns));
    if (get_net_status(&ns) == 0 && (ns.mac[0] | ns.mac[1] | ns.mac[2] | ns.mac[3] | ns.mac[4] | ns.mac[5]))
        memcpy(MY_MAC, ns.mac, 6);

    print((const uint8_t *)"-> eth driver start  MAC: ");
    print_mac(MY_MAC);
    if (use_static || no_dhcp) {
        default_mask_gw();
        net_source = NET_SOURCE_STATIC;
        set_ip(my_ip);
        print((const uint8_t *)"  IP: ");
        print_ip(my_ip);
        print((const uint8_t *)" (static)  mask ");
        print_ip(net_mask);
        print((const uint8_t *)"  gw ");
        print_ip(net_gw);
        print((const uint8_t *)"\n");
    } else {
        /* DHCP; my_ip is what to fall back to if no server answers. */
        memcpy(fallback_ip, my_ip, 4);
        memcpy(my_ip, zero_ip, 4);
        print((const uint8_t *)"  IP: DHCP\n");
        dhcp_init_hostname();
        dhcp_start(get_ticks());
    }

    uint8_t frame_buf[1514];
    uint32_t rx_count = 0;

    for (;;) {
        /* Blocking receive when there is nothing to time; otherwise poll, so
         * DHCP retries and renewals, and the gateway's ARP, happen with the
         * line quiet. */
        uint8_t timed = dhcp_state != DHCP_OFF || (!gw_mac_known && !ip_zero(net_gw));
        int64_t n = timed ? receive_data_nb(RECV_ETH, frame_buf) : receive_data(RECV_ETH, frame_buf);
        if (n > 0) {
            rx_count++;
            if (debug) {
                printf((const uint8_t *)"RX[%d]: %d bytes  etype=0x%x\n", rx_count, (int32_t)n, (uint32_t)htons(((EthHdr_T *)frame_buf)->ethertype));
            }
            on_eth_frame(frame_buf, (uint32_t)n);
        } else {
            sleep_ms(10);
        }
        dhcp_poll();
        gw_poll();
    }

    return 0;
}
