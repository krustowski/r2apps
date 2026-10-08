#ifndef _R2CXX_HEAP_HPP_
#define _R2CXX_HEAP_HPP_

/*
 *  heap.hpp — where dynamic memory comes from.
 *
 *  An arena: a first-fit free list with boundary tags, so memory really is
 *  reclaimed and coalesced --- a long-running program that allocates and frees
 *  in a loop stays flat instead of marching off the end of a bump pointer.
 *
 *  Where the arena is, is the program's choice:
 *
 *  - In its own .bss (the default, or R2_HEAP_ARENA).  It then comes out of
 *    the 2 MiB private frame the kernel maps at 0x600_000, alongside the code
 *    and the stack from _crt0.asm; see README.md, "Memory map".
 *
 *  - On the kernel's user heap (R2_HEAP_ARENA_KERNEL), 0xC00_000-0xFFF_FFF,
 *    which syscall 0x0a hands out.  The image then carries no arena at all,
 *    and the whole 2 MiB is left for code, data and stack.  The user heap is
 *    4 MiB for every process together, so a program should take what it needs
 *    and not more; the kernel gives a dead process's blocks back.
 *
 *  Either way the memory can be handed to the kernel: every pointer-taking
 *  syscall accepts a buffer that lies wholly inside the image (0x600_000 to
 *  0xA00_000) or wholly inside the user heap.  (Kernels before that check
 *  refused everything outside the image, and a program with its arena on the
 *  user heap cannot run on them.)
 *
 *  Sizing: the default arena is R2CXX_ARENA_BYTES (512 KiB).
 *
 *      R2_HEAP_ARENA(1024 * 1024)          // in .bss, in exactly one .cpp
 *      R2_HEAP_ARENA_KERNEL(1024 * 1024)   // on the user heap, likewise
 */

#include "types.hpp"
#include "syscall.hpp"

namespace r2::heap {

struct Stats {
    size_t arena_bytes;        /*  total arena size, including block headers  */
    size_t regions;            /*  pieces it is in: 1, more once it has grown  */
    size_t used_bytes;         /*  payload bytes currently handed out         */
    size_t free_bytes;         /*  payload bytes available                    */
    size_t largest_free_block; /*  biggest single allocation that can succeed  */
    size_t live_allocations;
    size_t total_allocations;
    size_t failed_allocations;
};

/*  Called by __r2_start before any global constructor runs.  Calling it again
 *  is a no-op.  */
void init() noexcept;

void *allocate(size_t size) noexcept;
void *allocate_aligned(size_t size, size_t align) noexcept;
void *reallocate(void *ptr, size_t new_size) noexcept;
void deallocate(void *ptr) noexcept;
void deallocate_aligned(void *ptr) noexcept;

/*  Payload size of a block previously returned by allocate().  */
size_t block_size(const void *ptr) noexcept;

Stats stats() noexcept;

/*
 *  Walks the block chain and the free list and reports the first thing that
 *  does not add up: a boundary tag that disagrees with its neighbour, a block
 *  that runs past the arena, a free-list entry that is not marked free.
 *
 *  Nothing calls this on its own --- it is for a program that suspects it is
 *  overrunning an allocation.  In this allocator that damage shows up twice
 *  over: the overrun rewrites the next block's header, and the first time that
 *  block is merged the live object beyond it is linked into the free list and
 *  its first two words are overwritten with the list pointers.  A null vtable
 *  pointer on an object that was fine a moment ago is the usual symptom.
 *  Call this after each suspect step; the last step that passes brackets it.
 *
 *  `where` is filled in with the address of the offending block when one is
 *  found; pass nullptr if the address is not wanted.
 */
enum class Integrity {
    Ok = 0,
    NotReady,        /*  the arena was never initialised  */
    BadPrevSize,     /*  block->prev_size disagrees with the block before it  */
    BlockOutOfRange, /*  a block header lies outside the arena  */
    NoSentinel,      /*  the chain does not end where the arena does  */
    FreeNotMarked,   /*  a free-list entry is not flagged free  */
    FreeOutOfRange,  /*  a free-list entry is not a block in this arena  */
    FreeListLoop,    /*  the free list does not terminate  */
};

Integrity validate(const void **where = nullptr) noexcept;

/*
 *  The kernel's user heap (syscall 0x0a), 0xC00_000-0xFFF_FFF.  Blocks come
 *  back zeroed, can be far larger than the arena, and may be passed to
 *  syscalls like any other buffer.  The same addresses mean the same memory in
 *  every process, which is also what makes a block shareable between two of
 *  them.  The kernel frees a process's blocks when it dies.
 */
void *kernel_allocate(size_t size) noexcept;
void *kernel_reallocate(void *ptr, size_t size) noexcept;
void kernel_deallocate(void *ptr) noexcept;

/* Checks a whole nonempty range in the shared heap, including its extension,
 * without dereferencing it. Does not check allocation ownership/lifetime.
 * Older kernels can verify only the original fixed region. */
inline bool kernel_contains(const void *ptr, size_t size) noexcept {
    const uintptr_t addr = (uintptr_t)ptr;
    const int64_t result = raw_syscall(Sys::HeapContains, (int64_t)addr, (int64_t)size);
    if (result == 0xff)
        return size && addr >= 0xc00000 && addr < 0x1000000 && size <= 0x1000000 - addr;
    return result == 1;
}

} // namespace r2::heap

