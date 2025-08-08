#include "mem.h"

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


