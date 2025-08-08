#include "syscall.h"

int main(int64_t pid, int64_t arg)
{
	print((const uint8_t *) "Hello, world!\n");
	exit(pid, 101);
}
