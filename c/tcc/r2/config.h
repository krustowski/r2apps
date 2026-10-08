/*
 *  tcc's configuration for running on r2, in place of the config.h its
 *  configure script writes for a host.
 *
 *  A native x86-64 ELF compiler that links statically.  The files it builds
 *  against live in the boot archive, laid out as on Unix: headers in
 *  /mnt/tar/include (libcr2's in include/r2), crt objects and libraries ---
 *  libc.a, libcr2.a, libtcc1.a --- in /mnt/tar/lib.  {B} below is /mnt/tar,
 *  or what -B says.
 */
#ifndef TCC_VERSION
#error "TCC_VERSION comes from the Makefile (tinycc/VERSION)"
#endif

#define TCC_TARGET_X86_64 1

#define CONFIG_TCCDIR "/mnt/tar"
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include"
#define CONFIG_TCC_LIBPATHS "{B}/lib"
#define CONFIG_TCC_CRTPREFIX "{B}/lib"

/* no dlopen, threads, signal-based backtraces or bounds checking on r2 */
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0

/* tccdefs.h compiled in (tccdefs_.h), rather than read at every start */
#define CONFIG_TCC_PREDEFS 1
