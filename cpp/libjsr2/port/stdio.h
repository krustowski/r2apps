/*
 *  stdio.h --- formatting for QuickJS on r2: stb_sprintf behind jsr2_ names
 *  (src/libc.c), so they cannot collide with libc++r2's stubs.  Output to a
 *  FILE goes to the platform's log hook (jsr2::Platform::log).
 */
#ifndef JSR2_PORT_STDIO_H
#define JSR2_PORT_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
#define EOF (-1)
typedef struct jsr2_file { int fd; } FILE;
extern FILE *jsr2_stdout, *jsr2_stderr;
#define stdout jsr2_stdout
#define stderr jsr2_stderr
#define snprintf jsr2_snprintf
#define vsnprintf jsr2_vsnprintf
#define sprintf jsr2_sprintf
#define printf jsr2_printf
#define fprintf jsr2_fprintf
#define vfprintf jsr2_vfprintf
#define fputs jsr2_fputs
#define fputc jsr2_fputc
#define putc jsr2_fputc
#define putchar jsr2_putchar
#define puts jsr2_puts
#define fwrite jsr2_fwrite
#define fflush jsr2_fflush
int jsr2_snprintf(char *, size_t, const char *, ...);
int jsr2_vsnprintf(char *, size_t, const char *, va_list);
int jsr2_sprintf(char *, const char *, ...);
int jsr2_printf(const char *, ...);
int jsr2_fprintf(FILE *, const char *, ...);
int jsr2_vfprintf(FILE *, const char *, va_list);
int jsr2_fputs(const char *, FILE *);
int jsr2_fputc(int, FILE *);
int jsr2_putchar(int);
int jsr2_puts(const char *);
size_t jsr2_fwrite(const void *, size_t, size_t, FILE *);
int jsr2_fflush(FILE *);
#ifdef __cplusplus
}
#endif
#endif
