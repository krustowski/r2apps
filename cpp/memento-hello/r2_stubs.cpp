// r2 C++ runtime stubs.
// Provides C runtime and Memento platform symbols not available in libcr2.

// ---------------------------------------------------------------------------
// User-space bump allocator — kernel malloc returns non-user-accessible memory.
// This replaces malloc/free for the entire process; all allocations come from
// a static BSS region within the 2 MiB user frame (0x600000–0x7FFFFF).
// ---------------------------------------------------------------------------
static unsigned char r2_heap[2 * 1024 * 1024]; // 2 MB: four 320×200 canvases (256 KB each) + overhead
static unsigned long r2_heap_used = 0;

extern "C" {

unsigned long r2_heap_checkpoint() { return r2_heap_used; }
void r2_heap_restore(unsigned long cp) { r2_heap_used = cp; }

void* malloc(unsigned long size) {
    if (size == 0) size = 1;
    unsigned long aligned = (r2_heap_used + 15UL) & ~15UL; // 16-byte alignment
    if (aligned + size > sizeof(r2_heap)) return nullptr;
    r2_heap_used = aligned + size;
    return &r2_heap[aligned];
}

void free(void* /*ptr*/) {} // bump allocator: no reclaim

void* realloc(void* /*old*/, unsigned long size) {
    return malloc(size); // leak old; sufficient for our usage pattern
}

} // extern "C"

// operator new/delete must be outside extern "C"
void* operator new  (unsigned long sz)          { return malloc(sz); }
void* operator new[](unsigned long sz)          { return malloc(sz); }
void  operator delete  (void* p) noexcept       { free(p); }
void  operator delete[](void* p) noexcept       { free(p); }
void  operator delete  (void* p, unsigned long) noexcept { free(p); }
void  operator delete[](void* p, unsigned long) noexcept { free(p); }

extern "C" {

// memset / calloc — not in libcr2
// __attribute__((optimize("O0"))): -O2 loop-distribute-patterns would otherwise
// recognize the fill loop as a memset idiom and replace it with a recursive
// call to memset itself, causing a stack overflow for any variable-count call.
__attribute__((optimize("O0")))
void* memset(void* dst, int c, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void* calloc(unsigned long nmemb, unsigned long size) {
    // BSS-backed bump heap is zero-initialized; no need to memset.
    return malloc(nmemb * size);
}

// memmove — libcr2 has memcpy but not memmove
void* memmove(void* dst, const void* src, unsigned long n) {
    unsigned char*       d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (unsigned long i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dst;
}

// memchr / strcmp / strncmp / strchr — used by libstdc++ string/RTTI code
void* memchr(const void* s, int c, unsigned long n) {
    const unsigned char* p = (const unsigned char*)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void*)p;
        p++;
    }
    return nullptr;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, unsigned long n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == 0) ? (char*)s : nullptr;
}

// round — used by PlatformScaler
double round(double x) {
    return (double)(long long)(x >= 0.0 ? x + 0.5 : x - 0.5);
}

// Pure-virtual call stub
void __cxa_pure_virtual() { for(;;) {} }

// abort — infinite loop (no OS to terminate to)
__attribute__((noreturn)) void abort() { for(;;) {} }

// C++ runtime init stubs
void* __dso_handle = nullptr;
int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
int __cxa_finalize(void*) { return 0; }

// errno
static int s_errno = 0;
int* __errno_location() { return &s_errno; }
int __libc_single_threaded = 1;

// strerror_r — used by libstdc++ system_error (never called in our code)
int strerror_r(int, char* buf, unsigned long) {
    if (buf) buf[0] = '\0';
    return 0;
}

// sprintf — used by cp-demangle (never called in our code paths)
int sprintf(char* buf, const char* fmt, ...) { (void)fmt; if (buf) buf[0]='\0'; return 0; }

// stdio no-ops — libstdc++ links these but we never write to stderr
typedef struct { int _dummy; } r2_FILE;
r2_FILE  s_stderr_buf;
r2_FILE* stderr = &s_stderr_buf;
int      fprintf(r2_FILE*, const char*, ...)            { return 0; }
int      fputc  (int, r2_FILE*)                         { return 0; }
int      fputs  (const char*, r2_FILE*)                 { return 0; }
unsigned long fwrite(const void*, unsigned long, unsigned long, r2_FILE*) { return 0; }

