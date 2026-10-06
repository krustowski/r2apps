#pragma once
#include <stddef.h>
void *malloc(size_t);
void *realloc(void *, size_t);
void free(void *);
void abort(void) __attribute__((noreturn));
int atoi(const char *);
