#include "bytes.h"
#include "mem.h"
#include "net.h"
#include "printf.h"

/*
 *  eth.c — userland Ethernet driver for r2OS
 *
 *  Receives raw Ethernet frames from the kernel via receive_data(RECV_ETH, buf),
 *  handles ARP (who-has -> reply), and dispatches IPv4 up to the existing IP/ICMP stack.
 *
 *  Requires the kernel to expose raw Ethernet frames through the 0x35/0x36 syscall
 *  pair with a new type code (RECV_ETH = 0x04).
 *
 *  krusty@vxn.dev
 */

/* Driver config */
static const uint8_t MY_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8_t MY_IP[4] = {10, 3, 4, 2};

/* Kernel data-port type for raw Ethernet frames */
#define RECV_ETH 0x04
#define SEND_ETH 0x04

/* Ethertype constants */
#define ETYPE_IPV4 0x0800
#define ETYPE_ARP 0x0806

/* Ethernet header */
typedef struct {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype; /* big-endian */
} __attribute__((packed)) EthHdr_T;

/* ARP packet (IPv4 over Ethernet) */
typedef struct {
    uint16_t htype; /* 1 = Ethernet */
    uint16_t ptype; /* 0x0800 = IPv4 */
    uint8_t hlen;   /* 6 */
    uint8_t plen;   /* 4 */
    uint16_t oper;  /* 1 = request, 2 = reply */
    uint8_t sha[6]; /* sender MAC */
    uint8_t spa[4]; /* sender IP */
    uint8_t tha[6]; /* target MAC */
    uint8_t tpa[4]; /* target IP */
} __attribute__((packed)) ArpPkt_T;

#define ETH_HDR_LEN sizeof(EthHdr_T)
#define ARP_PKT_LEN sizeof(ArpPkt_T)

