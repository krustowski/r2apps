#include "mem.h"
#include "net.h"
#include "print.h"
#include "syscall.h"

/*
 *  icmpresp
 *
 *  This program acts as a sample service for the rou2exOS kernel. It is to receive
 *  an ICMP packet, to parse it and to respond accordingally.
 *
 *  At the time of the initial development this prog could be run in userspace.
 *
 *  krusty@vxn.dev / Aug 5, 2025
 */

int main(int64_t pid, int64_t arg)
{
	print("-> icmpresp service start\n");

	const uint16_t KEYBOARD_PORT = 0x64;
	const uint16_t SERIAL_PORT = 0x3f8;
	uint32_t value = 0;

	uint8_t temp_buf[2048];
	uint8_t packet_buf[2048];
	uint8_t temp_len = 0;
	int64_t decoded_len = 0;

	Ipv4Header_T ipv4_header;
	uint16_t ipv4_header_len = 0;

	IcmpHeader_T icmp_header;
	uint8_t icmp_packet[512];
	uint8_t icmp_header_len = 0;

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

		//print("-> Received a SLIP frame!\n");

		// Parse the IPv4 header 
		ipv4_header_len = parse_ipv4_packet(packet_buf, &ipv4_header);

		if (!ipv4_header_len)
		{
			continue;
		}

		/*print("-> Received an IPv4 packet (protocol: ");

		  uint8_t proto[11];
		  u32_to_str((uint32_t) ipv4_header.protocol, proto);

		  print(proto);
		  print(")\n");*/

		if (ipv4_header.protocol != 1) 
		{
			//print("-> Not ICMP, skipping\n");
			continue;
		}

		memcpy(icmp_packet, packet_buf + ipv4_header_len, decoded_len - ipv4_header_len);

		// Parse the ICMP header
		icmp_header_len = parse_icmp_packet(icmp_packet, &icmp_header);

		if (!icmp_header_len)
		{
			continue;
		}

		if (icmp_header.type != 8 || icmp_header.code != 0)
		{
			print("-> Unknown ICMP type or code\n");
			continue;
		}

		print("-> Received an ICMP Echo Request!\n");

		// Create a reply ICMP packet
		if (!new_packet(0x02, (uint8_t *) icmp_packet))
		{
			print("-> ICMP packet creation failed\n");
			continue;
		}

		// Copy the ICMP packet into IPv4 packet
		memcpy(packet_buf + ipv4_header_len, icmp_packet, decoded_len - ipv4_header_len);

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

		print("-> Response sent\n");
	}

	print("*** Exit\n");
	exit(pid, 111);
}
