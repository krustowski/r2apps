#include "mem.h"
#include "net.h"
#include "printf.h"
#include "string.h"

/*
 *  garn
 *
 *  Sample HTTP server implementation.
 *
 *  krusty@vxn.dev / Aug 5, 2025
 */

int main(int64_t pid, int64_t arg)
{
	uint32_t value = 0;

	uint8_t temp_buf[2048];
	uint8_t packet_buf[2048];
	uint8_t temp_len = 0;
	int64_t decoded_len = 0;

	Ipv4Header_T ipv4_header;
	uint16_t ipv4_header_len = 0;

	TcpHeader_T tcp_header;
	uint8_t tcp_packet[1500];
	uint8_t tcp_header_len = 0;

	TcpSocket_T *server = socket_tcp4();
	bind(server, 80);
	listen(server);

	if (!serial_init())
	{
		// Could not init serial port...
		print("-> Serial port could not be initialized\n");
		exit(pid, 321);
	}

	for (;;)
	{
		// Fetch a byte
		if (!serial_read(&value))
		{
			continue;
		}

		temp_buf[temp_len] = value;
		temp_len++;

		// Try to decode the whole SLIP frame
		decoded_len = decode_slip(temp_buf, temp_len, packet_buf, 2048);
		if (decoded_len <= 0)
		{
			continue;
		}
		temp_len = 0;

		// Parse the IPv4 header 
		ipv4_header_len = parse_ipv4_packet(packet_buf, &ipv4_header);

		if (!ipv4_header_len)
		{
			continue;
		}

		if (ipv4_header.protocol != 6) 
		{
			//print("-> Not TCP, skipping\n");
			continue;
		}

		memcpy(tcp_packet, packet_buf + ipv4_header_len, decoded_len - ipv4_header_len);

		parse_tcp_packet(tcp_packet, &tcp_header);

		printf(">> TCP: src_port: %u, dest_port: %u, seq %u\n", tcp_header.source_port, tcp_header.dest_port, tcp_header.seq_num);

		on_tcp_packet(ipv4_header.source_addr, ipv4_header.destination_addr, &tcp_header, tcp_packet, decoded_len - ipv4_header_len);

		//
		//
		//

		TcpSocket_T *client = accept(server);

		if (!client)
		{
			continue;
		}

		uint8_t buf[1024];
		uint32_t n = read(client, buf, sizeof(buf));

		if (n > 0 && memcmp(buf, "GET /", 6) == 0)
		{
			printf("-> HTTP GET request!\n");

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
