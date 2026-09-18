/*
 *  new.cpp — operator new and operator delete, on top of r2::heap.
 *
 *  Allocation failure returns nullptr, including from the forms that would
 *  normally throw std::bad_alloc: this library is built -fno-exceptions, and a
 *  null return that every container already checks is more useful than a
 *  terminate() the program cannot handle.
 */

#include "r2/heap.hpp"
#include "r2/new.hpp"

namespace std {
const nothrow_t nothrow{};
}

void *operator new(size_t size) { return r2::heap::allocate(size); }
void *operator new[](size_t size) { return r2::heap::allocate(size); }

void *operator new(size_t size, const std::nothrow_t &) noexcept {
    return r2::heap::allocate(size);
}
void *operator new[](size_t size, const std::nothrow_t &) noexcept {
    return r2::heap::allocate(size);
}

void *operator new(size_t size, std::align_val_t align) {
    return r2::heap::allocate_aligned(size, (size_t)align);
}
void *operator new[](size_t size, std::align_val_t align) {
    return r2::heap::allocate_aligned(size, (size_t)align);
}

void operator delete(void *ptr) noexcept { r2::heap::deallocate(ptr); }
void operator delete[](void *ptr) noexcept { r2::heap::deallocate(ptr); }
void operator delete(void *ptr, size_t) noexcept { r2::heap::deallocate(ptr); }
void operator delete[](void *ptr, size_t) noexcept { r2::heap::deallocate(ptr); }

void operator delete(void *ptr, std::align_val_t) noexcept { r2::heap::deallocate_aligned(ptr); }
void operator delete[](void *ptr, std::align_val_t) noexcept { r2::heap::deallocate_aligned(ptr); }
void operator delete(void *ptr, size_t, std::align_val_t) noexcept {
    r2::heap::deallocate_aligned(ptr);
}
void operator delete[](void *ptr, size_t, std::align_val_t) noexcept {
    r2::heap::deallocate_aligned(ptr);
}
