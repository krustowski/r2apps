#include "syscall.h"

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
	const uint8_t *filename = "CTEST.TXT";
	const uint8_t *wbuffer = "Written by C using the rou2exOS ABI\n";

	uint8_t rbuffer[512];

	SysInfo_T sysinfo;
	Entry_T entries[32];

	/* Test printing to standard output (console, syscall 0x10) */
	print("*** Hello from C\n");

	/* Test System Information gathering and handling (syscall 0x01) */
	if (read_sysinfo(&sysinfo))
	{
		print("*** Reading system information\n");

		print("System rou2exOS ");
		print(sysinfo.system_version);
		print("\n");

		//printf("System rou2exOS %s\n", sysinfo.system_version);

		print("[");
		print(sysinfo.system_user);
		print("@");
		print(sysinfo.system_name);
		print(":");
		print(sysinfo.system_path);
		print("] > ...\n");

		//printf("[%s@%s:%s] > ...\n", sysinfo.system_user, sysinfo.system_name, sysinfo.system_path);
	}

	/* Test writing to a file (syscall 0x21) */
	if (write_file(filename, wbuffer))
	{
		print("*** Written to a file successfully\n");
	}

	/* Test reading such file back (syscall 0x20) */
	if (read_file(filename, rbuffer))
	{
		print("*** Reading the file contents\n");
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

			if (entries[i].attr & 0x10)
			{
				entries[i].attr = 0x00;
				print( entries[i].name );
				print("   <DIR>\n");
				continue;
			} 
			else
			{
				entries[i].attr = 0x00;
				print( entries[i].name );
			}

			uint8_t bytes[11];
			u32_to_str(entries[i].file_size, bytes);

			print("   ");
			print(bytes);
			print(" bytes");
			print("\n");

			//printf(" %s\n", (const uint8_t *)(entries[i].name));
		}
	}

	print("*** Exit\n");
	exit(pid, 999);
}

