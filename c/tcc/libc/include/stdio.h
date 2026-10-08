#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct r2_file FILE;
typedef long fpos_t;

#define EOF (-1)
#define BUFSIZ 4096
#define FILENAME_MAX 64
#define FOPEN_MAX 16

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#define stdin stdin
#define stdout stdout
#define stderr stderr

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
int fclose(FILE *f);
int fflush(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);

size_t fread(void *ptr, size_t size, size_t n, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *s, int size, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int fputs(const char *s, FILE *f);
int puts(const char *s);

int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int fileno(FILE *f);

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *s, const char *fmt, ...);
int snprintf(char *s, size_t size, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);
int vsnprintf(char *s, size_t size, const char *fmt, va_list ap);

int remove(const char *path);
int rename(const char *oldpath, const char *newpath);
void perror(const char *s);

#endif
