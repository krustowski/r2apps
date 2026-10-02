/*
 *  heap_arena.cpp — the default arena, and nothing else.
 *
 *  A translation unit of its own on purpose.  The linker pulls an archive
 *  member in only to resolve a symbol that is still undefined, so a program
 *  that supplies its own arena with R2_HEAP_ARENA has already defined
 *  __r2_heap_config by the time the archive is searched, and this member —
 *  half a megabyte of .bss — never comes in.  Next to the allocator in
 *  heap.cpp the array would be linked either way, and in a process with 2 MiB
 *  for everything that is the difference between fitting and not.
 *
 *  R2CXX_ARENA_BYTES sets the size; see the Makefile and README.md, "Memory
 *  map".
 */

#include "r2/heap.hpp"

#ifndef R2CXX_ARENA_BYTES
#define R2CXX_ARENA_BYTES (512 * 1024)
#endif

namespace {

alignas(16) unsigned char g_default_arena[R2CXX_ARENA_BYTES];

} // namespace

extern "C" void __r2_heap_config(void **base, size_t *size) {
    *base = g_default_arena;
    *size = sizeof(g_default_arena);
}
