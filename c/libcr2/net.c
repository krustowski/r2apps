#include "net.h"
#include "bytes.h"
#include "printf.h"
#include "string.h"

/* Network driver abstraction */

static uint8_t slip_temp_buf[2048];
static uint32_t slip_temp_len = 0;
static uint8_t slip_frame_buf[2048];

static int slip_recv(uint8_t *buf, uint32_t maxlen) {
    uint32_t value = 0;

    if (!serial_read(&value))
        return 0;
    if (slip_temp_len < sizeof(slip_temp_buf)) {
        slip_temp_buf[slip_temp_len++] = (uint8_t)value;
    }

    int64_t decoded = decode_slip(slip_temp_buf, slip_temp_len, slip_frame_buf, sizeof(slip_frame_buf));

    if (decoded > 0) {
        slip_temp_len = 0;
        uint32_t n = ((uint32_t)decoded < maxlen) ? (uint32_t)decoded : maxlen;
        memcpy(buf, slip_frame_buf, n);
        return (int)n;
    } else if (decoded < 0) {
        slip_temp_len = 0;
    }

    return 0;
}

static void slip_send(const uint8_t *pkt, uint32_t len) {
    (void)len;
    send_packet(0x01, (uint8_t *)pkt);
}

/* Ethernet driver (direct, no IPC) */

static uint8_t eth_my_mac[6];
static uint8_t eth_my_ip[4];

void net_get_local_ip(uint8_t ip[4]) { memcpy(ip, eth_my_ip, 4); }
void net_get_local_mac(uint8_t mac[6]) { memcpy(mac, eth_my_mac, 6); }

#define ETH_ARP_CACHE_SIZE 8

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    uint8_t valid;
} EthArpEntry_T;

static EthArpEntry_T eth_arp_cache[ETH_ARP_CACHE_SIZE];

void eth_arp_cache_update(const uint8_t ip[4], const uint8_t mac[6]) {
    for (int i = 0; i < ETH_ARP_CACHE_SIZE; i++) {
        if (!eth_arp_cache[i].valid || memcmp(eth_arp_cache[i].ip, ip, 4) == 0) {
            memcpy(eth_arp_cache[i].ip, ip, 4);
            memcpy(eth_arp_cache[i].mac, mac, 6);
            eth_arp_cache[i].valid = 1;

            return;
        }
    }

    memcpy(eth_arp_cache[0].ip, ip, 4);
    memcpy(eth_arp_cache[0].mac, mac, 6);
    eth_arp_cache[0].valid = 1;
}

void net_arp_set(const uint8_t ip[4], const uint8_t mac[6]) { eth_arp_cache_update(ip, mac); }

static uint8_t eth_arp_cache_lookup(const uint8_t ip[4], uint8_t mac_out[6]) {
    for (int i = 0; i < ETH_ARP_CACHE_SIZE; i++) {
        if (eth_arp_cache[i].valid && memcmp(eth_arp_cache[i].ip, ip, 4) == 0) {
            memcpy(mac_out, eth_arp_cache[i].mac, 6);

            return 1;
        }
    }

    return 0;
}

