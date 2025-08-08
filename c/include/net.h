#ifndef _R2_NET_INCLUDED_
#define _R2_NET_INCLUDED_

/*
 *  net.h
 *
 *  Custom C header file providing prototypes, definitions and constants to 
 *  work with netwoking via the r2 kernel project.
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "mem.h"

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

/*
 *  type Ipv4Header_T structure
 *
 *  This structure specifies the property list of an IPv4 header.
 */
typedef struct {
	uint8_t version;
	uint8_t dscp_ecn;
	uint16_t total_length;
	uint16_t identification;
	uint16_t flags_fragment_offset;
	uint8_t ttl;
	uint8_t protocol;
	uint16_t header_checksum;
	uint8_t source_addr[4];
	uint8_t destination_addr[4];
} __attribute__((packed)) Ipv4Header_T;

/*
 *  type IcmpHeader_T structure
 *
 *  This structure specifies the property list of an ICMP header.
 */
typedef struct {
	uint8_t type;
	uint8_t code;
	uint16_t checksum;
	uint16_t identifier;
	uint16_t sequence_number;
} __attribute__((packed)) IcmpHeader_T;

/*
 *  type TcpHeader_T structure
 *
 *  This structure specifies the property list of a TCP header.
 */
typedef struct {
    uint16_t source_port;
    uint16_t dest_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t data_offset_reserved_flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_pointer;
} __attribute__((packed)) TcpHeader_T;


int64_t decode_slip(const uint8_t *input, uint32_t input_len, uint8_t *output, uint32_t output_len);

uint16_t parse_ipv4_packet(const uint8_t *packet, Ipv4Header_T *header);
uint8_t parse_icmp_packet(const uint8_t *packet, IcmpHeader_T *header);

#ifdef __cplusplus
}
#endif

#endif
