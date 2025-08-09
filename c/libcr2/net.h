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
#include "syscall.h"

/*
 *  Serial-Line Internet Protocol
 *
 *  Definitions related to the decoder.
 */
#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

/*
 *  TCP socket-related definitions
 */
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

/*
 *  type TcpPacketRequest_T
 *
 *  This special-purpose structure is to cotransport IPv4 addresses
 *  with a TCP header when requesting a new TCP packet from r2 kernel.
 */
typedef struct {
	TcpHeader_T header;
	uint8_t src_ip[4];
	uint8_t dst_ip[4];
	uint16_t length;
} __attribute__((packed)) TcpPacketRequest_T;

/*
 *  type SocketState enumeration
 *
 *  Uncomplete list of various TCP connection states.
 */
typedef enum {
	SOCKET_CLOSED,
	SOCKET_LISTENING,
	SOCKET_ESTABLISHED,
	SOCKET_FIN_WAIT
} SocketState;

/*
 *  type TcpSocket_T structure
 *
 *  TCP connection implementing socket strucutre used to track the connection state, 
 *  properties and stuff.
 */
typedef struct TcpSocket_T {
	uint32_t id;
	SocketState state;
	uint16_t local_port;
	uint16_t remote_port;
	uint8_t local_ip[4];
	uint8_t remote_ip[4];
	uint8_t rx_buffer[RX_BUFFER_SIZE];
	uint8_t tx_buffer[TX_BUFFER_SIZE];
	uint32_t rx_len;
	uint32_t tx_len;
	uint8_t used;
	uint32_t seq_num;
	uint32_t ack_num;
} TcpSocket_T;

/*
 *  TcpSocket_T sockets
 *
 *  Static array of processable sockets. Experimental.
 */
static TcpSocket_T sockets[MAX_SOCKETS];

/*
 *  void send_tcp_packet() prototype
 *
 *  This function should be capeble of send the TCP packet wrapped in an IPv4 packet successfully over the line.
 */
void send_tcp_packet(TcpSocket_T *sock, const uint8_t* data, uint32_t len, uint8_t flags);

/*
 *  TcpSocket_T *socket_tcp4() prototype
 *
 *  This function tries to allocate a new socket. Returns the socket on success, 0 otherwise.
 */
TcpSocket_T *socket_tcp4();

/*
 *  static TcpSocket_T *alloc_socket() prototype
 *
 *  This function tries to allocate a new socket (unused socket). Returns the socket on success, 0 otherwise.
 */
static TcpSocket_T *alloc_socket();

/*
 *  static void free_socket() prototype
 *
 *  This simple macro-like function sets socket <used> property to 0 and marks the socket as CLOSED.
 */
static void free_socket(TcpSocket_T *sock);

/*
 *  void bind() prototype
 *
 *  This function sets a socket's <local_port> property to the <port> value.
 */
void bind(TcpSocket_T *sock, uint16_t port);

/*
 *  void listen() prototype
 *
 *  This function sets a socket's state to SOCKET_LISTENING.
 */
void listen(TcpSocket_T *sock);

/*
 *  TcpSocket_T *accept() prototype
 *
 *  This function checks the array of sockets and returns the one marked as used, with the conn state as ESTABLISHED, 
 *  and wuth the <local_port> being the same as the listener's one.
 */
TcpSocket_T *accept(TcpSocket_T *listener);

/*
 *  uint32_t read() prototype
 *
 *  This function reads the contents of socket's RX buffer into provided <buf> array. The number of bytes read
 *  is then returned.
 */
uint32_t read(TcpSocket_T *sock, uint8_t *buf, uint32_t maxlen);

/*
 *  uint32_t write() prototype
 *
 *  This function writes the given <buf> array directly over the line.
 */
uint32_t write(TcpSocket_T *sock, const uint8_t *buf, uint32_t len);

/*
 *  void close() prototype
 *
 *  This function closes the connection tracked by such socket provided. 
 */
void close(TcpSocket_T *sock);

/*
 *  void on_tcp_packet() prototype
 *
 *  This function is to parse a TCP packet and to set according sockets as ESTABLISHED, FIN_WAIT or to free them.
 */
void on_tcp_packet(const uint8_t src_ip[4], const uint8_t dst_ip[4], TcpHeader_T *tcp_header, const uint8_t *payload, uint32_t len);

/*
 *  int64_t decode_slip() prototype
 *
 *  A function implementing this prototype should be capeble of parsing the <input* according to
 *  the SLIP special chars and extracting the whole raw frame in <output>.
 *
 *  Function returns -1 when error occurs, 0 when the frame is not decoded yet, and N as the length of decoded frame.
 */
int64_t decode_slip(const uint8_t *input, uint32_t input_len, uint8_t *output, uint32_t output_len);

/*
 *  uint16_t parse_ipv4_packet() prototype
 *
 *  A simple macro-like function that copyies the contents of the packet into the IPv4 
 *  header structure of declared size. Function returns the header size.
 */
uint16_t parse_ipv4_packet(const uint8_t *packet, Ipv4Header_T *header);

/*
 *  uint8_t parse_icmp_packet() prototype
 *
 *  A simple macro-like function to copy the packet data into the ICMP header structure.
 *  Returns 8 as that is the standard ICMP header size.
 */
uint8_t parse_icmp_packet(const uint8_t *packet, IcmpHeader_T *header);

/*
 *  uint8_t parse_tcp_packet() prototype
 *
 *  A simple macro-like function to copy the packet data into the TCP header structure.
 */
uint16_t parse_tcp_packet(const uint8_t *packet, TcpHeader_T *header);

#ifdef __cplusplus
}
#endif

#endif
