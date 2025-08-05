#include "syscall.h"

/*
 *  icmpresp
 *
 *  This program acts like a sample service for the rou2exOS kernel. It is to receive
 *  an ICMP pakcet, to parse it and to respond accordingally.
 *
 *  As the time of the initial development this prog could be run in userspace.
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

int main(int64_t pid, int64_t arg)
{
	print("-> icmpresp service start\n");

	const uint16_t KEYBOARD_PORT = 0x64;
	const uint16_t SERIAL_PORT = 0x3f8;
	uint32_t value = 0;

	uint8_t temp_buf[2048];
	uint8_t packet_buf[2048];
	uint8_t temp_len = 0;


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

		print("-> Received a SLIP frame!\n");
		
		// Parse the IPv4 and ICMP headers...
		// ...
	}

	print("*** Exit\n");
	exit(pid, 111);
}
