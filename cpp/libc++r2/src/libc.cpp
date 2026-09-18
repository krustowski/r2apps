/*
 *  libc.cpp — the C functions the compiler assumes exist.
 *
 *  GCC turns a struct copy into memcpy and a zeroed array into memset whatever
 *  -ffreestanding says, so these have to be here for ordinary C++ to link.
 *
 *  The pragma matters.  At -O2 the loop-distribution pass recognises a byte
 *  fill loop as a memset idiom and rewrites it as a call to memset --- inside
 *  memset itself, which is then infinitely recursive and overflows the stack
 *  on the first variable-length call.  Turning that pass off for this file is
 *  the fix; the Makefile passes the same flag, and the pragma keeps the file
 *  correct even when someone compiles it by hand.
 */

#pragma GCC optimize("no-tree-loop-distribute-patterns")

#include "r2/libc.hpp"

namespace {

/*  Word-at-a-time is worth it: these run on every vector growth and every
 *  frame a graphical program blits.  */
constexpr size_t WORD = sizeof(uint64_t);

inline bool same_alignment(const void *a, const void *b) {
    return ((uintptr_t)a & (WORD - 1)) == ((uintptr_t)b & (WORD - 1));
}

} // namespace

extern "C" {

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (n >= WORD * 4 && same_alignment(d, s)) {
        while (((uintptr_t)d & (WORD - 1)) != 0 && n > 0) {
            *d++ = *s++;
            n--;
        }
        uint64_t *dw = (uint64_t *)d;
        const uint64_t *sw = (const uint64_t *)s;
        while (n >= WORD) {
            *dw++ = *sw++;
            n -= WORD;
        }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }

    while (n--)
        *d++ = *s++;

    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0)
        return dst;

    if (d < s)
        return memcpy(dst, src, n);

    /*  Overlapping and moving up: copy from the top down.  */
    d += n;
    s += n;
    while (n--)
        *--d = *--s;

    return dst;
}

void *memset(void *dst, int value, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char byte = (unsigned char)value;

    if (n >= WORD * 4) {
        while (((uintptr_t)d & (WORD - 1)) != 0 && n > 0) {
            *d++ = byte;
            n--;
        }
        uint64_t pattern = 0x0101010101010101ULL * byte;
        uint64_t *dw = (uint64_t *)d;
        while (n >= WORD) {
            *dw++ = pattern;
            n -= WORD;
        }
        d = (unsigned char *)dw;
    }

    while (n--)
        *d++ = byte;

    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char target = (unsigned char)c;

    for (size_t i = 0; i < n; i++)
        if (p[i] == target)
            return (void *)(p + i);

    return nullptr;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
    char *out = dst;
    while ((*out++ = *src++) != '\0')
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    char target = (char)c;
    while (*s) {
        if (*s == target)
            return (char *)s;
        s++;
    }
    return target == '\0' ? (char *)s : nullptr;
}

char *strrchr(const char *s, int c) {
    char target = (char)c;
    const char *found = nullptr;
    while (*s) {
        if (*s == target)
            found = s;
        s++;
    }
    if (target == '\0')
        return (char *)s;
    return (char *)found;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle)
        return (char *)haystack;

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
    }
    return nullptr;
}

} // extern "C"
