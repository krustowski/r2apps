#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    /* eight at a time once both are aligned alike */
    if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
        while (n && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        for (; n >= 8; n -= 8, d += 8, s += 8)
            *(uint64_t *)d = *(const uint64_t *)s;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    while (n--)
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    uint64_t w = (unsigned char)c * 0x0101010101010101UL;

    while (n && ((uintptr_t)d & 7)) {
        *d++ = (unsigned char)c;
        n--;
    }
    for (; n >= 8; n -= 8, d += 8)
        *(uint64_t *)d = w;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;

    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;

    for (; n; n--, p++)
        if (*p == (unsigned char)c)
            return (void *)p;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;

    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);

    while (n-- && *src)
        *d++ = *src++;
    *d = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;

    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; *hay; hay++)
        if (*hay == *needle && !strncmp(hay, needle, n))
            return (char *)hay;
    return n ? 0 : (char *)hay;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;

    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;

    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    s += strcspn(s, accept);
    return *s ? (char *)s : 0;
}

char *strtok(char *s, const char *delim)
{
    static char *next;

    if (!s)
        s = next;
    if (!s)
        return 0;
    s += strspn(s, delim);
    if (!*s) {
        next = 0;
        return 0;
    }
    next = s + strcspn(s, delim);
    if (*next)
        *next++ = 0;
    else
        next = 0;
    return s;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);

    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}

char *strdup(const char *s)
{
    return strndup(s, strlen(s));
}

char *strerror(int err)
{
    switch (err) {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EIO: return "I/O error";
    case EBADF: return "Bad file descriptor";
    case ENOMEM: return "Out of memory";
    case EACCES: return "Permission denied";
    case EEXIST: return "File exists";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case EMFILE: return "Too many open files";
    case ENOSPC: return "No space left on device";
    case ESPIPE: return "Illegal seek";
    case EROFS: return "Read-only file system";
    case EDOM: return "Numerical argument out of domain";
    case ERANGE: return "Numerical result out of range";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS: return "Function not implemented";
    default: return "Unknown error";
    }
}
