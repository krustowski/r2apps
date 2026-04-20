#include "net.h"
#include "string.h"
#include "bytes.h"
#include "printf.h"

// Returns number of decoded bytes, or -1 on protocol error, or 0 if frame not yet complete
int64_t decode_slip(const uint8_t *input, uint32_t input_len, uint8_t *output, uint32_t output_len)
{
	uint32_t out_pos = 0;
	uint8_t escape = 0;

	for (uint32_t i = 0; i < input_len; ++i)
	{
		uint8_t b = input[i];

		if (b == SLIP_END)
		{
			if (out_pos > 0)
			{
				// Frame complete
				return (int64_t)out_pos;  
			}
			// Else ignore leading END
			continue;
		}

		if (b == SLIP_ESC)
		{
			escape = 1;
			continue;
		}

		if (escape)
		{
			if (b == SLIP_ESC_END)
			{
				b = SLIP_END;
			}
			else if (b == SLIP_ESC_ESC)
			{
				b = SLIP_ESC;
			} 
			else
			{
				// Protocol error
				return -1; 
			}
			escape = 0;
		}

		if (out_pos >= output_len)
		{
			// Output buffer overflow
			return -1; 
		}

		output[out_pos++] = b;
	}

	// Not finished yet
	return 0; 
}

uint16_t parse_ipv4_packet(const uint8_t *packet, Ipv4Header_T *header)
{
	uint16_t header_len = 0;
	//uint16_t packet_len = 0;

	memcpy(header, packet, sizeof(Ipv4Header_T));
	header_len = (header->version & 0x0F) * 4;

	/*while (packet[packet_len]) ++packet_len;

	  if (packet_len < header_len)
	  {
	  return 0;
	  }*/

	return header_len;
}