static void eth_send_arp_reply(const EthHdr_T *req_eth, const ArpPkt_T *req_arp) {
    uint8_t frame[ETH_HDR_LEN + ARP_PKT_LEN];
    EthHdr_T *eth = (EthHdr_T *)frame;

    memcpy(eth->dst, req_eth->src, 6);
    memcpy(eth->src, eth_my_mac, 6);
    eth->ethertype = htons(ETYPE_ARP);

    ArpPkt_T *arp = (ArpPkt_T *)(frame + ETH_HDR_LEN);
    arp->htype = htons(1);
    arp->ptype = htons(ETYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(2);

    memcpy(arp->sha, eth_my_mac, 6);
    memcpy(arp->spa, eth_my_ip, 4);
    memcpy(arp->tha, req_arp->sha, 6);
    memcpy(arp->tpa, req_arp->spa, 4);

    send_eth_frame(frame, ETH_HDR_LEN + ARP_PKT_LEN);
}

static uint8_t eth_drv_frame_buf[1514];

static int eth_drv_recv(uint8_t *buf, uint32_t maxlen) {
    int64_t n = receive_data(RECV_ETH, eth_drv_frame_buf);

    if (n <= 0 || (uint32_t)n < ETH_HDR_LEN)
        return 0;

    const EthHdr_T *eth = (const EthHdr_T *)eth_drv_frame_buf;
    uint16_t etype = htons(eth->ethertype);

    if (etype == ETYPE_ARP) {
        if ((uint32_t)n < ETH_HDR_LEN + ARP_PKT_LEN)
            return 0;
        const ArpPkt_T *arp = (const ArpPkt_T *)(eth_drv_frame_buf + ETH_HDR_LEN);
        if (htons(arp->oper) == 1 && memcmp(arp->tpa, eth_my_ip, 4) == 0) {
            /* ARP request for our IP — reply and learn sender */
            eth_arp_cache_update(arp->spa, eth->src);
            eth_send_arp_reply(eth, arp);
        } else if (htons(arp->oper) == 2 && memcmp(arp->tha, eth_my_mac, 6) == 0) {
            /* ARP reply to our request — learn sender's MAC */
            eth_arp_cache_update(arp->spa, eth->src);
        }
        return 0;
    }

    if (etype == ETYPE_IPV4) {
        const uint8_t *ip_pkt = eth_drv_frame_buf + ETH_HDR_LEN;
        uint32_t ip_len = (uint32_t)n - ETH_HDR_LEN;

        Ipv4Header_T ipv4;
        uint16_t ipv4_hdr_len = parse_ipv4_packet(ip_pkt, &ipv4);

        if (!ipv4_hdr_len)
            return 0;

        /* Learn sender's MAC for outgoing replies */
        eth_arp_cache_update(ipv4.source_addr, eth->src);

        if (ipv4.protocol == 1) {
            /* ICMP — handle inline, don't expose to caller */
            uint32_t ip_total = (uint32_t)htons(ipv4.total_length);

            if (ip_total > ip_len || ip_total < (uint32_t)ipv4_hdr_len)
                return 0;

            uint32_t icmp_len = ip_total - ipv4_hdr_len;
            const uint8_t *icmp = ip_pkt + ipv4_hdr_len;

            if (icmp_len < 8 || icmp[0] != 8 || icmp[1] != 0)
                return 0; /* not an echo request */

            uint32_t frame_len = ETH_HDR_LEN + ip_total;

            if (frame_len > 1514)
                return 0;

            uint8_t reply[1514];
            EthHdr_T *reth = (EthHdr_T *)reply;

            memcpy(reth->dst, eth->src, 6);
            memcpy(reth->src, eth_my_mac, 6);

            reth->ethertype = htons(ETYPE_IPV4);
            uint8_t *rip = reply + ETH_HDR_LEN;

            memcpy(rip, ip_pkt, ipv4_hdr_len);

            Ipv4Header_T *rip_hdr = (Ipv4Header_T *)rip;
            uint8_t tmp[4];

            memcpy(tmp, rip_hdr->source_addr, 4);
            memcpy(rip_hdr->source_addr, rip_hdr->destination_addr, 4);
            memcpy(rip_hdr->destination_addr, tmp, 4);

            rip_hdr->header_checksum = 0;
            rip_hdr->header_checksum = htons(inet_cksum(rip, ipv4_hdr_len));

            uint8_t *ricmp = rip + ipv4_hdr_len;

            memcpy(ricmp, icmp, icmp_len);

            ricmp[0] = 0;
            ricmp[2] = 0;
            ricmp[3] = 0;
            uint16_t ck = inet_cksum(ricmp, icmp_len);
            ricmp[2] = (uint8_t)(ck >> 8);
            ricmp[3] = (uint8_t)(ck & 0xff);

            send_eth_frame(reply, frame_len);

            return 0;
        }

        /* Return raw IPv4 packet to the caller (TCP, UDP, …).
         * Use total_length from the IPv4 header, not ip_len, to strip any
         * Ethernet minimum-frame padding (NIC pads short frames to 60 bytes). */
        uint32_t ip_total = (uint32_t)htons(ipv4.total_length);
        if (ip_total > ip_len || ip_total < (uint32_t)ipv4_hdr_len)
            return 0;

        uint32_t copy_len = (ip_total < maxlen) ? ip_total : maxlen;
        memcpy(buf, ip_pkt, copy_len);

        return (int)copy_len;
    }

    return 0;
}

/*
 *  Non-blocking wrapper around eth_drv_recv.  Uses receive_data_nb so the
 *  caller returns immediately with 0 if no Ethernet frame is queued, instead
 *  of being suspended by the scheduler.  ARP/ICMP are still handled in-line
 *  when a frame IS available; only TCP payloads are returned to the caller.
 */
static int eth_drv_recv_nb(uint8_t *buf, uint32_t maxlen) {
    int64_t n = receive_data_nb(RECV_ETH, eth_drv_frame_buf);

    if (n <= 0 || (uint32_t)n < ETH_HDR_LEN)
        return 0;

    const EthHdr_T *eth = (const EthHdr_T *)eth_drv_frame_buf;
    uint16_t etype = htons(eth->ethertype);

    if (etype == ETYPE_ARP) {
        if ((uint32_t)n < ETH_HDR_LEN + ARP_PKT_LEN)
            return 0;
        const ArpPkt_T *arp = (const ArpPkt_T *)(eth_drv_frame_buf + ETH_HDR_LEN);
        if (htons(arp->oper) == 1 && memcmp(arp->tpa, eth_my_ip, 4) == 0) {
            eth_arp_cache_update(arp->spa, eth->src);
            eth_send_arp_reply(eth, arp);
        } else if (htons(arp->oper) == 2 && memcmp(arp->tha, eth_my_mac, 6) == 0) {
            eth_arp_cache_update(arp->spa, eth->src);
        }
        return 0;
    }

    if (etype == ETYPE_IPV4) {
        const uint8_t *ip_pkt = eth_drv_frame_buf + ETH_HDR_LEN;
        uint32_t ip_len = (uint32_t)n - ETH_HDR_LEN;

        Ipv4Header_T ipv4;
        uint16_t ipv4_hdr_len = parse_ipv4_packet(ip_pkt, &ipv4);

        if (!ipv4_hdr_len)
            return 0;

        eth_arp_cache_update(ipv4.source_addr, eth->src);

        if (ipv4.protocol == 1) {
            uint32_t ip_total = (uint32_t)htons(ipv4.total_length);
            if (ip_total > ip_len || ip_total < (uint32_t)ipv4_hdr_len)
                return 0;
            uint32_t icmp_len = ip_total - ipv4_hdr_len;
            const uint8_t *icmp = ip_pkt + ipv4_hdr_len;
            if (icmp_len < 8 || icmp[0] != 8 || icmp[1] != 0)
                return 0;
            uint32_t frame_len = ETH_HDR_LEN + ip_total;
            if (frame_len > 1514)
                return 0;
            uint8_t reply[1514];
            EthHdr_T *reth = (EthHdr_T *)reply;
            memcpy(reth->dst, eth->src, 6);
            memcpy(reth->src, eth_my_mac, 6);
            reth->ethertype = htons(ETYPE_IPV4);
            uint8_t *rip = reply + ETH_HDR_LEN;
            memcpy(rip, ip_pkt, ipv4_hdr_len);
            Ipv4Header_T *rip_hdr = (Ipv4Header_T *)rip;
            uint8_t tmp[4];
            memcpy(tmp, rip_hdr->source_addr, 4);
            memcpy(rip_hdr->source_addr, rip_hdr->destination_addr, 4);
            memcpy(rip_hdr->destination_addr, tmp, 4);
            rip_hdr->header_checksum = 0;
            rip_hdr->header_checksum = htons(inet_cksum(rip, ipv4_hdr_len));
            uint8_t *ricmp = rip + ipv4_hdr_len;
            memcpy(ricmp, icmp, icmp_len);
            ricmp[0] = 0;
            ricmp[2] = 0;
            ricmp[3] = 0;
            uint16_t ck = inet_cksum(ricmp, icmp_len);
            ricmp[2] = (uint8_t)(ck >> 8);
            ricmp[3] = (uint8_t)(ck & 0xff);
            send_eth_frame(reply, frame_len);
            return 0;
        }

        uint32_t ip_total = (uint32_t)htons(ipv4.total_length);
        if (ip_total > ip_len || ip_total < (uint32_t)ipv4_hdr_len)
            return 0;
        uint32_t copy_len = (ip_total < maxlen) ? ip_total : maxlen;
        memcpy(buf, ip_pkt, copy_len);
        return (int)copy_len;
    }

    return 0;
}

int net_recv_nb(uint8_t *buf, uint32_t maxlen) { return eth_drv_recv_nb(buf, maxlen); }

static void eth_drv_send(const uint8_t *ip_pkt, uint32_t len) {
    (void)len;
    Ipv4Header_T ipv4;
    uint16_t hdr_len = parse_ipv4_packet(ip_pkt, &ipv4);

    if (!hdr_len)
        return;

    uint32_t ip_total = (uint32_t)htons(ipv4.total_length);

    if (ip_total < hdr_len || ip_total > 1500)
        return;

    uint8_t pkt[1500];
    memcpy(pkt, ip_pkt, ip_total);
    Ipv4Header_T *hdr = (Ipv4Header_T *)pkt;

    /* send_tcp_packet builds a dummy IPv4 header with src/dst inverted for the kernel;
     * ensure source=eth_my_ip. */
    if (memcmp(hdr->source_addr, eth_my_ip, 4) != 0) {
        uint8_t tmp[4];
        memcpy(tmp, hdr->source_addr, 4);
        memcpy(hdr->source_addr, hdr->destination_addr, 4);
        memcpy(hdr->destination_addr, tmp, 4);
    }
    hdr->header_checksum = 0;
    hdr->header_checksum = htons(inet_cksum(pkt, hdr_len));

    /* Fix TCP header byte order.
     *
     * send_tcp_packet stores all TcpHeader_T fields in host byte order.
     * CRAFT_TCP_PACKET computes a checksum over those raw bytes but does not
     * reformat the fields — the SLIP send path does the final swap to network
     * order before the wire, but for Ethernet we must do it ourselves.
     *
     * Detection: in network byte order pkt[hdr_len] holds the data-offset byte
     * whose high nibble is 5 (standard 20-byte header).  In host byte order
     * that position holds the flags byte instead (SYN=0x02, ACK=0x10, …)
     * whose high nibble is never 5. */
    if (ipv4.protocol == 6 && ip_total >= hdr_len + (uint32_t)sizeof(TcpHeader_T)) {
        uint8_t *tcpp = pkt + hdr_len;
        TcpHeader_T *t = (TcpHeader_T *)tcpp;

        if ((tcpp[12] >> 4) != 5) {
            /* Host byte order — swap every multi-byte field to network order */
            t->source_port = swap16(t->source_port);
            t->dest_port = swap16(t->dest_port);
            t->seq_num = swap32(t->seq_num);
            t->ack_num = swap32(t->ack_num);
            t->data_offset_reserved_flags = swap16(t->data_offset_reserved_flags);
            t->window_size = swap16(t->window_size);
            t->urgent_pointer = swap16(t->urgent_pointer);
        }

        /* Recompute TCP checksum over the now-network-byte-order segment */
        uint32_t tcp_len = ip_total - hdr_len;

        t->checksum = 0;
        uint32_t sum = 0;

        sum += ((uint32_t)hdr->source_addr[0] << 8) | hdr->source_addr[1];
        sum += ((uint32_t)hdr->source_addr[2] << 8) | hdr->source_addr[3];
        sum += ((uint32_t)hdr->destination_addr[0] << 8) | hdr->destination_addr[1];
        sum += ((uint32_t)hdr->destination_addr[2] << 8) | hdr->destination_addr[3];
        sum += 6; /* protocol = TCP */
        sum += tcp_len;

        for (uint32_t i = 0; i + 1 < tcp_len; i += 2)
            sum += ((uint32_t)tcpp[i] << 8) | tcpp[i + 1];

        if (tcp_len & 1)
            sum += (uint32_t)tcpp[tcp_len - 1] << 8;

        while (sum >> 16)
            sum = (sum & 0xffff) + (sum >> 16);

        t->checksum = htons((uint16_t)~sum);
    }

    uint8_t dst_mac[6];
    int _arp_hit = eth_arp_cache_lookup(hdr->destination_addr, dst_mac);
    if (!_arp_hit || memcmp(dst_mac, eth_my_mac, 6) == 0) {
        /* ARP unresolved, OR resolved to our own MAC (the remote is on the
         * same guest — e.g. Memento → chat server, both at 10.3.4.2).
         * Using own_mac as dst would be L2-dropped by the host tap driver.
         * Broadcast forces the host to receive the frame and route it back
         * via ip_forward so the other process's RX queue gets it. */
        for (int _i = 0; _i < 6; _i++)
            dst_mac[_i] = 0xFF;
    }

    uint32_t frame_len = ETH_HDR_LEN + ip_total;
    uint8_t frame[1514];
    EthHdr_T *feth = (EthHdr_T *)frame;

    memcpy(feth->dst, dst_mac, 6);
    memcpy(feth->src, eth_my_mac, 6);
    feth->ethertype = htons(ETYPE_IPV4);
    memcpy(frame + ETH_HDR_LEN, pkt, ip_total);

    send_eth_frame(frame, frame_len);
}

NetDriver_T net_drv;

/* Wall-clock seconds published by the application through net_set_time().
 * 0 means no clock was ever supplied, which disables socket_reap(). */
static uint32_t net_now = 0;

/* Remembers which transport is bound so net_set_nonblocking() knows which
 * pair of receive functions it is choosing between. */
static uint8_t net_drv_is_eth = 0;

/* How long a socket may sit in FIN_WAIT before socket_reap() takes the slot
 * back.  The peer normally completes the close in well under a second; this
 * only covers peers that vanish mid-handshake. */
#define FIN_WAIT_TIMEOUT_SECS 5

/* Per-packet tracing, off by default: send_tcp_packet() runs for every
 * segment (including every ACK), so tracing it unconditionally floods the
 * kernel console and slows transfers to the speed of the serial writes. */
static uint8_t net_debug = 0;

void net_set_debug(uint8_t on) { net_debug = on ? 1 : 0; }

void net_set_time(uint32_t secs) { net_now = secs; }

void net_set_nonblocking(uint8_t on) {
    if (net_drv_is_eth)
        net_drv.recv = on ? eth_drv_recv_nb : eth_drv_recv;

    /* SLIP needs nothing: slip_recv() already returns 0 on an empty line. */
}

int net_driver_select(const uint8_t *name) {
    if (name && name[0] == 'e') {
        static const uint8_t my_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        static const uint8_t my_ip[4] = {10, 3, 4, 2};

        memcpy(eth_my_mac, my_mac, 6);
        memcpy(eth_my_ip, my_ip, 4);

        net_register();
        net_drv.recv = eth_drv_recv;
        net_drv.send_ip = eth_drv_send;
        net_drv_is_eth = 1;

        return 0;
    }

    /* Default: SLIP over serial */
    net_drv.recv = slip_recv;
    net_drv.send_ip = slip_send;
    net_drv_is_eth = 0;

    if (!serial_init())
        return -1;

    return 0;
}

int net_driver_bind_port(const uint8_t *name, uint16_t port) {
    if (name && name[0] == 'e') {
        static const uint8_t my_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        static const uint8_t my_ip[4] = {10, 3, 4, 2};

        memcpy(eth_my_mac, my_mac, 6);
        memcpy(eth_my_ip, my_ip, 4);

        /* Become the global driver if nobody else has registered yet.
         * The kernel's register_driver is idempotent: first caller wins,
         * subsequent calls are no-ops.  This lets GARN or TNT bootstrap
         * the NIC without a separate eth.elf process. */
        net_register();
        net_bind_port(port);
        net_drv.recv = eth_drv_recv;
        net_drv.send_ip = eth_drv_send;
        net_drv_is_eth = 1;

        return 0;
    }

    /* SLIP: no port-level demux — behaves the same as net_driver_select */
    net_drv.recv = slip_recv;
    net_drv.send_ip = slip_send;
    net_drv_is_eth = 0;

    if (!serial_init())
        return -1;

    return 0;
}

/*
 *  SLIP decoder
 */

// Returns number of decoded bytes, or -1 on protocol error, or 0 if frame not yet complete
int64_t decode_slip(const uint8_t *input, uint32_t input_len, uint8_t *output, uint32_t output_len) {
    uint32_t out_pos = 0;
    uint8_t escape = 0;

    for (uint32_t i = 0; i < input_len; ++i) {
        uint8_t b = input[i];

        if (b == SLIP_END) {
            if (out_pos > 0) {
                // Frame complete
                return (int64_t)out_pos;
            }
            // Else ignore leading END
            continue;
        }

        if (b == SLIP_ESC) {
            escape = 1;
            continue;
        }

        if (escape) {
            if (b == SLIP_ESC_END) {
                b = SLIP_END;
            } else if (b == SLIP_ESC_ESC) {
                b = SLIP_ESC;
            } else {
                // Protocol error
                return -1;
            }
            escape = 0;
        }

        if (out_pos >= output_len) {
            // Output buffer overflow
            return -1;
        }

        output[out_pos++] = b;
    }

    // Not finished yet
    return 0;
}

void icmp_make_reply(uint8_t *packet, uint32_t len) {
    if (len < 8)
        return;

    packet[0] = 0; /* type = echo reply */
    packet[2] = 0;
    packet[3] = 0;

    uint16_t ck = inet_cksum(packet, len);

    packet[2] = (uint8_t)(ck >> 8);
    packet[3] = (uint8_t)(ck & 0xff);
}

uint16_t parse_ipv4_packet(const uint8_t *packet, Ipv4Header_T *header) {
    uint16_t header_len = 0;
    // uint16_t packet_len = 0;

    memcpy(header, packet, sizeof(Ipv4Header_T));
    header_len = (header->version & 0x0F) * 4;

    /*while (packet[packet_len]) ++packet_len;

      if (packet_len < header_len)
      {
      return 0;
      }*/

    return header_len;
}

uint8_t parse_icmp_packet(const uint8_t *packet, IcmpHeader_T *header) {
    uint8_t header_len = 0;
    // uint16_t packet_len = 0;

    memcpy(header, packet, sizeof(IcmpHeader_T));

    // ICMP header is always 8 bytes
    header_len = 8;

    /*while (packet[packet_len]) ++packet_len;

      if (packet_len < header_len)
      {
      return 0;
      }*/

    return header_len;
}

uint16_t parse_tcp_packet(const uint8_t *packet, TcpHeader_T *header) {
    uint16_t header_len = 20;

    memcpy(header, packet, sizeof(TcpHeader_T));

    header->source_port = swap16(header->source_port);
    header->dest_port = swap16(header->dest_port);
    header->ack_num = swap32(header->ack_num);
    header->seq_num = swap32(header->seq_num);
    header->data_offset_reserved_flags = htons(header->data_offset_reserved_flags);

    return header_len;
}

TcpSocket_T *socket_tcp4(TcpSocket_T sockets[MAX_SOCKETS]) {
    TcpSocket_T *sock = alloc_socket(sockets);
    if (!sock) {
        return 0;
    }

    sock->state = SOCKET_CLOSED;

    return sock;
}

void bind(TcpSocket_T *sock, uint16_t port) { sock->local_port = port; }

void listen(TcpSocket_T *sock) { sock->state = SOCKET_LISTENING; }

TcpSocket_T *accept(TcpSocket_T *listener, TcpSocket_T sockets[MAX_SOCKETS]) {
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        TcpSocket_T *s = &sockets[i];

        if (!s->used || s == listener || s->local_port != listener->local_port)
            continue;

        /* CLOSE_WAIT counts too: the peer half-closed after sending its
         * request, which is still sitting unread in the RX buffer. */
        if (s->state != SOCKET_ESTABLISHED && s->state != SOCKET_CLOSE_WAIT)
            continue;

        if (s->rx_len > 0)
            return s;
    }

    return 0;
}

