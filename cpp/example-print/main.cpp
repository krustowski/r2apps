#include "syscall.hpp"

extern "C" void main(int64_t pid)
{
    const char *s = "Hello from C++!\n";

    print(s);

    const char *filename = "CPP.TXT";

    const char *wbuffer = "Written from C++\n";
    const char *written = "Written to a file\n";

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

