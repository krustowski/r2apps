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


