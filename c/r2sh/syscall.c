#include "syscall.h"

int64_t syscall(int64_t number, int64_t arg1, int64_t arg2, int64_t arg3) 
{
    int64_t ret;
    asm volatile (
        "int $0x7f"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void exit(int64_t pid, int64_t code)
{
	syscall(ScExit, pid, code, 0);

	for (;;)
	{}
}

int64_t print(const uint8_t *str) 
{
	int64_t len = 0;
	while (str[len]) ++len;

	if (len > 0)
	{
	    	if (syscall(ScPrints, (int64_t)str, len, 0))
		{
			return 0;
		}
	}

	return len;
}

int64_t read_file(const uint8_t *name, uint8_t *buffer)
{
	int64_t len = 0;
	while (name[len]) ++len;

	if (len > 0)
	{
		if (syscall(ScReadFile, (int64_t)name, (int64_t)buffer, 0))
		{
			return 0;
		}

		return 1;
	}

	return 0;
}

int64_t write_file(const uint8_t *name, const uint8_t *buffer)
{
	int64_t len = 0;
	while (name[len]) ++len;

	if (len > 0)
	{
		if (syscall(ScWriteFile, (int64_t)name, (int64_t)buffer, 0))
		{
			return 0;
		}

		return 1;
	}

	return 0;
}

int64_t list_dir(int64_t cluster, Entry_T entries[32])
{
	if (syscall(ScListDir, cluster, (int64_t)entries, 0))
	{
		return 0;
	}

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

	return 1;
}

