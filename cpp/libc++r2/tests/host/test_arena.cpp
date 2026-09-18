/*
 *  test_arena.cpp — checks that an application can replace the default arena.
 *
 *  R2_HEAP_ARENA defines the weak __r2_heap_config that heap::init() looks for,
 *  so the allocator must end up using this 64 KiB buffer rather than the
 *  512 KiB (or whatever R2CXX_ARENA_BYTES says) one compiled into heap.cpp.
 *  A separate binary from the main host test because there can only be one
 *  arena per program.
 */

#include "r2/heap.hpp"
#include "r2/vector.hpp"

extern "C" int printf(const char *format, ...);

R2_HEAP_ARENA(64 * 1024)

int main() {
    int failures = 0;

    r2::heap::Stats stats = r2::heap::stats();
    printf("arena is %lu bytes\n", (unsigned long)stats.arena_bytes);

    if (stats.arena_bytes > 64 * 1024 || stats.arena_bytes < 64 * 1024 - 64) {
        printf("  FAIL: expected the 64 KiB arena from R2_HEAP_ARENA\n");
        failures++;
    }

    /*  Allocate until the arena is exhausted, then give it all back and check
     *  the free space returns --- a leak in the failure path would show here. */
    size_t free_before = stats.free_bytes;

    r2::vector<void *> blocks;
    for (;;) {
        void *block = r2::heap::allocate(1024);
        if (!block)
            break;
        if (!blocks.push_back(block)) {
            r2::heap::deallocate(block);
            break;
        }
    }

    printf("filled with %lu blocks of 1 KiB\n", (unsigned long)blocks.size());
    if (blocks.size() < 32) {
        printf("  FAIL: a 64 KiB arena should hold more than that\n");
        failures++;
    }

    for (void *block : blocks)
        r2::heap::deallocate(block);
    blocks = r2::vector<void *>();

    r2::heap::Stats after = r2::heap::stats();
    if (after.free_bytes != free_before) {
        printf("  FAIL: %lu bytes free at the start, %lu after\n", (unsigned long)free_before,
               (unsigned long)after.free_bytes);
        failures++;
    }
    if (after.live_allocations != 0) {
        printf("  FAIL: %lu allocations still live\n", (unsigned long)after.live_allocations);
        failures++;
    }
    if (after.failed_allocations == 0) {
        printf("  FAIL: filling the arena should have refused at least one\n");
        failures++;
    }

    printf("%s\n", failures == 0 ? "arena override OK" : "arena override FAILED");
    return failures == 0 ? 0 : 1;
}
