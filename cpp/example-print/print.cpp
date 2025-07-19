#include "syscall.hpp"

extern "C" int64_t syscall(int64_t number, int64_t arg1, int64_t arg2, int64_t arg3) 
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

void exit(int64_t code)
{
	syscall(ScNull, code, 0, 0);

	for (;;)
	{}
}

int64_t print(const char *str) 
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

int64_t read_file(const char *name, char *buffer)
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

extern "C" void main()
{
    const char *s = "Hello from C++!\n";

    print(s);

    const char *filename = "HELLO.TXT";
    char buffer[512];

    if (read_file(filename, buffer))
    {
	    print(buffer);
    }

    exit(999);
}

