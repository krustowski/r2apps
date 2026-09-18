/*
 *  stdcxx_compat.cpp — symbols a host libstdc++ expects to find.
 *
 *  libc++r2 does not need any of this.  It exists for the other case: a
 *  program that links the host's libstdc++.a for std::string, std::vector or
 *  the like, as cpp/memento-hello does.  That archive is compiled against
 *  glibc and drags in references to parts of it that never run on r2 --- the
 *  verbose terminate handler wanting fprintf, the demangler wanting sprintf,
 *  system_error wanting strerror_r --- and the link fails on symbols the
 *  program will never call.
 *
 *  Link libc++r2compat.a after libstdc++ to satisfy them.  Everything here is
 *  either a no-op or the smallest honest implementation; the ones that would
 *  be wrong to fake, such as the allocator, are routed to r2::heap instead.
 *
 *  These are kept out of libc++r2.a on purpose.  c/libcr2 defines malloc,
 *  free and realloc too, so a program linking both libraries would otherwise
 *  get a duplicate definition; this way it only sees them if it asks.
 *
 *  Collected from the stubs in cpp/memento-hello/r2_stubs.cpp.
 */

#include "r2/heap.hpp"
#include "r2/libc.hpp"

extern "C" {

/* ------------------------------------------------------------------------ *
 *  Allocation — the C names, onto the same arena operator new uses.
 *
 *  Note that this is deliberately not the kernel's malloc (syscall 0x0a):
 *  blocks from that heap live at 0xC00_000 and are rejected by every syscall
 *  that takes a pointer, so a buffer allocated there could never be printed
 *  or written to a file.
 * ------------------------------------------------------------------------ */

void *malloc(size_t size) { return r2::heap::allocate(size); }

void free(void *ptr) { r2::heap::deallocate(ptr); }

void *realloc(void *ptr, size_t size) { return r2::heap::reallocate(ptr, size); }

void *calloc(size_t count, size_t size) {
    size_t total = count * size;
    if (count != 0 && total / count != size)
        return nullptr; /*  multiplication overflowed  */

    void *block = r2::heap::allocate(total);
    if (block)
        memset(block, 0, total);
    return block;
}

void *reallocarray(void *ptr, size_t count, size_t size) {
    size_t total = count * size;
    if (count != 0 && total / count != size)
        return nullptr;
    return r2::heap::reallocate(ptr, total);
}

/* ------------------------------------------------------------------------ *
 *  stdio — there is no stderr on r2, and nothing here is reached on a path
 *  the program can recover from anyway.
 * ------------------------------------------------------------------------ */

typedef struct {
    int unused;
} r2_FILE;

r2_FILE r2_stderr_storage;
r2_FILE *stderr = &r2_stderr_storage;
r2_FILE *stdout = &r2_stderr_storage;

int fprintf(r2_FILE *, const char *, ...) { return 0; }
int fputc(int, r2_FILE *) { return 0; }
int fputs(const char *, r2_FILE *) { return 0; }
int fflush(r2_FILE *) { return 0; }
size_t fwrite(const void *, size_t, size_t count, r2_FILE *) { return count; }

/*  The demangler formats into these; it is never on a live path here.  */
int sprintf(char *buffer, const char *, ...) {
    if (buffer)
        buffer[0] = '\0';
    return 0;
}

int snprintf(char *buffer, size_t size, const char *, ...) {
    if (buffer && size)
        buffer[0] = '\0';
    return 0;
}

/* ------------------------------------------------------------------------ *
 *  errno, threads, locale
 * ------------------------------------------------------------------------ */

static int s_errno = 0;
int *__errno_location() { return &s_errno; }

int __libc_single_threaded = 1;

int strerror_r(int, char *buffer, size_t size) {
    if (buffer && size)
        buffer[0] = '\0';
    return 0;
}

const char *gettext(const char *text) { return text; }
const char *dcgettext(const char *, const char *text, int) { return text; }

typedef struct {
    long opaque[5];
} r2_pthread_mutex_t;

int pthread_mutex_lock(r2_pthread_mutex_t *) { return 0; }
int pthread_mutex_unlock(r2_pthread_mutex_t *) { return 0; }
int pthread_mutex_destroy(r2_pthread_mutex_t *) { return 0; }

char *secure_getenv(const char *) { return nullptr; }
char *getenv(const char *) { return nullptr; }

int getentropy(void *, size_t) { return 0; }
unsigned int arc4random() { return 0x12345678u; }

/*  glibc 2.38 renamed the strtoul that libstdc++ calls.  */
unsigned long __isoc23_strtoul(const char *text, char **end, int base) {
    unsigned long result = 0;

    while (*text == ' ' || *text == '\t')
        text++;

    if (base == 16 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;

    while (*text) {
        int digit;
        if (*text >= '0' && *text <= '9')
            digit = *text - '0';
        else if (*text >= 'a' && *text <= 'f')
            digit = *text - 'a' + 10;
        else if (*text >= 'A' && *text <= 'F')
            digit = *text - 'A' + 10;
        else
            break;

        if (digit >= base)
            break;

        result = result * (unsigned long)base + (unsigned long)digit;
        text++;
    }

    if (end)
        *end = (char *)text;
    return result;
}

unsigned long strtoul(const char *text, char **end, int base) {
    return __isoc23_strtoul(text, end, base);
}

/* ------------------------------------------------------------------------ *
 *  The unwinder.  libstdc++.a carries the exception ABI whether or not the
 *  program throws, and it references these.  Reaching one means something did
 *  throw, which cannot be handled here.
 * ------------------------------------------------------------------------ */

struct _Unwind_Exception {
    unsigned long long exception_class;
    void (*exception_cleanup)(int, struct _Unwind_Exception *);
    unsigned long private_1;
    unsigned long private_2;
};

struct _Unwind_Context {
    int unused;
};

typedef int _Unwind_Reason_Code;

__attribute__((noreturn)) _Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *) {
    for (;;) {
    }
}

__attribute__((noreturn)) _Unwind_Reason_Code
_Unwind_Resume_or_Rethrow(struct _Unwind_Exception *) {
    for (;;) {
    }
}

__attribute__((noreturn)) void _Unwind_Resume(struct _Unwind_Exception *) {
    for (;;) {
    }
}

void _Unwind_DeleteException(struct _Unwind_Exception *) {}
void _Unwind_SetGR(struct _Unwind_Context *, int, unsigned long) {}
void _Unwind_SetIP(struct _Unwind_Context *, unsigned long) {}
unsigned long _Unwind_GetIPInfo(struct _Unwind_Context *, int *) { return 0; }
unsigned long _Unwind_GetRegionStart(struct _Unwind_Context *) { return 0; }
unsigned long _Unwind_GetDataRelBase(struct _Unwind_Context *) { return 0; }
unsigned long _Unwind_GetTextRelBase(struct _Unwind_Context *) { return 0; }
unsigned long _Unwind_GetLanguageSpecificData(struct _Unwind_Context *) { return 0; }

} // extern "C"
