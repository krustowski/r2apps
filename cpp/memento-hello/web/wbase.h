#pragma once

//
//  wbase.h --- what every part of the web engine stands on.
//
//  The engine is written against this header and nothing else from the
//  platform, so that the parts that do not touch the machine --- URLs, HTTP,
//  HTML, layout, the TLS pump --- build on the host too, where they can be
//  tested against real servers (see tests/).  WEB_HOST selects the host.
//
//  The platform supplies the functions declared at the bottom: web_r2.cpp on
//  r2, the test harness on the host.
//

#ifdef WEB_HOST
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#else
#include <r2/libc.hpp>
#endif

namespace web {

//
//  Memory.
//
//  Two pools.  The small one is the program's arena: fast, but shared with
//  everything else Memento has open.  The big one is for the few buffers that
//  grow with the page --- the raw document, its text, its layout --- and on r2
//  it is the kernel heap, which is far larger and which these buffers can use
//  because none of them is ever handed to a syscall.
//
void *alloc(size_t n);
void *realloc(void *p, size_t n);
void free(void *p);

void *big_alloc(size_t n);
void *big_realloc(void *p, size_t n);
void big_free(void *p);

//  Milliseconds since some fixed point; only differences mean anything.
uint64_t now_ms();

//
//  A growable byte buffer.  Every append can fail --- the machine has very
//  little memory --- and a failure is sticky, so a caller can do a run of
//  appends and look once at the end.
//
struct Buf
{
    uint8_t *data = nullptr;
    size_t len = 0;
    size_t cap = 0;
    bool big = false;
    bool failed = false;

    Buf() {}
    explicit Buf(bool bigPool) : big(bigPool) {}
    ~Buf() { release(); }
    Buf(const Buf &) = delete;
    Buf &operator=(const Buf &) = delete;

    bool reserve(size_t want);
    bool append(const void *src, size_t n);
    bool push(uint8_t b) { return append(&b, 1); }
    bool appendStr(const char *s) { return append(s, strlen(s)); }
    void clear()
    {
        len = 0;
        failed = false;
    }
    void release();

    //  The contents as a C string.  Keeps the terminator outside len.
    const char *cstr();
};

//
//  Small string helpers.  The engine keeps strings in fixed arrays wherever it
//  can, and these are what makes that bearable.
//
void scopy(char *dst, const char *src, size_t cap);
void scopyn(char *dst, const char *src, size_t n, size_t cap);
void scat(char *dst, const char *src, size_t cap);
void scatInt(char *dst, long v, size_t cap);
char lower(char c);
bool ieq(const char *a, const char *b);
bool ieqn(const char *a, const char *b, size_t n);
bool istarts(const char *s, const char *prefix);
const char *ifind(const char *hay, size_t hayLen, const char *needle);
bool isSpace(char c);

} // namespace web
