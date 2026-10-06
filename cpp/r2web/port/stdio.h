#pragma once
#include <stddef.h>
#include <stdarg.h>
#define EOF (-1)
typedef struct { int unused; } FILE;
extern FILE *stderr;
#define snprintf r2js_snprintf
#define vsnprintf r2js_vsnprintf
#define sprintf r2js_sprintf
int r2js_snprintf(char *, size_t, const char *, ...);
int r2js_vsnprintf(char *, size_t, const char *, va_list);
int r2js_sprintf(char *, const char *, ...);
int printf(const char *, ...);
int fputs(const char *, FILE *);
int fputc(int, FILE *);
int putchar(int);
int puts(const char *);
