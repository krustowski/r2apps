#include <assert.h>
#include "net.h"

static uint16_t advertised;
static uint8_t received[8192], output[8192], payload[20 + TCP_MAX_PAYLOAD];
static void send_ip(const uint8_t *packet, uint32_t length) { assert(length >= 40); advertised = ((uint16_t)packet[34] << 8) | packet[35]; }
int64_t print(const uint8_t *text) { (void)text; return 1; }
#include "packet_shim.h"

int main(void) {
    TcpSocket_T sockets[MAX_SOCKETS];
    socket_pool_init(sockets);
    net_drv.send_ip = send_ip;
    TcpSocket_T *s = socket_tcp4(sockets);
    assert(s && tcp_receive_data(s) == s->rx_buffer);
    s->rx_buffer[0] = 77; s->rx_len = 1;
    assert(tcp_set_receive_buffer(s, received, sizeof(received)));
    assert(tcp_receive_data(s) == received && received[0] == 77);
    s->rx_len = 0;
    s->state = SOCKET_ESTABLISHED; s->local_port = 7780; s->remote_port = 1;
    s->remote_ip[0] = 1; s->local_ip[0] = 2;
    s->seq_num = s->tx_acked = 1; s->ack_num = 100;
    for (uint32_t i = 20; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 37);
    TcpHeader_T h = {0};
    h.source_port = 1; h.dest_port = 7780; h.data_offset_reserved_flags = 0x5010;
    h.window_size = 32768; h.ack_num = 1;
    uint8_t remote[4] = {1, 0, 0, 0}, local[4] = {2, 0, 0, 0};
    for (unsigned i = 0; i < 4; ++i) {
        h.seq_num = 100 + i * TCP_MAX_PAYLOAD;
        on_tcp_packet(remote, local, &h, payload, sizeof(payload), sockets);
    }
    assert(s->rx_len == 4 * TCP_MAX_PAYLOAD && advertised == sizeof(received) - s->rx_len);
    h.seq_num = 100; // duplicate
    on_tcp_packet(remote, local, &h, payload, sizeof(payload), sockets);
    assert(s->rx_len == 4 * TCP_MAX_PAYLOAD);
    h.seq_num = s->ack_num + 100; // out of order
    on_tcp_packet(remote, local, &h, payload, sizeof(payload), sockets);
    assert(s->rx_len == 4 * TCP_MAX_PAYLOAD);
    while (s->rx_len < sizeof(received)) {
        h.seq_num = s->ack_num;
        on_tcp_packet(remote, local, &h, payload, sizeof(payload), sockets);
    }
    assert(advertised == 0 && s->ack_num == 100 + sizeof(received));
    h.seq_num = s->ack_num;
    on_tcp_packet(remote, local, &h, payload, sizeof(payload), sockets);
    assert(s->ack_num == 100 + sizeof(received));
    assert(!tcp_set_receive_buffer(s, 0, 0)); // cannot shrink unread data
    unsigned before_read = tcp_calls;
    assert(tcp_read(s, output, sizeof(output)) == sizeof(output));
    assert(!s->rx_len);
    assert(tcp_calls == before_read + 1); // no application write needed to reopen
    assert(advertised == sizeof(received));
    for (unsigned i = 0; i < sizeof(received); ++i) assert(output[i] == received[i]);
    free_socket(s);
    assert(tcp_receive_data(s) == s->rx_buffer);
    TcpSocket_T *again = socket_tcp4(sockets);
    assert(again == s && !again->rx_len);
    assert(tcp_receive_data(again) == again->rx_buffer);
    send_tcp_packet(again, 0, 0, TCP_FLAG_ACK);
    assert(advertised == RX_BUFFER_SIZE);
    return 0;
}
