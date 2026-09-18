#ifndef _R2CXX_HEAP_HPP_
#define _R2CXX_HEAP_HPP_

/*
 *  heap.hpp — where dynamic memory comes from.
 *
 *  Not from the kernel.  Syscall 0x0a hands out blocks in 0xC00_000-0xFFF_FFF,
 *  but every pointer-taking syscall range-checks its arguments against
 *  0x600_000-0xA00_000 and rejects anything outside it.  A string built on the
 *  kernel heap therefore cannot be printed, written to a file, or blitted: the
 *  allocation succeeds and the call that uses it fails.
 *
 *  So the default heap is an arena in the program's own .bss, which is inside
 *  the image and inside the range the kernel accepts.  It is a first-fit free
 *  list with boundary tags, so memory really is reclaimed and coalesced --- a
 *  long-running program that allocates and frees in a loop stays flat instead
 *  of marching off the end of a bump pointer.
 *
 *  The kernel heap is still reachable, through kernel_allocate(), for large
 *  buffers that never cross the ABI boundary.
 *
 *  Sizing: the arena defaults to R2CXX_ARENA_BYTES (512 KiB) and lives in .bss
 *  alongside the stack from _crt0.asm (another 512 KiB by default).  Both come
 *  out of the 2 MiB private frame the kernel maps at 0x600_000; see README.md,
 *  "Memory map", before making either much larger.
 *
 *  An application that wants a different arena defines its own:
 *
 *      R2_HEAP_ARENA(1024 * 1024)   // at file scope, in exactly one .cpp
 */

#include "types.hpp"

namespace r2::heap {

struct Stats {
    size_t arena_bytes;        /*  total arena size, including block headers  */
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
 *  The kernel's own heap (syscall 0x0a).  Blocks come back zeroed and can be
 *  far larger than the arena, but they live at 0xC00_000 and so CANNOT be
 *  passed to any syscall.  Use for scratch space, not for anything the kernel
 *  will read.
 */
void *kernel_allocate(size_t size) noexcept;
void *kernel_reallocate(void *ptr, size_t size) noexcept;
void kernel_deallocate(void *ptr) noexcept;

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

/*  Defined by R2_HEAP_ARENA when an application supplies its own arena.  */
extern "C" void __r2_heap_config(void **base, size_t *size) __attribute__((weak));

#endif
