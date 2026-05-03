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

void net_get_local_ip(uint8_t ip[4])  { memcpy(ip,  eth_my_ip,  4); }
void net_get_local_mac(uint8_t mac[6]) { memcpy(mac, eth_my_mac, 6); }

#define ETH_ARP_CACHE_SIZE 8

typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    uint8_t valid;
} EthArpEntry_T;

static EthArpEntry_T eth_arp_cache[ETH_ARP_CACHE_SIZE];

static void eth_arp_cache_update(const uint8_t ip[4], const uint8_t mac[6]) {
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
            eth_arp_cache_update(arp->spa, eth->src);
            eth_send_arp_reply(eth, arp);
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
    if (!eth_arp_cache_lookup(hdr->destination_addr, dst_mac))
        return;

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

int net_driver_select(const uint8_t *name) {
    if (name && name[0] == 'e') {
        static const uint8_t my_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        static const uint8_t my_ip[4]  = {10, 3, 4, 2};

        memcpy(eth_my_mac, my_mac, 6);
        memcpy(eth_my_ip,  my_ip,  4);

        net_register();
        net_drv.recv = eth_drv_recv;
        net_drv.send_ip = eth_drv_send;

        return 0;
    }

    /* Default: SLIP over serial */
    net_drv.recv = slip_recv;
    net_drv.send_ip = slip_send;

    if (!serial_init())
        return -1;

    return 0;
}

int net_driver_bind_port(const uint8_t *name, uint16_t port) {
    if (name && name[0] == 'e') {
        static const uint8_t my_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        static const uint8_t my_ip[4]  = {10, 3, 4, 2};

        memcpy(eth_my_mac, my_mac, 6);
        memcpy(eth_my_ip,  my_ip,  4);

        /* Become the global driver if nobody else has registered yet.
         * The kernel's register_driver is idempotent: first caller wins,
         * subsequent calls are no-ops.  This lets GARN or TNT bootstrap
         * the NIC without a separate eth.elf process. */
        net_register();
        net_bind_port(port);
        net_drv.recv = eth_drv_recv;
        net_drv.send_ip = eth_drv_send;

        return 0;
    }

    /* SLIP: no port-level demux — behaves the same as net_driver_select */
    net_drv.recv = slip_recv;
    net_drv.send_ip = slip_send;

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

        if (s->used && s->state == SOCKET_ESTABLISHED && s->local_port == listener->local_port) {
            if (s != listener && s->rx_len > 0) {
                return s;
            }
        }
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

    return len;
}

void close(TcpSocket_T *sock) {
    send_tcp_packet(sock, 0, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
    free_socket(sock);
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

        if (s->state == SOCKET_LISTENING && (flags & TCP_FLAG_SYN)) {
            TcpSocket_T *new_conn = alloc_socket(sockets);

            if (!new_conn) {
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
            return;
        }

        if (s->state == SOCKET_SYN_SENT && (flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && memcmp(s->remote_ip, src_ip, 4) == 0 && s->remote_port == tcp_header->source_port) {
            memcpy(s->local_ip, dst_ip, 4);
            s->ack_num = tcp_header->seq_num + 1;
            s->state = SOCKET_ESTABLISHED;
            send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
            return;
        }

        if (s->state == SOCKET_ESTABLISHED && memcmp(s->remote_ip, src_ip, 4) == 0 && s->remote_port == tcp_header->source_port) {
            if (flags & TCP_FLAG_RST) {
                free_socket(s);
                return;
            }

            uint32_t data_len = (len > tcp_hdr_len) ? len - tcp_hdr_len : 0;

            if (data_len > 0) {
                const uint8_t *data = payload + tcp_hdr_len;
                for (uint32_t j = 0; j < data_len && j < RX_BUFFER_SIZE; j++) {
                    s->rx_buffer[j] = data[j];
                }

                s->rx_len = data_len;
                s->ack_num = tcp_header->seq_num + data_len;
            }

            if (flags & TCP_FLAG_FIN) {
                send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
                free_socket(s);
            }
        }
    }
}

TcpSocket_T *alloc_socket(TcpSocket_T sockets[MAX_SOCKETS]) {
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            sockets[i].used = 1;
            sockets[i].id = i;

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
}

SocketSet_T socket_select(TcpSocket_T sockets[MAX_SOCKETS], uint8_t events) {
    SocketSet_T result = 0;
    for (uint8_t i = 0; i < MAX_SOCKETS; i++) {
        TcpSocket_T *s = &sockets[i];
        if (!s->used) continue;
        uint8_t match = 0;
        if ((events & SEL_READ)   && s->state == SOCKET_ESTABLISHED && s->rx_len > 0)
            match = 1;
        if ((events & SEL_WRITE)  && s->state == SOCKET_ESTABLISHED)
            match = 1;
        if ((events & SEL_EXCEPT) && s->state != SOCKET_ESTABLISHED && s->state != SOCKET_LISTENING)
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
    TcpHeader_T tcp_header;
    Ipv4Header_T ipv4_header;

    uint8_t ipv4_header_len = sizeof(Ipv4Header_T);
    uint8_t tcp_header_len = sizeof(TcpHeader_T);
    uint8_t tcp_req_len = sizeof(TcpPacketRequest_T);

    request.header.source_port = sock->local_port;
    request.header.dest_port = sock->remote_port;
    request.header.seq_num = sock->seq_num;
    request.header.ack_num = sock->ack_num;
    request.header.window_size = 1024;

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

    parse_tcp_packet(tcp_packet, &tcp_header);

    printf((const uint8_t *)"<< TCP: (%x) src_port: %u, dest_port: %u, seq %u\n", tcp_header.data_offset_reserved_flags, tcp_header.source_port, tcp_header.dest_port, tcp_header.seq_num);
}
