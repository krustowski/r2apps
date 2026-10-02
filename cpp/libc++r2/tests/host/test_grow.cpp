/*
 *  test_grow.cpp — an arena that grows through __r2_heap_more.
 *
 *  The first region is 64 KiB; further regions come from a pool of 512 KiB
 *  standing in for the kernel's user heap.  Filling past the first region must
 *  add regions, a block larger than the growth step must still get one of its
 *  own, everything must validate, and freeing must give every byte back.  When
 *  the pool is dry, allocation fails the ordinary way.
 */

#include "r2/heap.hpp"

extern "C" int printf(const char *format, ...);

alignas(16) static unsigned char g_first[64 * 1024];
alignas(16) static unsigned char g_pool[512 * 1024];
static size_t g_pool_used = 0;
static int g_more_calls = 0;

extern "C" void __r2_heap_config(void **base, size_t *size) {
    *base = g_first;
    *size = sizeof(g_first);
}

extern "C" bool __r2_heap_more(size_t need, void **base, size_t *size) {
    g_more_calls++;
    size_t want = need > 96 * 1024 ? need : 96 * 1024;
    want = (want + 15) & ~(size_t)15;
    if (g_pool_used + want > sizeof(g_pool))
        return false;
    *base = g_pool + g_pool_used;
    *size = want;
    g_pool_used += want;
    return true;
}

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    r2::heap::Stats start = r2::heap::stats();
    check(start.regions == 1, "one region to start with");

    /*  Past the first region, 1 KiB at a time.  */
    static void *blocks[1024];
    int n = 0;
    for (; n < 150; n++) {
        blocks[n] = r2::heap::allocate(1000);
        if (!blocks[n])
            break;
    }
    check(n == 150, "150 KiB of small blocks, more than the first region holds");

    /*  Bigger than the growth step: needs a region of its own size.  */
    void *big = r2::heap::allocate(200 * 1024);
    check(big != nullptr, "a 200 KiB block beyond the 96 KiB step");

    r2::heap::Stats grown = r2::heap::stats();
    printf("grew to %lu regions, %lu bytes, %d requests\n", (unsigned long)grown.regions,
           (unsigned long)grown.arena_bytes, g_more_calls);
    check(grown.regions >= 3, "at least two regions added");
    check(r2::heap::validate() == r2::heap::Integrity::Ok, "validates after growing");

    /*  No block may straddle two regions.  */
    for (int i = 0; i < n; i++) {
        unsigned char *p = (unsigned char *)blocks[i];
        bool inFirst = p >= g_first && p + 1000 <= g_first + sizeof(g_first);
        bool inPool = p >= g_pool && p + 1000 <= g_pool + sizeof(g_pool);
        check(inFirst || inPool, "a block inside one region");
    }

    /*  Dry: the pool cannot give another 400 KiB.  */
    size_t failed = grown.failed_allocations;
    check(r2::heap::allocate(400 * 1024) == nullptr, "refused when the pool is dry");
    check(r2::heap::stats().failed_allocations == failed + 1, "counted as a failure");

    for (int i = 0; i < n; i++)
        r2::heap::deallocate(blocks[i]);
    r2::heap::deallocate(big);

    r2::heap::Stats after = r2::heap::stats();
    check(after.live_allocations == 0, "nothing live after freeing");
    check(after.regions == grown.regions, "regions are kept");
    /*  Each region is one free block again: its size less a header and a
     *  sentinel.  */
    check(after.free_bytes == after.arena_bytes - after.regions * 32, "every byte back");
    check(r2::heap::validate() == r2::heap::Integrity::Ok, "validates after freeing");

    /*  And the space is reused rather than grown into again.  */
    int calls = g_more_calls;
    void *again = r2::heap::allocate(150 * 1024);
    check(again != nullptr && g_more_calls == calls, "reuses a freed region");

    printf("%s\n", failures == 0 ? "arena growth OK" : "arena growth FAILED");
    return failures == 0 ? 0 : 1;
}
