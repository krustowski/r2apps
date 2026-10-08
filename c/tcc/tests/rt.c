/* What libcr2 lacks and the compiler runtime calls into. */
#include "syscall.h"

void abort(void)
{
    exit(0, 134);
}

void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}
