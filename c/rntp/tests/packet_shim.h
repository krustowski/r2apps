#include "bytes.h"
/* Model the kernel's request-to-wire transformation, including old kernels
 * that always emit a plain TCP header. Real builder checks live in r2_main. */
static int old_kernel;
static unsigned tcp_calls;
int64_t new_packet(uint8_t kind, uint8_t *buffer) {
    if (kind == CRAFT_TCP_PACKET) {
        TcpPacketRequest_T request;
        memcpy(&request, buffer, sizeof(request));
        unsigned words = request.header.data_offset_reserved_flags >> 12;
        unsigned options = old_kernel || words <= 5 ? 0 : (words - 5) * 4;
        for (unsigned i = 0; i < request.length; ++i)
            buffer[20 + i] = buffer[sizeof(request) + i];
        TcpHeader_T h = request.header;
        h.source_port = htons(h.source_port); h.dest_port = htons(h.dest_port);
        h.seq_num = swap32(h.seq_num); h.ack_num = swap32(h.ack_num);
        h.data_offset_reserved_flags = htons(((5 + options / 4) << 12) |
                                              (h.data_offset_reserved_flags & 255));
        h.window_size = htons(h.window_size);
        h.checksum = h.urgent_pointer = 0;
        memcpy(buffer, &h, sizeof(h));
        ++tcp_calls;
    } else if (kind == CRAFT_IPV4_PACKET) {
        Ipv4Header_T *ip = (Ipv4Header_T *)buffer;
        uint8_t address[4];
        memcpy(address, ip->source_addr, 4);
        memcpy(ip->source_addr, ip->destination_addr, 4);
        memcpy(ip->destination_addr, address, 4);
        ip->dscp_ecn = 0; ip->identification = 0;
        ip->flags_fragment_offset = 0; ip->ttl = 64; ip->header_checksum = 0;
    }
    return 1;
}
