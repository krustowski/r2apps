/* string.h --- the C string functions QuickJS uses; libc++r2 supplies them. */
#ifndef JSR2_PORT_STRING_H
#define JSR2_PORT_STRING_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *);
char *strcpy(char *, const char *);
char *strcat(char *, const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);
char *strstr(const char *, const char *);
#ifdef __cplusplus
}
#endif
#endif