uint32_t read(TcpSocket_T *sock, uint8_t *buf, uint32_t maxlen) {
    uint32_t n = (sock->rx_len < maxlen) ? sock->rx_len : maxlen;

    for (uint32_t i = 0; i < n; i++) {
        buf[i] = sock->rx_buffer[i];
    }

    sock->rx_len = 0;

    return n;
}

uint32_t write(TcpSocket_T *sock, const uint8_t *buf, uint32_t len) {
    send_tcp_packet(sock, buf, len, TCP_FLAG_ACK);

    /* Sending is activity.  This is what keeps a server-push stream (an SSE
     * connection, say) from being reaped: the peer never sends anything on it,
     * so inbound traffic alone would make it look abandoned. */
    sock->last_activity = net_now;

    return len;
}

void close(TcpSocket_T *sock) {
    if (sock->state == SOCKET_ESTABLISHED || sock->state == SOCKET_CLOSE_WAIT) {
        /* Send the FIN but keep the slot allocated.  Releasing it here would
         * leave the peer's ACK-of-FIN — and its own FIN — arriving at a socket
         * whose used flag is already 0, which on_tcp_packet() skips without a
         * word.  The peer then waits for an acknowledgement that never comes
         * and only gives up on its own timeout.  on_tcp_packet() finishes the
         * handshake; socket_reap() and alloc_socket() bound the wait. */
        send_tcp_packet(sock, 0, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
        sock->state = SOCKET_FIN_WAIT;
        sock->last_activity = net_now;

        return;
    }

    if (sock->state == SOCKET_FIN_WAIT)
        return; /* close already under way — a second FIN would desync the peer */

    if (sock->state == SOCKET_SYN_SENT)
        send_tcp_packet(sock, 0, 0, TCP_FLAG_RST);

    free_socket(sock);
}

/* Find the socket owning a 4-tuple, skipping <skip> (the listener) and any
 * socket still in LISTENING state. */
static TcpSocket_T *find_conn(TcpSocket_T sockets[MAX_SOCKETS], const uint8_t src_ip[4], uint16_t src_port, uint16_t dst_port, const TcpSocket_T *skip) {
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        TcpSocket_T *s = &sockets[i];

        if (!s->used || s == skip || s->state == SOCKET_LISTENING)
            continue;

        if (s->local_port == dst_port && s->remote_port == src_port && memcmp(s->remote_ip, src_ip, 4) == 0)
            return s;
    }

    return 0;
}

