/* Exercise libcr2 itself: the sender tests use a simulated transport. */
#include "../../libcr2/net.c"
extern int puts(const char *);
static uint32_t sent_packets, packet_len;
int64_t print(const uint8_t *s) { (void)s; return 0; }
int64_t new_packet(uint8_t type, uint8_t *buffer) { (void)type; (void)buffer; return 1; }
static void transmit(const uint8_t *packet, uint32_t len) {
    (void)packet; ++sent_packets; packet_len = len;
}
int main(void) {
    uint8_t options[28] = {0};
    options[20] = 1; options[21] = 2; options[22] = 4;
    options[23] = 0x05; options[24] = 0xb4;
    if (syn_mss(options, 28, 28) != 1460) return 1;
    if (syn_mss(options, 20, 28) != 536) return 2;
    options[22] = 1;
    if (syn_mss(options, 28, 28) != 536) return 3;
    options[22] = 4;

    TcpSocket_T pool[MAX_SOCKETS];
    socket_pool_init(pool);
    net_drv.send_ip = transmit;
    pool[0].used = 1; pool[0].state = SOCKET_LISTENING; pool[0].local_port = 8080;
    uint8_t src[4] = {10, 3, 4, 1}, dst[4] = {10, 3, 4, 2};
    TcpHeader_T header = {0};
    header.source_port = 45000; header.dest_port = 8080;
    header.seq_num = 500; header.window_size = 65535;
    header.data_offset_reserved_flags = (7 << 12) | TCP_FLAG_SYN;
    on_tcp_packet(src, dst, &header, options, sizeof(options), pool);
    TcpSocket_T *sock = &pool[1];
    if (!sock->used || sock->state != SOCKET_ESTABLISHED || tcp_peer_mss(sock) != 1460) return 4;
    header.data_offset_reserved_flags = (5 << 12) | TCP_FLAG_ACK;
    sock->tx_acked = 0xfffffff0U; sock->seq_num = 100;
    header.ack_num = 64;
    on_tcp_packet(src, dst, &header, options, 20, pool);
    if (sock->tx_acked != 64 || tcp_bytes_in_flight(sock) != 36) return 5;
    header.ack_num = 101;
    on_tcp_packet(src, dst, &header, options, 20, pool);
    if (sock->tx_acked != 64) return 6;
    header.ack_num = 32;
    on_tcp_packet(src, dst, &header, options, 20, pool);
    if (sock->tx_acked != 64) return 7;
    free_socket(sock);
    sock = alloc_socket(pool);
    if (tcp_peer_mss(sock) != 536) return 8;
    uint8_t payload[TCP_MAX_PAYLOAD + 1] = {0};
    uint32_t before = sent_packets;
    uint32_t seq_before = sock->seq_num;
    send_tcp_packet(sock, payload, sizeof(payload), TCP_FLAG_ACK);
    if (sent_packets != before || sock->seq_num != seq_before) return 9;
    send_tcp_packet(sock, payload, TCP_MAX_PAYLOAD, TCP_FLAG_ACK);
    if (sent_packets != before + 1 || sock->seq_num != seq_before + TCP_MAX_PAYLOAD ||
        packet_len != TCP_MAX_PAYLOAD + 40) return 10;
    before = sent_packets;
    seq_before = sock->seq_num;
    if (write(sock, payload, sizeof(payload)) != 0 || write(sock, 0, 1) != 0 ||
        write(0, payload, 1) != 0 || sent_packets != before || sock->seq_num != seq_before) return 11;
    if (write(sock, payload, TCP_MAX_PAYLOAD) != TCP_MAX_PAYLOAD ||
        sent_packets != before + 1 || sock->seq_num != seq_before + TCP_MAX_PAYLOAD) return 12;
    puts("libcr2: MSS negotiation, malformed options, ACK wrap and packet bounds passed");
    return 0;
}