/*
 *  Override the default arena.  Use at file scope in exactly one translation
 *  unit of the application.
 */
#define R2_HEAP_ARENA(bytes)                                                                       \
    alignas(16) static unsigned char __r2_user_arena[(bytes)];                                     \
    extern "C" void __r2_heap_config(void **base, size_t *size) {                                  \
        *base = __r2_user_arena;                                                                   \
        *size = (bytes);                                                                           \
    }

/*
 *  The arena from the kernel's user heap instead of the image: nothing in
 *  .bss.  It starts at `bytes` (when the heap cannot give that much, half is
 *  tried, and so on down to 64 KiB) and grows: when nothing in it fits, another
 *  region of at least R2_HEAP_GROW_BYTES comes from the user heap.  Allocation
 *  fails only when the user heap itself is out.
 */
#ifndef R2_HEAP_GROW_BYTES
#define R2_HEAP_GROW_BYTES (256 * 1024)
#endif
#define R2_HEAP_MORE_FROM_KERNEL()                                                                 \
    extern "C" bool __r2_heap_more(size_t need, void **base, size_t *size) {                       \
        size_t want = need > R2_HEAP_GROW_BYTES ? need : R2_HEAP_GROW_BYTES;                      \
        void *p = r2::heap::kernel_allocate(want);                                                 \
        if (!p && want > need)                                                                     \
            p = r2::heap::kernel_allocate(want = need);                                            \
        *base = p;                                                                                 \
        *size = p ? want : 0;                                                                      \
        return p != nullptr;                                                                       \
    }
#define R2_HEAP_ARENA_KERNEL(bytes)                                                                \
    extern "C" void __r2_heap_config(void **base, size_t *size) {                                  \
        size_t want = (bytes);                                                                     \
        void *p = nullptr;                                                                         \
        while (want >= 64 * 1024 && !(p = r2::heap::kernel_allocate(want)))                        \
            want /= 2;                                                                             \
        *base = p;                                                                                 \
        *size = p ? want : 0;                                                                      \
    }                                                                                              \
    R2_HEAP_MORE_FROM_KERNEL()

/*
 *  The arena in .bss, like R2_HEAP_ARENA, and growing onto the kernel's user
 *  heap, like R2_HEAP_ARENA_KERNEL, once it is full.  For a program that
 *  usually fits in what its image has room for but must not fail when it
 *  does not --- and that should leave the shared user heap to others until
 *  then.
 */
#define R2_HEAP_ARENA_GROWING(bytes)                                                               \
    R2_HEAP_ARENA(bytes)                                                                           \
    R2_HEAP_MORE_FROM_KERNEL()

/*
 *  Where the heap lives.  R2_HEAP_ARENA defines it when an application wants
 *  its own arena; when none does, the definition comes from the library's own
 *  heap_arena.cpp.  The reference is deliberately not weak — a weak one would
 *  not make the linker search the archive for that fallback, and an
 *  application that does supply an arena would still be paying for it.
 */
extern "C" void __r2_heap_config(void **base, size_t *size);

/*
 *  Another region for the arena, at least `need` bytes, when nothing in it
 *  fits.  Weak: an application without one has an arena of fixed size.
 */
extern "C" bool __r2_heap_more(size_t need, void **base, size_t *size) __attribute__((weak));

#endif
