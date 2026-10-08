#ifndef _R2SYS_H
#define _R2SYS_H

/*
 *  The libcr2 syscall wrappers this C library is built on.  libcr2/syscall.c
 *  goes into libc.a, built with this library's headers on the include path,
 *  which gives it the C library's names (R2_LIBC, libcr2/types.h).
 *
 *  Their conventions vary: read_file_at and write_file_at return a count or -1;
 *  delete_file and rename_file return 1 when done and 0 when not.
 */
#include "../../libcr2/syscall.h"

/* stdio.c: flush every open stream, for exit(). */
void __stdio_flush_all(void);

#endif