/* Answer a segment we have no socket for.  Uses a scratch socket so a reset
 * can be sent without occupying a slot — which is the whole point when the
 * reason for the reset is that the pool is empty. */
static void send_reset(const uint8_t src_ip[4], const uint8_t dst_ip[4], const TcpHeader_T *tcp_header) {
    TcpSocket_T tmp;

    memcpy(tmp.local_ip, dst_ip, 4);
    memcpy(tmp.remote_ip, src_ip, 4);

    tmp.local_port = tcp_header->dest_port;
    tmp.remote_port = tcp_header->source_port;
    tmp.seq_num = 0;
    tmp.ack_num = tcp_header->seq_num + 1;
    tmp.rx_len = 0;
    tmp.state = SOCKET_CLOSED;

    send_tcp_packet(&tmp, 0, 0, TCP_FLAG_RST | TCP_FLAG_ACK);
}

void on_tcp_packet(const uint8_t src_ip[4], const uint8_t dst_ip[4], TcpHeader_T *tcp_header, const uint8_t *payload, uint32_t len, TcpSocket_T sockets[MAX_SOCKETS]) {
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        TcpSocket_T *s = &sockets[i];

        if (!s->used || s->local_port != tcp_header->dest_port) {
            continue;
        }

        /* After parse_tcp_packet's htons swap: low byte = flags, bits 15-12 = data-offset */
        uint8_t flags = tcp_header->data_offset_reserved_flags & 0xFF;
        uint16_t tcp_hdr_len = ((tcp_header->data_offset_reserved_flags >> 12) & 0xF) * 4;
        uint32_t data_len = (len > tcp_hdr_len) ? len - tcp_hdr_len : 0;

        if (s->state == SOCKET_LISTENING && (flags & TCP_FLAG_SYN)) {
            /* A SYN whose 4-tuple we already track is either a retransmission
             * (our SYN-ACK was lost) or the peer reusing an ephemeral port
             * after we dropped its state.  Allocating a second socket for the
             * same tuple would give accept() two candidates and waste a slot,
             * so deal with the existing one first. */
            TcpSocket_T *dup = find_conn(sockets, src_ip, tcp_header->source_port, tcp_header->dest_port, s);

            if (dup) {
                if (dup->state == SOCKET_ESTABLISHED && dup->seq_num <= 1 && dup->rx_len == 0) {
                    /* Nothing has flowed on it yet — just repeat the SYN-ACK. */
                    dup->ack_num = tcp_header->seq_num + 1;
                    dup->seq_num = 0;
                    send_tcp_packet(dup, 0, 0, TCP_FLAG_SYN | TCP_FLAG_ACK);
                    dup->seq_num = 1;
                    dup->last_activity = net_now;

                    return;
                }

                /* Otherwise the peer really is starting over on this tuple;
                 * drop our stale half so the new SYN gets a clean socket. */
                free_socket(dup);
            }

            TcpSocket_T *new_conn = alloc_socket(sockets);

            if (!new_conn) {
                /* Out of slots.  A reset lets the peer fail — and retry —
                 * at once; staying silent instead leaves it retransmitting
                 * SYNs into the void until its connect timeout expires,
                 * which is indistinguishable from the host being down. */
                send_reset(src_ip, dst_ip, tcp_header);

                return;
            }

            memcpy(new_conn->remote_ip, src_ip, 4);
            memcpy(new_conn->local_ip, dst_ip, 4);

            new_conn->local_port = tcp_header->dest_port;
            new_conn->remote_port = tcp_header->source_port;

            new_conn->state = SOCKET_ESTABLISHED;

            new_conn->seq_num = 0;
            new_conn->ack_num = tcp_header->seq_num + 1;

            send_tcp_packet(new_conn, 0, 0, TCP_FLAG_SYN | TCP_FLAG_ACK);
            new_conn->seq_num = 1;
            new_conn->last_activity = net_now;

            return;
        }

        if (s->state == SOCKET_SYN_SENT && (flags & TCP_FLAG_RST)) {
            free_socket(s);
            return;
        }

        /* Challenge ACK: server has stale established state and replied to our
         * SYN with a plain ACK (RFC 5961).  Send RST at the server's expected
         * seq to clear its stale state, then re-send SYN so the next incoming
         * SYN-ACK can complete the handshake. */
        if (s->state == SOCKET_SYN_SENT && (flags & TCP_FLAG_ACK) && !(flags & TCP_FLAG_SYN) &&
            memcmp(s->remote_ip, src_ip, 4) == 0 && s->remote_port == tcp_header->source_port) {
            s->seq_num = tcp_header->ack_num;
            s->ack_num = 0;
            send_tcp_packet(s, 0, 0, TCP_FLAG_RST);
            s->seq_num = 0;
            send_tcp_packet(s, 0, 0, TCP_FLAG_SYN);
            s->seq_num = 1;
            s->last_activity = net_now;
            return;
        }

        if (s->state == SOCKET_SYN_SENT && (flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && memcmp(s->remote_ip, src_ip, 4) == 0 && s->remote_port == tcp_header->source_port) {
            memcpy(s->local_ip, dst_ip, 4);
            s->ack_num = tcp_header->seq_num + 1;
            s->state = SOCKET_ESTABLISHED;
            s->last_activity = net_now;
            send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
            return;
        }

        /* Everything below concerns an open connection, so the peer has to match. */
        if (memcmp(s->remote_ip, src_ip, 4) != 0 || s->remote_port != tcp_header->source_port)
            continue;

        if (s->state == SOCKET_FIN_WAIT) {
            if (flags & TCP_FLAG_RST) {
                free_socket(s);
                return;
            }

            s->last_activity = net_now;

            if (flags & TCP_FLAG_FIN) {
                /* Acknowledge the peer's FIN so it can finish closing instead
                 * of sitting on a timeout, then release the slot. */
                s->ack_num = tcp_header->seq_num + data_len + 1;
                send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
                free_socket(s);
            }

            /* A bare ACK of our FIN leaves the socket here so we can still
             * answer the peer's own FIN when it arrives; socket_reap() and
             * alloc_socket() put a ceiling on how long that lasts. */
            return;
        }

        if (s->state == SOCKET_ESTABLISHED || s->state == SOCKET_CLOSE_WAIT) {
            if (flags & TCP_FLAG_RST) {
                free_socket(s);
                return;
            }

            s->last_activity = net_now;

            if (data_len > 0) {
                if (tcp_header->seq_num == s->ack_num) {
                    /* In order: append rather than overwrite, so a request
                     * split across segments survives.  Only acknowledge what
                     * actually fit — the peer retransmits the remainder once
                     * read() drains the buffer and the advertised window
                     * reopens. */
                    const uint8_t *data = payload + tcp_hdr_len;
                    uint32_t space = RX_BUFFER_SIZE - s->rx_len;
                    uint32_t take = (data_len < space) ? data_len : space;

                    for (uint32_t j = 0; j < take; j++) {
                        s->rx_buffer[s->rx_len + j] = data[j];
                    }

                    s->rx_len += take;
                    s->ack_num = tcp_header->seq_num + take;
                }

                /* Duplicate or out-of-order segments skip the copy above and
                 * fall through to a plain re-ACK of what we do have, so a
                 * retransmission cannot corrupt a half-read request or drag
                 * ack_num backwards. */
                send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
            }

            if (flags & TCP_FLAG_FIN) {
                /* A FIN occupies a sequence number of its own; acknowledge
                 * past it or the peer keeps retransmitting. */
                s->ack_num = tcp_header->seq_num + data_len + 1;
                send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);

                if (s->rx_len > 0) {
                    /* The peer half-closed right after its request, which many
                     * HTTP/1.0 clients do.  Keep the socket so the application
                     * can still read that request and write a reply. */
                    s->state = SOCKET_CLOSE_WAIT;
                } else {
                    free_socket(s);
                }
            }

            return;
        }
    }
}

