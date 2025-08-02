#include "syscall.h"

int main(int64_t pid, int64_t *args)
{
    const char *s = "*** Hello from C!\n";

    const char *filename = "C.TXT";

    const char *wbuffer = "Written from C\n";
    const char *written = "*** Written to a file\n";

    print(s);


    if (write_file(filename, wbuffer))
    {
	    print(written);
    }

    char rbuffer[512];

    if(read_file(filename, rbuffer))
    {
	    print(rbuffer);
    }

    exit(123, 999);
}

