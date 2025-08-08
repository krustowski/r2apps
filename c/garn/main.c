#include "mem.h"
#include "net.h"
#include "print.h"
#include "syscall.h"

/*
 *  garn
 *
 *  Sample HTTP server implementation.
 *
 *  krusty@vxn.dev / Aug 5, 2025
 */

void send_tcp_packet(TcpSocket *sock, const uint8_t* data, uint32_t len, uint8_t flags)
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
}

//
//
//

int main(int64_t pid, int64_t arg)
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

	exit(pid, 456);
}
