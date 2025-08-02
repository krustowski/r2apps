#include "syscall.h"

int main(int64_t pid, int64_t arg)
{
	const uint8_t *filename = "C.TXT";
	const uint8_t *wbuffer = "Written from C\n";

	uint8_t rbuffer[512];

	SysInfo_T sysinfo;
	Entry_T entries[32];

	/* Test printing to standard output (console, syscall 0x10) */
	print("*** Hello from C\n");

	if (read_sysinfo(&sysinfo))
	{
		print("*** Reading system information\n");

		print("[");
		print(sysinfo.system_user);
		print("@");
		print(sysinfo.system_name);
		print(":/");
		print("] > ...\n");
	}

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
		print("*** Listing current directory\n");

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

	print("*** Exit\n");
	exit(pid, 999);
}

