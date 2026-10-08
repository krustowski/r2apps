#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* r2 has no environment: always NULL. */
extern char **environ;

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
off_t lseek(int fd, off_t off, int whence);
int close(int fd);
int unlink(const char *path);
int isatty(int fd);

/* The working directory is the kernel's, shared with the shell. */
char *getcwd(char *buf, size_t size);
int chdir(const char *path);

/* r2 cannot replace a running program: these fail with ENOSYS. */
int execvp(const char *file, char *const argv[]);

#endif
