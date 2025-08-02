#include "syscall.h"

int main(int64_t pid, int64_t *args)
{
    const char *filename = "C.TXT";
    const char *wbuffer = "Written from C\n";

    char rbuffer[512];

    print("*** Hello from C\n");

    if (write_file(filename, wbuffer))
    {
	    print("*** Written to a file\n");
    }

    if (read_file(filename, rbuffer))
    {
	    print(rbuffer);
    }

    if (list_dir()) 
    {
	    print("*** Pochcal sem sa\n");
    }

    exit(123, 999);
}

