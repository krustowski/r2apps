#include "string.h"

uint32_t strlen(const uint8_t *str)
{
	uint32_t len = 0;
	while (str[len]) ++len;

	return len;
}

void u32_to_str(uint32_t value, uint8_t *buffer)
{
	uint8_t temp[10];
	uint32_t i = 0;

	if (value == 0)
	{
		buffer[0] = '0';
		buffer[1] = '\0';
		return;
	}

	// Convert digits in reverse order
	while (value > 0 && i < 10)
	{
		temp[i++] = '0' + (value % 10);
		value /= 10;
	}

	// Reverse the digits into the output buffer
	for (uint8_t j = 0; j < i; j++)
	{
		buffer[j] = temp[i - j - 1];
	}

	buffer[i] = '\0';
}


