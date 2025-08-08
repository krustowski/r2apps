#include "mem.h"

void *memcpy(void *dest, const void *src, uint16_t n)
{
	uint8_t *d = (uint8_t *)dest;
	const uint8_t *s = (const uint8_t *)src;

	for (uint16_t i = 0; i < n; ++i)
	{
		d[i] = s[i];
	}

	return dest;
}


