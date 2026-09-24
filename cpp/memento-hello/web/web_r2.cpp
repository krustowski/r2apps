//
//  web_r2.cpp --- what the web engine needs from r2: memory, a clock, entropy
//  and the date.
//

#include "wbase.h"
#include "tls.h"

#include <r2/fs.hpp>
#include <r2/heap.hpp>
#include <r2/syscall.hpp>
#include <r2/time.hpp>

namespace web {

void *alloc(size_t n) { return r2::heap::allocate(n); }
void *realloc(void *p, size_t n) { return r2::heap::reallocate(p, n); }
void free(void *p) { r2::heap::deallocate(p); }

//
//  The big pool.  The kernel heap is several times the size of the arena and
//  these buffers --- the page as it came, its text, its layout, the TLS
//  record buffer --- never go through a syscall, which is the one thing a
//  kernel-heap block cannot do.  Blocks are tagged so that a block from either
//  pool finds its way back to the right one; build with -DWEB_BIG_ARENA to
//  keep everything in the arena.
//
namespace {
const uint64_t TAG_KERNEL = 0x4b524e4c4b524e4cull; // "KRNLKRNL"
const uint64_t TAG_ARENA = 0x4152454e4152454eull;  // "ARENAREN"
const size_t HDR = 16;                              // keeps the payload 16-aligned

struct BigHdr
{
    uint64_t tag;
    uint64_t size;
};
} // namespace

void *big_alloc(size_t n)
{
    uint8_t *p = nullptr;
    uint64_t tag = TAG_KERNEL;
#ifndef WEB_BIG_ARENA
    p = (uint8_t *)r2::heap::kernel_allocate(n + HDR);
#endif
    if (!p)
    {
        p = (uint8_t *)r2::heap::allocate(n + HDR);
        tag = TAG_ARENA;
    }
    if (!p)
        return nullptr;
    BigHdr *h = (BigHdr *)p;
    h->tag = tag;
    h->size = n;
    return p + HDR;
}

void big_free(void *q)
{
    if (!q)
        return;
    BigHdr *h = (BigHdr *)((uint8_t *)q - HDR);
    if (h->tag == TAG_KERNEL)
        r2::heap::kernel_deallocate(h);
    else
        r2::heap::deallocate(h);
}

void *big_realloc(void *q, size_t n)
{
    if (!q)
        return big_alloc(n);
    BigHdr *h = (BigHdr *)((uint8_t *)q - HDR);
    //  A copy rather than the pool's own realloc, so that a block can move
    //  from a full kernel heap to the arena and the tag stays true.
    void *fresh = big_alloc(n);
    if (!fresh)
        return nullptr;
    memcpy(fresh, q, h->size < n ? h->size : n);
    big_free(q);
    return fresh;
}

uint64_t now_ms() { return r2::ticks(); }

// ─── Entropy ─────────────────────────────────────────────────────────────────

namespace {

inline uint64_t rdtsc()
{
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

bool haveRdrand()
{
    uint32_t a = 1, b, c, d;
    asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d) : "c"(0));
    return (c >> 30) & 1;
}

bool rdrand64(uint64_t &out)
{
    for (int tries = 0; tries < 10; tries++)
    {
        unsigned char ok;
        asm volatile("rdrand %0; setc %1" : "=r"(out), "=qm"(ok));
        if (ok)
            return true;
    }
    return false;
}

} // namespace