static void socket_claim(TcpSocket_T *s, uint8_t i) {
    s->used = 1;
    s->id = i;
    s->rx_len = 0;
    s->tx_len = 0;
    s->last_activity = net_now;
}

TcpSocket_T *alloc_socket(TcpSocket_T sockets[MAX_SOCKETS]) {
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            socket_claim(&sockets[i], i);

            return &sockets[i];
        }
    }

    /* Pool is full.  A socket in FIN_WAIT has already been closed from this
     * side and is only waiting on the peer's half of the handshake, so it is
     * the cheapest thing to give up.  Without this fallback a peer that never
     * finishes a close would hold a slot indefinitely in applications that do
     * not call socket_reap(). */
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].state == SOCKET_FIN_WAIT) {
            sockets[i].state = SOCKET_CLOSED;
            socket_claim(&sockets[i], i);

            return &sockets[i];
        }
    }

    return 0;
}

TcpSocket_T *tcp_connect(TcpSocket_T sockets[MAX_SOCKETS], const uint8_t remote_ip[4], uint16_t remote_port, uint16_t local_port, const uint8_t local_ip[4]) {
    TcpSocket_T *sock = alloc_socket(sockets);
    if (!sock)
        return 0;

    memcpy(sock->remote_ip, remote_ip, 4);
    memcpy(sock->local_ip, local_ip, 4);

    sock->remote_port = remote_port;
    sock->local_port = local_port;
    sock->seq_num = 0;
    sock->ack_num = 0;
    sock->state = SOCKET_SYN_SENT;

    send_tcp_packet(sock, 0, 0, TCP_FLAG_SYN);
    sock->seq_num = 1;

    return sock;
}

