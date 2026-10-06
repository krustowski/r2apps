#include <r2/heap.hpp>
#include <r2/process.hpp>
#include <r2/libc.hpp>
R2_HEAP_ARENA_KERNEL(1024 * 1024)
// Font discovery hooks used by Memento's software drawing context.
namespace Memento {
const char *getSystemFontName(int *size) { if (size) *size = 16; return "r2font"; }
bool findFond(const char *, char *path, unsigned len) {
    if (path && len > 8) strcpy(path, "/r2font"); return true;
}
}
extern "C" char *strcat(char *dst, const char *src) { strcpy(dst+strlen(dst), src); return dst; }
extern "C" int atoi(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n') ++s;
    bool neg = *s == '-'; if (*s == '-' || *s == '+') ++s;
    unsigned n = 0; while (*s >= '0' && *s <= '9') n = n*10+(*s++-'0');
    return neg ? -(int)n : (int)n;
}
extern "C" int putchar(int c) { return c; }
extern "C" int puts(const char *) { return 0; }
extern "C" int printf(const char *, ...) { return 0; }
