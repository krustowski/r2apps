/* The rest of POSIX that programs reach for, as far as r2 has it. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "r2sys.h"

/* The shell's working directory, which the kernel keeps for everyone. */
char *getcwd(char *buf, size_t size)
{
    SysInfo_T si;
    size_t n;

    memset(&si, 0, sizeof(si));
    read_sysinfo(&si);
    n = strnlen((const char *)si.system_path, sizeof(si.system_path));
    if (n == 0) {
        si.system_path[0] = '/';
        n = 1;
    }
    if (!buf) {
        size = n + 1;
        if (!(buf = malloc(size)))
            return 0;
    }
    if (n + 1 > size) {
        errno = ERANGE;
        return 0;
    }
    memcpy(buf, si.system_path, n);
    buf[n] = 0;
    return buf;
}

int chdir(const char *path)
{
    if (r2_chdir((const uint8_t *)path) != 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int execvp(const char *file, char *const argv[])
{
    (void)file;
    (void)argv;
    errno = ENOSYS;
    return -1;
}

int mprotect(void *addr, size_t len, int prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    return 0;
}
