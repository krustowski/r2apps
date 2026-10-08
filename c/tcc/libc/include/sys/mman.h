#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stddef.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

/* r2 enables no no-execute bit, so heap memory can run code as it is:
 * mprotect only says yes. */
int mprotect(void *addr, size_t len, int prot);

#endif
