#include "net.h"

// Returns number of decoded bytes, or -1 on protocol error, or 0 if frame not yet complete
int64_t decode_slip(const uint8_t *input, uint32_t input_len, uint8_t *output, uint32_t output_len)
{
	uint32_t out_pos = 0;
	uint8_t escape = 0;

	for (uint8_t i = 0; i < input_len; ++i)
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

TcpSocket_T *socket_tcp4()
{
	TcpSocket_T *sock = alloc_socket();
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

TcpSocket_T *accept(TcpSocket_T *listener)
{
	for (uint8_t i = 0; i < MAX_SOCKETS; i++)
	{
		TcpSocket_T *s = &sockets[i];

		if (s->used && s->state == SOCKET_ESTABLISHED && s->local_port == listener->local_port)
		{
			if (s != listener)
			{
				return s;
			}
		}
	}

	return 0;
}

uint32_t read(TcpSocket_T *sock, uint8_t *buf, uint32_t maxlen)
{
	uint32_t n = (sock->rx_len < maxlen) ? sock->tx_len : maxlen;

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
	sock->state = SOCKET_FIN_WAIT;
}

void on_tcp_packet(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint8_t flags, const uint8_t *payload, uint32_t len)
{
	for (uint8_t i = 0; i < MAX_SOCKETS; i++)
	{
		TcpSocket_T *s = &sockets[i];

		if (!s->used || s->local_port != dst_port)
		{
			continue;
		}

		if (s->state == SOCKET_LISTENING && (flags & TCP_FLAG_SYN))
		{
			TcpSocket_T *new_conn = alloc_socket();

			if (!new_conn)
			{
				return;
			}

			new_conn->local_port = dst_port;
			new_conn->remote_port = src_port;
			new_conn->remoze_ip = src_ip;
			new_conn->state = SOCKET_ESTABLISHED;

			send_tcp_packet(new_conn, 0, 0, TCP_FLAG_SYN | TCP_FLAG_ACK);
			return;
		}

		if (s->state == SOCKET_ESTABLISHED && s->remoze_ip == src_ip && s->remote_port == src_port)
		{
			if (len > 0)
			{
				for (uint32_t j = 0; j < len && j < RX_BUFFER_SIZE; j++)
				{
					s->rx_buffer[j] = payload[j];
				}

				s->rx_len = len;
			}

			if (flags & TCP_FLAG_FIN)
			{
				send_tcp_packet(s, 0, 0, TCP_FLAG_ACK);
				free_socket(s);
			}
		}
	}
}

static TcpSocket_T *alloc_socket() 
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

static void free_socket(TcpSocket_T *sock)
{
	sock->used = 0;
	sock->state = SOCKET_CLOSED;
}

void send_tcp_packet(TcpSocket_T *sock, const uint8_t* data, uint32_t len, uint8_t flags)
{}
/*void send_tcp_packet(TcpSocket_T *sock, const uint8_t* data, uint32_t len, uint8_t flags)
{
	uint8_t tcp_packet[TX_BUFFER_SIZE];
	TcpHeader_T tcp_header;

	tcp_header.source_port = sock->local_port;
	tcp_header.dest_port = sock->remote_port;

	//memcpy(header, packet, sizeof(IcmpHeader_T));

	// Create a reply TCP packet
	if (!new_packet(0x03, (uint8_t *) tcp_packet))
	{
		print("-> TCP packet creation failed\n");
		continue;
	}

	// Copy the TCP packet into IPv4 packet
	memcpy(packet_buf + ipv4_header_len, tcp_packet, decoded_len - ipv4_header_len);

	if (!new_packet(0x01, (uint8_t *) packet_buf))
	{
		print("-> IPv4 packet creation failed\n");
		continue;
	}

	if (!send_packet(0x01, packet_buf))
	{
		print("-> Failed to send the IPv4 packet\n");
		continue;
	}
}*/

