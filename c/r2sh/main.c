#include "syscall.h"

int main(int64_t pid, int64_t *args)
{
	const uint8_t *filename = "C.TXT";
	const uint8_t *wbuffer = "Written from C\n";

	uint8_t rbuffer[512];

	Entry_T entries[32];

	print("*** Hello from C\n");

	if (write_file(filename, wbuffer))
	{
		print("*** Written to a file\n");
	}

	if (read_file(filename, rbuffer))
	{
		print(rbuffer);
	}

	if (!list_dir(0, entries)) 
	{
		print("*** Empty cluster\n");
	}

	exit(123, 999);
}

