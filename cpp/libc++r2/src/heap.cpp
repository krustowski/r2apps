/*
 *  heap.cpp — the arena allocator.
 *
 *  A first-fit free list with boundary tags.  Each block carries its own
 *  payload size and the size of the block physically before it, so a freed
 *  block can find both neighbours in constant time and merge with whichever of
 *  them is also free.  Free blocks keep their list links inside their own
 *  payload, which is why the minimum payload is 16 bytes.
 *
 *      +----------------+------------------+----------------+-----
 *      | size | prev_sz |     payload      | size | prev_sz |  ...
 *      +----------------+------------------+----------------+-----
 *      \--- 16 bytes ---/                  \--- 16 bytes ---/
 *
 *  Bit 0 of `size` is the free flag; payload sizes are always multiples of 16,
 *  so the bit is free to borrow.  The arena ends with a zero-sized used block
 *  that coalescing stops at, which removes the end-of-arena special case from
 *  every path.
 *
 *  Single-threaded by construction: each r2 process has one thread and its own
 *  arena in its own private frame, so there is nothing to lock against.
 */

#include "r2/heap.hpp"
#include "r2/libc.hpp"
#include "r2/syscall.hpp"

namespace {

constexpr size_t ALIGNMENT = 16;
constexpr size_t HEADER_SIZE = 16;
constexpr size_t MIN_PAYLOAD = 16; /*  room for the free-list links  */
constexpr size_t FREE_FLAG = 1;

struct Block {
    size_t size;      /*  payload bytes, with FREE_FLAG in bit 0  */
    size_t prev_size; /*  payload bytes of the block before this one, 0 if first  */
};

struct FreeLinks {
    Block *next;
    Block *prev;
};

inline size_t payload_size(const Block *b) { return b->size & ~FREE_FLAG; }
inline bool is_free(const Block *b) { return (b->size & FREE_FLAG) != 0; }
inline void set_free(Block *b, bool free_flag) {
    b->size = payload_size(b) | (free_flag ? FREE_FLAG : 0);
}

inline void *payload_of(Block *b) { return (unsigned char *)b + HEADER_SIZE; }
inline Block *block_of(void *p) { return (Block *)((unsigned char *)p - HEADER_SIZE); }
inline FreeLinks *links_of(Block *b) { return (FreeLinks *)payload_of(b); }

inline Block *next_block(Block *b) {
    return (Block *)((unsigned char *)b + HEADER_SIZE + payload_size(b));
}

inline Block *prev_block(Block *b) {
    if (b->prev_size == 0)
        return nullptr;
    return (Block *)((unsigned char *)b - HEADER_SIZE - b->prev_size);
}

inline size_t round_up(size_t value, size_t multiple) {
    return (value + multiple - 1) & ~(multiple - 1);
}

/*  The default arena.  R2CXX_ARENA_BYTES is settable at build time; see the
 *  Makefile and README.md, "Memory map".  */
#ifndef R2CXX_ARENA_BYTES
#define R2CXX_ARENA_BYTES (512 * 1024)
#endif

alignas(ALIGNMENT) unsigned char g_default_arena[R2CXX_ARENA_BYTES];

Block *g_free_list = nullptr;
unsigned char *g_arena_base = nullptr;
size_t g_arena_size = 0;
bool g_ready = false;

r2::heap::Stats g_stats = {};

void free_list_insert(Block *b) {
    FreeLinks *links = links_of(b);
    links->prev = nullptr;
    links->next = g_free_list;
    if (g_free_list)
        links_of(g_free_list)->prev = b;
    g_free_list = b;
}

void free_list_remove(Block *b) {
    FreeLinks *links = links_of(b);
    if (links->prev)
        links_of(links->prev)->next = links->next;
    else
        g_free_list = links->next;
    if (links->next)
        links_of(links->next)->prev = links->prev;
}

void arena_init(unsigned char *base, size_t size) {
    /*  Align the base up and the size down, then reserve the tail sentinel.  */
    uintptr_t aligned_base = round_up((uintptr_t)base, ALIGNMENT);
    size -= (size_t)(aligned_base - (uintptr_t)base);
    size &= ~(ALIGNMENT - 1);

    if (size < HEADER_SIZE * 2 + MIN_PAYLOAD)
        return;

    g_arena_base = (unsigned char *)aligned_base;
    g_arena_size = size;

    Block *first = (Block *)g_arena_base;
    size_t usable = size - HEADER_SIZE /*  first header  */ - HEADER_SIZE /*  sentinel  */;
    usable &= ~(ALIGNMENT - 1);

    first->size = usable | FREE_FLAG;
    first->prev_size = 0;

    Block *sentinel = next_block(first);
    sentinel->size = 0; /*  zero-sized and not free: coalescing stops here  */
    sentinel->prev_size = usable;

    g_free_list = nullptr;
    free_list_insert(first);

    g_stats = {};
    g_stats.arena_bytes = size;
    g_ready = true;
}

void ensure_ready() {
    if (!g_ready)
        r2::heap::init();
}

/*  Merges `b` with the block after it when that one is free.  The sentinel is
 *  never free, so this always terminates.  */
void coalesce_forward(Block *b) {
    Block *after = next_block(b);
    if (!is_free(after))
        return;

    free_list_remove(after);
    b->size = (payload_size(b) + HEADER_SIZE + payload_size(after)) | (b->size & FREE_FLAG);
    next_block(b)->prev_size = payload_size(b);
}

/*  Splits `b` so that it holds exactly `wanted` payload bytes, if the leftover
 *  is big enough to be a block of its own.  */
void split_block(Block *b, size_t wanted) {
    size_t have = payload_size(b);
    if (have < wanted + HEADER_SIZE + MIN_PAYLOAD)
        return;

    size_t leftover = have - wanted - HEADER_SIZE;

    b->size = wanted | (is_free(b) ? FREE_FLAG : 0);

    Block *rest = next_block(b);
    rest->size = leftover | FREE_FLAG;
    rest->prev_size = wanted;

    Block *after = next_block(rest);
    after->prev_size = leftover;

    free_list_insert(rest);

    /*  The leftover may now sit against a block that is already free --- when
     *  a realloc shrinks a block, for one.  Merge them, or the two would stay
     *  apart until something allocated and freed the first one again.  */
    coalesce_forward(rest);
}

} // namespace

