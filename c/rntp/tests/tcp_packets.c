#include <assert.h>
#include "net.h"
#include "packet_shim.h"
extern int putchar(int);
static uint8_t sent[1500];
static unsigned sent_len;
static void send_ip(const uint8_t *packet, uint32_t length) {
    assert(length <= sizeof(sent));
    memcpy(sent, packet, length); sent_len = length;
}
int64_t print(const uint8_t *text) { (void)text; return 1; }
static uint16_t wire16(unsigned off) { return ((uint16_t)sent[off] << 8) | sent[off+1]; }
static void dump(void) {
#ifdef RNTP_DUMP_DEFAULT
    for (unsigned i = 20; i < sent_len; ++i) {
        putchar("0123456789abcdef"[sent[i] >> 4]);
        putchar("0123456789abcdef"[sent[i] & 15]);
    }
    putchar('\n');
#endif
}
int main(void) {
    TcpSocket_T pool[MAX_SOCKETS];
    socket_pool_init(pool); net_drv.send_ip = send_ip;
    TcpSocket_T *s = socket_tcp4(pool);
    s->local_port = 7780; s->remote_port = 48000;
    s->seq_num = 0x12345678; s->ack_num = 0x87654321;
    s->state = SOCKET_ESTABLISHED; s->tx_acked = s->seq_num;
    uint8_t data[1460];
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = i * 37;
    /* Dump identical ordinary SYN, ACK and data for comparison to the old SDK. */
    send_tcp_packet(s, 0, 0, TCP_FLAG_SYN);
    assert(sent_len == 40 && sent[32] == 0x50 && s->seq_num == 0x12345679);
    assert(wire16(34) == RX_BUFFER_SIZE); dump();
    send_tcp_packet(s, 0, 0, TCP_FLAG_ACK); dump();
    send_tcp_packet(s, data, 1370, TCP_FLAG_ACK);
    assert(sent_len == 1410 && !memcmp(sent + 40, data, 1370)); dump();
#ifdef RNTP_MSS_TEST
    uint32_t unchanged = s->seq_num;
    send_tcp_packet(s, data, 1460, TCP_FLAG_ACK);
    assert(s->seq_num == unchanged && sent_len == 1410);
    tcp_set_mss(1460);
    uint32_t seq = s->seq_num;
    send_tcp_packet(s, 0, 0, TCP_FLAG_SYN);
    assert(sent_len == 44 && sent[32] == 0x60 && sent[33] == TCP_FLAG_SYN);
    assert(sent[40] == 2 && sent[41] == 4 && wire16(42) == 1460);
    assert(s->seq_num == seq + 1); // options consume no sequence numbers
    s->seq_num = seq;
    send_tcp_packet(s, 0, 0, TCP_FLAG_SYN | TCP_FLAG_ACK);
    assert(sent_len == 44 && s->seq_num == seq + 1);
    send_tcp_packet(s, data, sizeof(data), TCP_FLAG_ACK);
    assert(sent_len == 1500 && !memcmp(sent + 40, data, sizeof(data)));
    assert(s->seq_num == seq + 1 + sizeof(data));
    seq = s->seq_num; unsigned previous_calls = tcp_calls;
    old_kernel = 1;
    send_tcp_packet(s, 0, 0, TCP_FLAG_SYN);
    assert(tcp_calls == previous_calls + 2 && sent_len == 40 && sent[32] == 0x50);
    assert(s->seq_num == seq + 1); // one plain SYN; no stray option data
    send_tcp_packet(s, data, 1460, TCP_FLAG_ACK);
    assert(s->seq_num == seq + 1 && sent_len == 40); // legacy construction cap
    old_kernel = 0; tcp_set_mss(0);
    send_tcp_packet(s, 0, 0, TCP_FLAG_SYN);
    assert(sent_len == 40 && sent[32] == 0x50);
    tcp_set_mss(1460); tcp_set_local_packet_crafting(1);
    previous_calls = tcp_calls; seq = s->seq_num;
    s->local_ip[0] = 192; s->local_ip[2] = 2; s->local_ip[3] = 1;
    s->remote_ip[0] = 192; s->remote_ip[2] = 2; s->remote_ip[3] = 2;
    for (unsigned n = 0; n <= sizeof(data); n += n < 2 ? 1 : 1458) {
        uint8_t flags = n ? TCP_FLAG_ACK : TCP_FLAG_SYN;
        send_tcp_packet(s, n ? data : 0, n, flags);
        assert(tcp_calls == previous_calls && wire16(22) == s->remote_port);
        assert(!memcmp(sent + 12, s->local_ip, 4) && !memcmp(sent + 16, s->remote_ip, 4));
        unsigned ihl = 20, thl = n ? 20 : 24;
        assert(sent_len == ihl + thl + n);
        if (n) assert(!memcmp(sent + ihl + thl, data, n));
        uint32_t sum = 0;
        for (unsigned i = 0; i < 20; i += 2) sum += wire16(i);
        while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
        assert(sum == 65535);
        sum = 6 + thl + n;
        for (unsigned i = 12; i < 20; i += 2) sum += wire16(i);
        for (unsigned i = ihl; i + 1 < sent_len; i += 2) sum += wire16(i);
        if ((thl + n) & 1) sum += (uint32_t)sent[sent_len - 1] << 8;
        while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
        assert(sum == 65535);
        seq += n ? n : 1; assert(s->seq_num == seq);
    }
    static uint8_t large_receive[32768];
    assert(tcp_set_receive_buffer(s, large_receive, sizeof(large_receive)));
    s->rx_len = 123;
    send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
    assert(wire16(34) == sizeof(large_receive) - 123);
    s->rx_len = sizeof(large_receive);
    send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
    assert(wire16(34) == 0);
    s->rx_len = 0; seq = s->seq_num;
    send_tcp_packet(s, 0, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
    assert(s->seq_num == seq + 1 && sent[33] == (TCP_FLAG_FIN | TCP_FLAG_ACK));
    send_tcp_packet(s, 0, 0, TCP_FLAG_RST | TCP_FLAG_ACK);
    assert(s->seq_num == seq + 1 && sent[33] == (TCP_FLAG_RST | TCP_FLAG_ACK));
    assert(tcp_calls == previous_calls);
    tcp_set_local_packet_crafting(0);
#endif
    return 0;
}