void free_socket(TcpSocket_T *sock) {
    sock->used = 0;
    sock->state = SOCKET_CLOSED;
    sock->rx_len = 0;
    sock->tx_len = 0;
}

void socket_pool_init(TcpSocket_T sockets[MAX_SOCKETS]) {
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        sockets[i].used = 0;
        sockets[i].state = SOCKET_CLOSED;
        sockets[i].id = i;
        sockets[i].local_port = 0;
        sockets[i].remote_port = 0;
        sockets[i].rx_len = 0;
        sockets[i].tx_len = 0;
        sockets[i].seq_num = 0;
        sockets[i].ack_num = 0;
        sockets[i].last_activity = 0;
    }
}

uint8_t socket_reap(TcpSocket_T sockets[MAX_SOCKETS], uint32_t idle_secs, SocketSet_T protect) {
    uint8_t freed = 0;

    if (!net_now)
        return 0; /* no clock supplied — nothing can be judged idle */

    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        TcpSocket_T *s = &sockets[i];

        if (!s->used || s->state == SOCKET_LISTENING)
            continue;

        if (protect & (SocketSet_T)(1u << i))
            continue;

        uint32_t idle = (net_now >= s->last_activity) ? net_now - s->last_activity : 0;

        if (s->state == SOCKET_FIN_WAIT) {
            if (idle >= FIN_WAIT_TIMEOUT_SECS) {
                free_socket(s);
                freed++;
            }

            continue;
        }

        if (idle < idle_secs)
            continue;

        /* Reset rather than FIN: the connection is being abandoned, not closed
         * politely, and a reset needs no acknowledgement to complete. */
        if (s->state == SOCKET_ESTABLISHED || s->state == SOCKET_CLOSE_WAIT)
            send_tcp_packet(s, 0, 0, TCP_FLAG_RST);

        free_socket(s);
        freed++;
    }

    return freed;
}