namespace r2::heap {

void init() noexcept {
    if (g_ready)
        return;

    unsigned char *base = g_default_arena;
    size_t size = sizeof(g_default_arena);

    /*  An application can supply its own arena with R2_HEAP_ARENA().  */
    if (__r2_heap_config) {
        void *custom_base = nullptr;
        size_t custom_size = 0;
        __r2_heap_config(&custom_base, &custom_size);
        if (custom_base && custom_size >= HEADER_SIZE * 2 + MIN_PAYLOAD) {
            base = (unsigned char *)custom_base;
            size = custom_size;
        }
    }

    arena_init(base, size);
}

void *allocate(size_t size) noexcept {
    ensure_ready();

    if (size == 0)
        size = 1;

    size_t wanted = round_up(size, ALIGNMENT);
    if (wanted < MIN_PAYLOAD)
        wanted = MIN_PAYLOAD;

    /*  Overflow guard: a size that wraps must not look like a small request. */
    if (wanted < size) {
        g_stats.failed_allocations++;
        return nullptr;
    }

    for (Block *b = g_free_list; b != nullptr; b = links_of(b)->next) {
        if (payload_size(b) < wanted)
            continue;

        free_list_remove(b);
        split_block(b, wanted);
        set_free(b, false);

        g_stats.used_bytes += payload_size(b);
        g_stats.live_allocations++;
        g_stats.total_allocations++;
        return payload_of(b);
    }

    g_stats.failed_allocations++;
    return nullptr;
}

void *allocate_aligned(size_t size, size_t align) noexcept {
    if (align <= ALIGNMENT)
        return allocate(size);

    /*  Over-aligned: allocate slack, then stash the real block pointer in the
     *  word before the aligned address.  Only deallocate_aligned() may free
     *  the result --- which the C++ ABI guarantees, because an over-aligned
     *  type is always deleted through the aligned operator delete.  */
    size_t total = size + align + sizeof(void *) * 2;
    if (total < size)
        return nullptr;

    void *raw = allocate(total);
    if (!raw)
        return nullptr;

    uintptr_t start = (uintptr_t)raw + sizeof(void *) * 2;
    uintptr_t aligned = (start + align - 1) & ~(uintptr_t)(align - 1);
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

void deallocate(void *ptr) noexcept {
    if (!ptr)
        return;

    Block *b = block_of(ptr);
    if (is_free(b))
        return; /*  double free: ignore rather than corrupt the list  */

    g_stats.used_bytes -= payload_size(b);
    g_stats.live_allocations--;

    set_free(b, true);

    coalesce_forward(b);

    /*  Merge backwards into a block that is already on the free list, so it
     *  stays there and only its size changes.  */
    Block *before = prev_block(b);
    if (before && is_free(before)) {
        before->size = (payload_size(before) + HEADER_SIZE + payload_size(b)) | FREE_FLAG;
        next_block(before)->prev_size = payload_size(before);
    } else {
        free_list_insert(b);
    }
}

void deallocate_aligned(void *ptr) noexcept {
    if (!ptr)
        return;
    deallocate(((void **)ptr)[-1]);
}

void *reallocate(void *ptr, size_t new_size) noexcept {
    if (!ptr)
        return allocate(new_size);

    if (new_size == 0) {
        deallocate(ptr);
        return nullptr;
    }

    ensure_ready();

    Block *b = block_of(ptr);
    size_t have = payload_size(b);
    size_t wanted = round_up(new_size, ALIGNMENT);
    if (wanted < MIN_PAYLOAD)
        wanted = MIN_PAYLOAD;

    if (wanted <= have) {
        size_t before = payload_size(b);
        split_block(b, wanted);
        g_stats.used_bytes -= before - payload_size(b);
        return ptr;
    }

    /*  Grow in place by swallowing the next block when it is free and big
     *  enough --- the common case for a vector that keeps appending.  */
    Block *after = next_block(b);
    if (is_free(after) && have + HEADER_SIZE + payload_size(after) >= wanted) {
        free_list_remove(after);
        b->size = (have + HEADER_SIZE + payload_size(after)) | 0;
        next_block(b)->prev_size = payload_size(b);
        g_stats.used_bytes += payload_size(b) - have;

        size_t merged = payload_size(b);
        split_block(b, wanted);
        g_stats.used_bytes -= merged - payload_size(b);
        return ptr;
    }

    void *fresh = allocate(new_size);
    if (!fresh)
        return nullptr;

    memcpy(fresh, ptr, have < new_size ? have : new_size);
    deallocate(ptr);
    return fresh;
}

size_t block_size(const void *ptr) noexcept {
    if (!ptr)
        return 0;
    return payload_size(block_of(const_cast<void *>(ptr)));
}

Stats stats() noexcept {
    ensure_ready();

    /*  free_bytes and the largest block are read off the free list rather than
     *  tracked on every split and merge: one walk at diagnostic time is far
     *  easier to keep correct than a delta on each of half a dozen paths.  */
    Stats snapshot = g_stats;
    snapshot.free_bytes = 0;
    snapshot.largest_free_block = 0;
    for (Block *b = g_free_list; b != nullptr; b = links_of(b)->next) {
        size_t size = payload_size(b);
        snapshot.free_bytes += size;
        if (size > snapshot.largest_free_block)
            snapshot.largest_free_block = size;
    }
    return snapshot;
}

void *kernel_allocate(size_t size) noexcept {
    return (void *)raw_syscall(Sys::Malloc, (int64_t)size, 0);
}

void *kernel_reallocate(void *ptr, size_t size) noexcept {
    return (void *)raw_syscall(Sys::Realloc, (int64_t)ptr, (int64_t)size);
}

void kernel_deallocate(void *ptr) noexcept {
    if (ptr)
        raw_syscall(Sys::Free, (int64_t)ptr, 0);
}

} // namespace r2::heap