//
//  Seeds the TLS engine.  BearSSL runs whatever it is given through HMAC-DRBG,
//  so this gathers raw material rather than finished randomness, and plenty of
//  it:
//
//  - RDRAND, where the CPU has it.  QEMU's default CPU model does not; with
//    "-cpu host" (and KVM) it usually does.
//  - The jitter of the timestamp counter across syscalls and short sleeps.
//    Each sample carries only a few unpredictable bits, which is why there
//    are hundreds of them.
//  - The clock, the tick count and a stack address, which are guessable but
//    cost nothing.
//
//  Without RDRAND this is the weakest part of the browser's TLS: good enough
//  to keep a passive observer out, not something to trust against someone who
//  can model the machine's timing.
//
void gatherEntropy(uint8_t *out, size_t n)
{
    size_t k = 0;
    auto put = [&](uint64_t v) {
        for (int b = 0; b < 8 && k < n; b++)
            out[k++] = (uint8_t)(v >> (8 * b));
    };

    if (haveRdrand())
    {
        uint64_t v;
        for (int i = 0; i < 8 && rdrand64(v); i++)
            put(v);
    }

    r2::RtcTime rtc;
    memset(&rtc, 0, sizeof(rtc));
    r2::raw_syscall(r2::Sys::Rtc, 0x01, (int64_t)&rtc);
    put(((uint64_t)rtc.year << 40) | ((uint64_t)rtc.month << 32) | ((uint64_t)rtc.day << 24) |
        ((uint64_t)rtc.hours << 16) | ((uint64_t)rtc.minutes << 8) | rtc.seconds);
    put(r2::ticks());
    put((uint64_t)(uintptr_t)&k);

    //  The rest is timing jitter: two bytes per sample, the low ones.
    uint64_t prev = rdtsc();
    for (int i = 0; k + 2 <= n; i++)
    {
        if ((i & 63) == 63)
            r2::raw_syscall(r2::Sys::Sleep, 1);
        else
            r2::raw_syscall(r2::Sys::GetTicks);
        uint64_t t = rdtsc();
        uint64_t d = t - prev;
        prev = t;
        out[k++] = (uint8_t)d;
        out[k++] = (uint8_t)(d >> 8) ^ (uint8_t)(t >> 3);
    }
}

//
//  The date, as BearSSL counts it: days since 1 January of year 0, and
//  seconds into the day.  The RTC is taken to run on UTC, which is QEMU's
//  default.  A clock that has clearly never been set says "unknown", and
//  certificates are then refused with a message that says why.
//
void currentTime(unsigned long *days, unsigned long *seconds)
{
    r2::RtcTime t;
    memset(&t, 0, sizeof(t));
    *days = *seconds = 0;
    if (r2::raw_syscall(r2::Sys::Rtc, 0x01, (int64_t)&t) != 0)
        return;
    int y = t.year;
    if (y < 100)
        y += 2000;
    if (y < 2024 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31)
        return;

    //  Days from 1970-01-01 (Howard Hinnant's days_from_civil).
    int m = t.month, d = t.day;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long unixDays = era * 146097 + (long)doe - 719468;

    *days = (unsigned long)(unixDays + 719528);
    *seconds = (unsigned long)t.hours * 3600 + (unsigned long)t.minutes * 60 + t.seconds;
}

} // namespace web

//
//  The trust anchors live on the CD (tools/mkcacerts.sh puts them there), read
//  once, on the first handshake, and kept: BearSSL's anchors point into them.
//  The kernel reads a file into memory the process owns, so it comes in
//  through a buffer on the stack a sector at a time and is gathered in the
//  big pool.
//
static const char ANCHOR_PATH[] = "/mnt/iso/opt/memento/cacerts.bin";
static const size_t ANCHOR_MAX = 256 * 1024;

extern "C" const char *web_tls_platform_anchor_path(void) { return ANCHOR_PATH; }

extern "C" int web_tls_platform_anchors(const unsigned char **data, unsigned long *len)
{
    web::Buf file(true);
    uint8_t chunk[512];
    for (;;)
    {
        int64_t got = r2::fs::read_at(ANCHOR_PATH, r2::byte_span(chunk, sizeof(chunk)), file.len);
        if (got < 0)
            return -1;
        if (got > 0 && !file.append(chunk, (size_t)got))
            return -1;
        if ((size_t)got < sizeof(chunk) || file.len > ANCHOR_MAX)
            break;
    }
    if (!file.len)
        return -1;
    //  Handed over for good: the Buf lets go of it without freeing it.
    *data = file.data;
    *len = (unsigned long)file.len;
    file.data = nullptr;
    file.len = file.cap = 0;
    return 0;
}

extern "C" void *web_tls_alloc(unsigned long n) { return web::big_alloc(n); }
extern "C" void web_tls_release(void *p) { web::big_free(p); }
