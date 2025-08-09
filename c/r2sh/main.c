#include "printf.h"
#include "syscall.h"

int main(int64_t pid, int64_t arg)
{
	uint8_t buf[16];
	uint8_t ptr = 0;

	for (uint8_t i = 0; i < sizeof(buf); i++)
	{
		buf[i] = 0x00;
	}

	if (!pipe_subscribe((const uint8_t *) buf))
	{
		print("-> Buffer not subscribed!\n");
	}

	while (3)
	{
		while (buf[ptr])
		{
			printf("%x", buf[ptr]);
			buf[ptr++] = 0x00;
		}

		ptr = 0;
	}

	if (!pipe_unsubscribe((const uint8_t *) buf))
	{
		print("-> Buffer not unsubscribed!\n");
	}

	exit(pid, 345);
}

