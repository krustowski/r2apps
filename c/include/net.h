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

#define MAX_SOCKETS	8
#define RX_BUFFER_SIZE  1024
#define TX_BUFFER_SIZE  1024

#define TCP_FLAG_FIN	0x01
#define TCP_FLAG_SYN	0x02
#define TCP_FLAG_ACK	0x10

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

typedef enum {
	SOCKET_CLOSED,
	SOCKET_LISTENING,
	SOCKET_ESTABLISHED,
	SOCKET_FIN_WAIT
} SocketState;

typedef struct TcpSocket {
	uint32_t id;
	SocketState state;
	uint16_t local_port;
	uint32_t remoze_ip;
	uint16_t remote_port;
	uint8_t rx_buffer[RX_BUFFER_SIZE];
	uint8_t tx_buffer[TX_BUFFER_SIZE];
	uint32_t rx_len;
	uint32_t tx_len;
	uint8_t used;
} TcpSocket;

static TcpSocket sockets[MAX_SOCKETS];

void send_tcp_packet(TcpSocket *sock, const uint8_t* data, uint32_t len, uint8_t flags);

TcpSocket *socket_tcp4();
static TcpSocket *alloc_socket();
static void free_socket(TcpSocket *sock);

void bind(TcpSocket *sock, uint16_t port);
void listen(TcpSocket *sock);
TcpSocket *accept(TcpSocket *listener);
uint32_t read(TcpSocket *sock, uint8_t *buf, uint32_t maxlen);
uint32_t write(TcpSocket *sock, const uint8_t *buf, uint32_t len);
void close(TcpSocket *sock);

void on_tcp_packet(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint8_t flags, const uint8_t *payload, uint32_t len);


int64_t decode_slip(const uint8_t *input, uint32_t input_len, uint8_t *output, uint32_t output_len);

uint16_t parse_ipv4_packet(const uint8_t *packet, Ipv4Header_T *header);
uint8_t parse_icmp_packet(const uint8_t *packet, IcmpHeader_T *header);

#ifdef __cplusplus
}
#endif

#endif