/* Helpers */
static void print_mac(const uint8_t mac[6]) { printf((const uint8_t *)"%x:%x:%x:%x:%x:%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); }

static void print_ip(const uint8_t ip[4]) { printf((const uint8_t *)"%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]); }

static uint8_t ip_eq(const uint8_t a[4], const uint8_t b[4]) { return (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]); }

static uint16_t inet_cksum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)(((uint32_t)data[i] << 8) | data[i + 1]);
    if (len & 1)
        sum += (uint32_t)((uint32_t)data[len - 1] << 8);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Send a raw Ethernet frame */
static void eth_send(uint8_t *frame, uint32_t len) { send_eth_frame(frame, len); }

/* Handle ARP request: craft and send a reply */
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
    arp->oper = htons(2); /* reply */
    memcpy(arp->sha, MY_MAC, 6);
    memcpy(arp->spa, MY_IP, 4);
    memcpy(arp->tha, req->sha, 6);
    memcpy(arp->tpa, req->spa, 4);

    eth_send(frame, (uint32_t)(ETH_HDR_LEN + ARP_PKT_LEN));

    print((const uint8_t *)"<< ARP reply sent to ");
    print_mac(req->sha);
    print((const uint8_t *)"\n");
}

/* Dispatch one Ethernet frame */
static void on_eth_frame(const uint8_t *buf, uint32_t len) {
    if (len < ETH_HDR_LEN)
        return;

    const EthHdr_T *eth = (const EthHdr_T *)buf;
    uint16_t etype = htons(eth->ethertype); /* htons doubles as ntohs */

    if (etype == ETYPE_ARP) {
        if (len < ETH_HDR_LEN + ARP_PKT_LEN)
            return;

        const ArpPkt_T *arp = (const ArpPkt_T *)(buf + ETH_HDR_LEN);

        if (htons(arp->oper) != 1)
            return; /* only handle requests */
        if (!ip_eq(arp->tpa, MY_IP))
            return; /* not for us */

        print((const uint8_t *)">> ARP who-has ");
        print_ip(arp->tpa);
        print((const uint8_t *)" tell ");
        print_ip(arp->spa);
        print((const uint8_t *)"\n");

        arp_reply(arp, eth->src);

    } else if (etype == ETYPE_IPV4) {
        const uint8_t *ip_payload = buf + ETH_HDR_LEN;
        uint32_t ip_len = len - (uint32_t)ETH_HDR_LEN;

        Ipv4Header_T ipv4;
        uint16_t ipv4_hdr_len = parse_ipv4_packet(ip_payload, &ipv4);
        if (!ipv4_hdr_len)
            return;

        if (ipv4.protocol == 1) { /* ICMP */
            /* Use total_length from the IPv4 header — authoritative, independent of NIC framing */
            uint32_t ip_total = (uint32_t)htons(ipv4.total_length);
            if (ip_total > ip_len || ip_total < (uint32_t)ipv4_hdr_len)
                return;
            uint32_t icmp_len = ip_total - ipv4_hdr_len;
            if (icmp_len < 8)
                return;

            const uint8_t *icmp_pkt = ip_payload + ipv4_hdr_len;
            if (icmp_pkt[0] != 8 || icmp_pkt[1] != 0)
                return; /* not an echo request */

            print((const uint8_t *)">> ICMP echo request — sending reply\n");

            /* Build Ethernet + IPv4 + ICMP echo reply entirely in userland */
            uint32_t frame_len = (uint32_t)ETH_HDR_LEN + ip_total;
            if (frame_len > 1514)
                return;
            uint8_t reply[1514];

            /* Ethernet header */
            EthHdr_T *reth = (EthHdr_T *)reply;
            memcpy(reth->dst, eth->src, 6);
            memcpy(reth->src, MY_MAC, 6);
            reth->ethertype = htons(ETYPE_IPV4);

            /* IPv4 header — copy from request, swap src/dst, recompute checksum */
            uint8_t *rip = reply + ETH_HDR_LEN;
            memcpy(rip, ip_payload, ipv4_hdr_len);
            Ipv4Header_T *rip_hdr = (Ipv4Header_T *)rip;
            uint8_t tmp[4];
            memcpy(tmp, rip_hdr->source_addr, 4);
            memcpy(rip_hdr->source_addr, rip_hdr->destination_addr, 4);
            memcpy(rip_hdr->destination_addr, tmp, 4);
            rip_hdr->header_checksum = 0;
            rip_hdr->header_checksum = htons(inet_cksum(rip, ipv4_hdr_len));

            /* ICMP: copy payload, change type to 0 (reply), recompute checksum */
            uint8_t *ricmp = rip + ipv4_hdr_len;
            memcpy(ricmp, icmp_pkt, icmp_len);
            ricmp[0] = 0; /* type = echo reply */
            ricmp[2] = 0;
            ricmp[3] = 0;
            uint16_t ck = inet_cksum(ricmp, icmp_len);
            ricmp[2] = (uint8_t)(ck >> 8);
            ricmp[3] = (uint8_t)(ck & 0xff);

            eth_send(reply, frame_len);
        }
    }
}

int main(void) {
    print((const uint8_t *)"-> eth driver start  MAC: ");
    print_mac(MY_MAC);
    print((const uint8_t *)"  IP: ");
    print_ip(MY_IP);
    print((const uint8_t *)"\n");

    /* Register with the kernel as the Ethernet driver.
     * This stores our PID in netdrv and initialises the RTL8139 NIC. */
    int64_t reg = net_register();
    printf((const uint8_t *)"-> net_register() = %d\n", (int32_t)reg);

    /* Max standard Ethernet frame */
    uint8_t frame_buf[1514];
    uint32_t rx_count = 0;

    for (;;) {
        int64_t n = receive_data(RECV_ETH, frame_buf);
        if (n <= 0)
            continue;

        rx_count++;
        printf((const uint8_t *)"RX[%d]: %d bytes  etype=0x%x\n", rx_count, (int32_t)n, (uint32_t)htons(((EthHdr_T *)frame_buf)->ethertype));

        on_eth_frame(frame_buf, (uint32_t)n);
    }

    return 0;
}