SocketSet_T socket_select(TcpSocket_T sockets[MAX_SOCKETS], uint8_t events) {
    SocketSet_T result = 0;
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        TcpSocket_T *s = &sockets[i];
        if (!s->used)
            continue;
        uint8_t match = 0;
        if ((events & SEL_READ) && (s->state == SOCKET_ESTABLISHED || s->state == SOCKET_CLOSE_WAIT) && s->rx_len > 0)
            match = 1;
        if ((events & SEL_WRITE) && s->state == SOCKET_ESTABLISHED)
            match = 1;
        /* CLOSE_WAIT is an active state with a request still to be read, not a
         * stale one — reporting it here would invite callers to free it. */
        if ((events & SEL_EXCEPT) && s->state != SOCKET_ESTABLISHED && s->state != SOCKET_LISTENING && s->state != SOCKET_CLOSE_WAIT)
            match = 1;
        if (match)
            result |= (SocketSet_T)(1u << i);
    }
    return result;
}

void send_tcp_packet(TcpSocket_T *sock, const uint8_t *data, uint32_t len, uint8_t flags) {
    uint8_t packet_buf[1500];
    uint8_t tcp_packet[1500];

    TcpPacketRequest_T request;
    Ipv4Header_T ipv4_header;

    uint8_t ipv4_header_len = sizeof(Ipv4Header_T);
    uint8_t tcp_header_len = sizeof(TcpHeader_T);
    uint8_t tcp_req_len = sizeof(TcpPacketRequest_T);

    request.header.source_port = sock->local_port;
    request.header.dest_port = sock->remote_port;
    request.header.seq_num = sock->seq_num;
    request.header.ack_num = sock->ack_num;
    /* Advertise the space actually left in the receive buffer instead of a
     * constant, so a peer cannot legitimately overrun it. */
    request.header.window_size = (uint16_t)(RX_BUFFER_SIZE - ((sock->rx_len < RX_BUFFER_SIZE) ? sock->rx_len : RX_BUFFER_SIZE));

    uint16_t data_offset = (sizeof(TcpHeader_T) / 4) & 0xF;
    request.header.data_offset_reserved_flags = (data_offset << 12) | (flags & 0xFF);

    memcpy(request.src_ip, sock->local_ip, 4);
    memcpy(request.dst_ip, sock->remote_ip, 4);

    request.length = len;

    memcpy(tcp_packet, (uint8_t *)&request, tcp_req_len);

    if (data) {
        memcpy(tcp_packet + tcp_req_len, data, len);
    }

    // Create a reply TCP packet
    if (!new_packet(CRAFT_TCP_PACKET, (uint8_t *)tcp_packet)) {
        print((const uint8_t *)"-> TCP packet creation failed\n");
        return;
    }

    // Compose a dummy IPv4 header
    ipv4_header.version = 0x45; // version=4, IHL=5 (20 bytes) — kernel uses this to find payload offset
    memcpy(ipv4_header.source_addr, sock->remote_ip, 4);
    memcpy(ipv4_header.destination_addr, sock->local_ip, 4);
    ipv4_header.protocol = 6;
    ipv4_header.total_length = htons(ipv4_header_len + tcp_header_len + len);

    // Compose the IP packet: IPv4 header, then TCP header, then payload data.
    // After new_packet(CRAFT_TCP_PACKET) the kernel writes the computed TCP checksum back
    // into tcp_packet[0..tcp_header_len-1], so we take the TCP header from there.
    // We copy `data` directly rather than from tcp_packet+tcp_req_len to avoid the
    // 10-byte TcpPacketRequest_T padding that would otherwise truncate the payload.
    memcpy(packet_buf, (const uint8_t *)&ipv4_header, ipv4_header_len);
    memcpy(packet_buf + ipv4_header_len, (const uint8_t *)tcp_packet, tcp_header_len);
    if (data && len > 0)
        memcpy(packet_buf + ipv4_header_len + tcp_header_len, data, len);

    if (!new_packet(CRAFT_IPV4_PACKET, (uint8_t *)packet_buf)) {
        print((const uint8_t *)"-> IPv4 packet creation failed\n");
        return;
    }

    net_drv.send_ip(packet_buf, (uint32_t)htons(ipv4_header.total_length));

    sock->seq_num += len;

    /* SYN and FIN each occupy a sequence number despite carrying no payload.
     * The callers that assign seq_num explicitly just after sending a SYN
     * (tcp_connect, the SYN-ACK path in on_tcp_packet) write back the same
     * value, so the accounting agrees either way. */
    if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN))
        sock->seq_num += 1;

    /* The parse exists only to render the trace, so it is gated too rather
     * than byte-swapping a header on every packet for output nobody reads. */
    if (net_debug) {
        TcpHeader_T tcp_header;
        parse_tcp_packet(tcp_packet, &tcp_header);

        printf((const uint8_t *)"<< TCP: (%x) src_port: %u, dest_port: %u, seq %u\n", tcp_header.data_offset_reserved_flags, tcp_header.source_port, tcp_header.dest_port, tcp_header.seq_num);
    }
}
