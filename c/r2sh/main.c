#include "syscall.h"

int main(int64_t pid, int64_t arg)
{
	const uint8_t *filename = "C.TXT";
	const uint8_t *wbuffer = "Written from C\n";

	uint8_t rbuffer[512];

	Entry_T entries[32];

	/* Test printing to standard output (console, syscall 0x10) */
	print("*** Hello from C\n");

	/* Test writing to a file (syscall 0x21) */
	if (write_file(filename, wbuffer))
	{
		print("*** Written to a file\n");
	}

	/* Test reading such file back (syscall 0x20) */
	if (read_file(filename, rbuffer))
	{
		print(rbuffer);
	}

	/* Test listing a directory (syscall 0x28) */
	if (list_dir(0, entries)) 
	{
		for (uint8_t i = 0; i < 32; i++) 
		{
			if (entries[i].name[0] == 0x00) 
			{
				continue;
			}

			print(" ");
			print( (const uint8_t*)(entries[i].name) );
			print("\n");
		}
	}

	exit(pid, 999);
}