uint8_t parse_icmp_packet(const uint8_t *packet, IcmpHeader_T *header)
{
	uint8_t header_len = 0;
	//uint16_t packet_len = 0;

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

uint16_t parse_tcp_packet(const uint8_t *packet, TcpHeader_T *header)
{
	uint16_t header_len = 20;

	memcpy(header, packet, sizeof(TcpHeader_T));

	header->source_port = swap16(header->source_port);
	header->dest_port = swap16(header->dest_port);
	header->ack_num = swap32(header->ack_num);
	header->seq_num = swap32(header->seq_num);
	header->data_offset_reserved_flags = htons(header->data_offset_reserved_flags);

	return header_len;
}

TcpSocket_T *socket_tcp4(TcpSocket_T sockets[MAX_SOCKETS])
{
	TcpSocket_T *sock = alloc_socket(sockets);
	if (!sock)
	{
		return 0;
	}

	sock->state = SOCKET_CLOSED;

	return sock;
}

void bind(TcpSocket_T *sock, uint16_t port)
{
	sock->local_port = port;
}

void listen(TcpSocket_T *sock)
{
	sock->state = SOCKET_LISTENING;
}

TcpSocket_T *accept(TcpSocket_T *listener, TcpSocket_T sockets[MAX_SOCKETS])
{
	for (uint8_t i = 0; i < MAX_SOCKETS; i++)
	{
		TcpSocket_T *s = &sockets[i];

		if (s->used && s->state == SOCKET_ESTABLISHED && s->local_port == listener->local_port)
		{
			if (s != listener && s->rx_len > 0)
			{
				return s;
			}
		}
	}

	return 0;
}

uint32_t read(TcpSocket_T *sock, uint8_t *buf, uint32_t maxlen)
{
	uint32_t n = (sock->rx_len < maxlen) ? sock->rx_len : maxlen;

	for (uint32_t i = 0; i < n; i++)
	{
		buf[i] = sock->rx_buffer[i];
	}

	sock->rx_len = 0;
	return n;
}

uint32_t write(TcpSocket_T *sock, const uint8_t *buf, uint32_t len)
{
	send_tcp_packet(sock, buf, len, TCP_FLAG_ACK);
	return len;
}

void close(TcpSocket_T *sock)
{
	send_tcp_packet(sock, 0, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
	free_socket(sock);
}

void on_tcp_packet(const uint8_t src_ip[4], const uint8_t dst_ip[4], TcpHeader_T *tcp_header, const uint8_t *payload, uint32_t len, TcpSocket_T sockets[MAX_SOCKETS])
{
	for (uint8_t i = 0; i < MAX_SOCKETS; i++)
	{
		TcpSocket_T *s = &sockets[i];

		if (!s->used || s->local_port != tcp_header->dest_port)
		{
			continue;
		}

		/* After parse_tcp_packet's htons swap: low byte = flags, bits 15-12 = data-offset */
		uint8_t flags = tcp_header->data_offset_reserved_flags & 0xFF;
		uint16_t tcp_hdr_len = ((tcp_header->data_offset_reserved_flags >> 12) & 0xF) * 4;

		if (s->state == SOCKET_LISTENING && (flags & TCP_FLAG_SYN))
		{
			TcpSocket_T *new_conn = alloc_socket(sockets);

			if (!new_conn)
			{
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

		if (s->state == SOCKET_ESTABLISHED && memcmp(s->remote_ip, src_ip, 4) == 0 && s->remote_port == tcp_header->source_port)
		{
			uint32_t data_len = (len > tcp_hdr_len) ? len - tcp_hdr_len : 0;

			if (data_len > 0)
			{
				const uint8_t *data = payload + tcp_hdr_len;
				for (uint32_t j = 0; j < data_len && j < RX_BUFFER_SIZE; j++)
				{
					s->rx_buffer[j] = data[j];
				}

				s->rx_len = data_len;
				s->ack_num = tcp_header->seq_num + data_len;
			}

			if (flags & TCP_FLAG_FIN)
			{
				send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
				free_socket(s);
			}
		}
	}
}

TcpSocket_T *alloc_socket(TcpSocket_T sockets[MAX_SOCKETS]) 
{
	for (uint8_t i = 0; i < MAX_SOCKETS; i++)
	{
		if (!sockets[i].used)
		{
			sockets[i].used = 1;
			sockets[i].id = i;

			return &sockets[i];
		}
	}

	return 0;
}

void free_socket(TcpSocket_T *sock)
{
	sock->used = 0;
	sock->state = SOCKET_CLOSED;
}

void send_tcp_packet(TcpSocket_T *sock, const uint8_t *data, uint32_t len, uint8_t flags)
{
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

	memcpy(tcp_packet, (uint8_t *) &request, tcp_req_len);

	if (data)
	{
		memcpy(tcp_packet + tcp_req_len, data, len);
	}

	// Create a reply TCP packet
	if (!new_packet(CRAFT_TCP_PACKET, (uint8_t *) tcp_packet))
	{
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
	memcpy(packet_buf, (const uint8_t *) &ipv4_header, ipv4_header_len);
	memcpy(packet_buf + ipv4_header_len, (const uint8_t *) tcp_packet, tcp_header_len);
	if (data && len > 0)
		memcpy(packet_buf + ipv4_header_len + tcp_header_len, data, len);

	if (!new_packet(CRAFT_IPV4_PACKET, (uint8_t *) packet_buf))
	{
		print((const uint8_t *)"-> IPv4 packet creation failed\n");
		return;
	}

	if (!send_packet(0x01, (uint8_t *) packet_buf))
	{
		print((const uint8_t *)"-> Failed to send the IPv4 packet\n");
		return;
	}

	sock->seq_num += len;

	parse_tcp_packet(tcp_packet, &tcp_header);

	printf((const uint8_t *)"<< TCP: (%x) src_port: %u, dest_port: %u, seq %u\n", tcp_header.data_offset_reserved_flags, tcp_header.source_port, tcp_header.dest_port, tcp_header.seq_num);
}

