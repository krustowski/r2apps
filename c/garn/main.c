#include "syscall.h"

/*
 *  garn
 *
 *  Sample HTTP server implementation.
 *
 *  krusty@vxn.dev / Aug 5, 2025
 */

#define MAX_SOCKETS	8
#define RX_BUFFER_SIZE  1024
#define TX_BUFFER_SIZE  1024

#define TCP_FLAG_FIN	0x01
#define TCP_FLAG_SYN	0x02
#define TCP_FLAG_ACK	0x10

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

void bind(TcpSocket *sock, uint16_t port);
void listen(TcpSocket *sock);
TcpSocket *accept(TcpSocket *listener);
uint32_t read(TcpSocket *sock, uint8_t *buf, uint32_t maxlen);
uint32_t write(TcpSocket *sock, const uint8_t *buf, uint32_t len);
void close(TcpSocket *sock);
void on_tcp_packet(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint8_t flags, const uint8_t *payload, uint32_t len);

//
//
//

uint32_t strlen(const char *str)
{
	uint32_t len = 0;
	while (str[len]) ++len;

	return len;
}

uint32_t memcmp(const uint8_t *s1, const uint8_t *s2, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++)
	{
		if (s1[i] != s2[i])
		{
			return s1[i] - s2[i];
		}
	}

	return 0;
}

static TcpSocket *alloc_socket() 
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

static void free_socket(TcpSocket *sock)
{
	sock->used = 0;
	sock->state = SOCKET_CLOSED;
}

//
//
//

TcpSocket *socket_tcp4()
{
	TcpSocket *sock = alloc_socket();
	if (!sock)
	{
		return 0;
	}

	sock->state = SOCKET_CLOSED;

	return sock;
}

void bind(TcpSocket *sock, uint16_t port)
{
	sock->local_port = port;
}

void listen(TcpSocket *sock)
{
	sock->state = SOCKET_LISTENING;
}

TcpSocket *accept(TcpSocket *listener)
{
	for (uint8_t i = 0; i < MAX_SOCKETS; i++)
	{
		TcpSocket *s = &sockets[i];

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

uint32_t read(TcpSocket *sock, uint8_t *buf, uint32_t maxlen)
{
	uint32_t n = (sock->rx_len < maxlen) ? sock->tx_len : maxlen;

	for (uint32_t i = 0; i < n; i++)
	{
		buf[i] = sock->rx_buffer[i];
	}

	sock->rx_len = 0;
	return n;
}

uint32_t write(TcpSocket *sock, const uint8_t *buf, uint32_t len)
{
	send_tcp_packet(sock, buf, len, TCP_FLAG_ACK);
	return len;
}

void close(TcpSocket *sock)
{
	send_tcp_packet(sock, 0, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
	sock->state = SOCKET_FIN_WAIT;
}

void on_tcp_packet(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint8_t flags, const uint8_t *payload, uint32_t len)
{
	for (uint8_t i = 0; i < MAX_SOCKETS; i++)
	{
		TcpSocket *s = &sockets[i];

		if (!s->used || s->local_port != dst_port)
		{
			continue;
		}

		if (s->state == SOCKET_LISTENING && (flags & TCP_FLAG_SYN))
		{
			TcpSocket *new_conn = alloc_socket();

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

//
//
//


/*void send_tcp_packet(TcpSocket *sock, const uint8_t* data, uint32_t len, uint8_t flags)
{
	uint8_t tcp_packet[TX_BUFFER_SIZE];
	TcpHeader_T tcp_header;

	tcp_header.source_port = sock->src_port;
	tcp_header.dest_port = sock->dst_port;

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

//
//
//

void run_http_server()
{
	TcpSocket *server = socket_tcp4();
	bind(server, 80);
	listen(server);

	for (;;)
	{
		TcpSocket *client = accept(server);

		if (!client)
		{
			continue;
		}

		uint8_t buf[1024];
		uint32_t n = read(client, buf, sizeof(buf));

		if (n > 0 && memcmp(buf, "GET /", 6) == 0)
		{
			const uint8_t *response = 
				"HTTP/1.0 200 OK\r\n"
				"Content-Length: 13\r\n"
				"\r\n"
				"Hello, world!";
			write(client, (const uint8_t *) response, strlen(response));
			close(client);
		}
	}
}

//
//
//

int main(int64_t pid, int64_t arg)
{
	exit(pid, 456);
}
