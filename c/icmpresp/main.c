#include "syscall.h"

/*
 *  icmpresp
 *
 *  This program acts as a sample service for the rou2exOS kernel. It is to receive
 *  an ICMP pakcet, to parse it and to respond accordingally.
 *
 *  At the time of the initial development this prog could be run in userspace.
 *
 *  krusty@vxn.dev / Aug 5, 2025
 */

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

// Returns number of decoded bytes, or -1 on protocol error, or 0 if frame not yet complete
int decode_slip(const uint8_t *input, uint32_t input_len, uint8_t *output, uint32_t output_len) {
	uint32_t out_pos = 0;
	uint8_t escape = 0;

	for (uint8_t i = 0; i < input_len; ++i) {
		uint8_t b = input[i];

		if (b == SLIP_END) {
			if (out_pos > 0) {
				// Frame complete
				return (int)out_pos;  
			}
			// Else ignore leading END
			continue;
		}

		if (b == SLIP_ESC) {
			escape = 1;
			continue;
		}

		if (escape) {
			if (b == SLIP_ESC_END) {
				b = SLIP_END;
			} else if (b == SLIP_ESC_ESC) {
				b = SLIP_ESC;
			} else {
				// Protocol error
				return -1; 
			}
			escape = 0;
		}

		if (out_pos >= output_len) {
			// Output buffer overflow
			return -1; 
		}

		output[out_pos++] = b;
	}

	// Not finished yet
	return 0; 
}

void *memcpy(void *dest, const void *src, uint16_t n) {
	uint8_t *d = (uint8_t *)dest;
	const uint8_t *s = (const uint8_t *)src;

	for (uint16_t i = 0; i < n; ++i) {
		d[i] = s[i];
	}

	return dest;
}

uint16_t parse_ipv4_packet(const uint8_t *packet, Ipv4Header_T *header)
{
	uint16_t header_len = 0;
	//uint16_t packet_len = 0;

	memcpy(header, packet, sizeof(Ipv4Header_T));
	header_len = header->version & 0x0F * 4;

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

void u32_to_str(uint32_t value, uint8_t *buffer) {
	uint8_t temp[10];
	uint32_t i = 0;

	if (value == 0) {
		buffer[0] = '0';
		buffer[1] = '\0';
		return;
	}

	// Convert digits in reverse order
	while (value > 0 && i < 10) {
		temp[i++] = '0' + (value % 10);
		value /= 10;
	}

	// Reverse the digits into the output buffer
	for (uint8_t j = 0; j < i; j++) {
		buffer[j] = temp[i - j - 1];
	}

	buffer[i] = '\0';
}

int main(int64_t pid, int64_t arg)
{
	print("-> icmpresp service start\n");

	const uint16_t KEYBOARD_PORT = 0x64;
	const uint16_t SERIAL_PORT = 0x3f8;
	uint32_t value = 0;

	uint8_t temp_buf[2048];
	uint8_t packet_buf[2048];
	uint8_t temp_len = 0;

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
		if (!decode_slip(temp_buf, temp_len, packet_buf, 2048))
		{
			continue;
		}

		//print("-> Received a SLIP frame!\n");

		// Parse the IPv4 header 
		ipv4_header_len = parse_ipv4_packet(packet_buf, &ipv4_header);

		if (!ipv4_header_len)
		{
			continue;
		}

		print("-> Received an IPv4 packet (protocol: ");

		uint8_t proto[11];
		u32_to_str((uint32_t) ipv4_header.protocol, proto);

		print(proto);
		print(")\n");

		continue;

		for (uint8_t i = 0; i < 512; i++)
		{
			icmp_packet[i] = packet_buf[ipv4_header_len + i];
		}

		// Parse the ICMP header
		icmp_header_len = parse_icmp_packet(icmp_packet, &icmp_header);

		if (!icmp_header_len)
		{
			continue;
		}

		print("-> Received an ICMP packet!\n");
	}

	print("*** Exit\n");
	exit(pid, 111);
}
