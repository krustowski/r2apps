#include "syscall.h"

int main(int64_t pid, int64_t arg)
{
	print("Hello, world!\n");
	exit(pid, 101);
}
