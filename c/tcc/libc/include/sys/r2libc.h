#ifndef _SYS_R2LIBC_H
#define _SYS_R2LIBC_H

/*
 *  The mark of r2's C library.  libcr2's headers look for this file
 *  (libcr2/types.h): where it is, the C names --- the integer types, exit,
 *  chdir, read, write, close, printf, strlen, memcpy --- are this library's,
 *  and libcr2's own calls go by r2_exit, r2_chdir, tcp_read, tcp_write and
 *  tcp_close.
 */
#define R2_LIBC 1

#endif