// gettext — locale stub
const char* gettext(const char* s) { return s; }
const char* dcgettext(const char*, const char* s, int) { return s; }

// secure_getenv / getentropy / arc4random — not needed on r2
char*         secure_getenv(const char*)       { return nullptr; }
int           getentropy(void*, unsigned long)  { return 0; }
unsigned int  arc4random()                     { return 0x12345678u; }

// strtoul variant from newer glibc
unsigned long __isoc23_strtoul(const char* s, char**, int base) {
    unsigned long result = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (base == 16 && s[0] == '0' && (s[1]=='x'||s[1]=='X')) s += 2;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        result = result * (unsigned long)base + (unsigned long)digit;
        s++;
    }
    return result;
}

// Exception-unwinding stubs — never reached with -fno-exceptions, but libstdc++.a
// references them from the C++ exception ABI that it bundles even when unused.
struct _Unwind_Exception {
    unsigned long long exception_class;
    void (*exception_cleanup)(int, struct _Unwind_Exception*);
    unsigned long private_1, private_2;
};
struct _Unwind_Context { int _dummy; }; // empty struct is size 0 in C, 1 in C++
typedef int _Unwind_Reason_Code;

__attribute__((noreturn)) _Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception*) { for(;;){} }
__attribute__((noreturn)) _Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception*) { for(;;){} }
__attribute__((noreturn)) void                _Unwind_Resume(struct _Unwind_Exception*) { for(;;){} }
void          _Unwind_DeleteException(struct _Unwind_Exception*) {}
void          _Unwind_SetGR(struct _Unwind_Context*, int, unsigned long) {}
void          _Unwind_SetIP(struct _Unwind_Context*, unsigned long) {}
unsigned long _Unwind_GetIPInfo(struct _Unwind_Context*, int*) { return 0; }
unsigned long _Unwind_GetRegionStart(struct _Unwind_Context*) { return 0; }
unsigned long _Unwind_GetDataRelBase(struct _Unwind_Context*) { return 0; }
unsigned long _Unwind_GetTextRelBase(struct _Unwind_Context*) { return 0; }
unsigned long _Unwind_GetLanguageSpecificData(struct _Unwind_Context*) { return 0; }

// pthread stubs — single-threaded on r2
typedef struct { long __p[5]; } pthread_mutex_t;
int pthread_mutex_lock  (pthread_mutex_t*) { return 0; }
int pthread_mutex_unlock(pthread_mutex_t*) { return 0; }

// blit_buffer_scaled — ScBlitBuffer(0x17) with arg2=(src_w<<16)|src_h for nearest-neighbour scale
// Not in pre-compiled libcr2.a; implement here using the int $0x7f ABI directly.
long blit_buffer_scaled(const unsigned int* pixels, unsigned int src_w, unsigned int src_h) {
    long dims = ((long)src_w << 16) | (long)src_h;
    long ret;
    asm volatile("int $0x7f"
                 : "=a"(ret)
                 : "d"(0x17L), "D"((long)pixels), "S"(dims), "c"(0L)
                 : "r11", "memory");
    return ret;
}

} // extern "C"

// Memento platform stubs — font discovery not available on r2.
// R2_FontImpl uses get_kernel_font (PSF) and ignores the path entirely.
// We return non-null dummy values so PlatformFontManager::HasError() is satisfied.
namespace Memento {
    using mchar  = char;
    using int32  = int;
    using uint32 = unsigned int;

    static const mchar s_r2FontName[] = "r2font";
    static const mchar s_r2FontPath[] = "/r2font";

    const mchar* getSystemFontName(int32* sz) {
        if (sz) *sz = 16;
        return s_r2FontName;
    }

    bool findFond(const mchar*, mchar* buf, uint32 len) {
        if (buf && len > sizeof(s_r2FontPath)) {
            for (uint32 i = 0; i < sizeof(s_r2FontPath); i++)
                buf[i] = s_r2FontPath[i];
        }
        return true;
    }
}
