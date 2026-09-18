#ifndef _R2CXX_NEW_HPP_
#define _R2CXX_NEW_HPP_

/*
 *  new.hpp
 *
 *  Allocation functions.  The compiler implicitly declares the ordinary forms
 *  of operator new and operator delete, but the placement forms have to be
 *  declared by a header before `new (p) T{...}` will compile, and the aligned
 *  forms are needed by any type whose alignment exceeds 16 bytes.
 *
 *  All of them are defined in src/new.cpp on top of r2::heap.  Allocation
 *  never throws --- libc++r2 is built -fno-exceptions --- so the throwing
 *  forms return nullptr on failure, and every container checks for it.
 */

#include "types.hpp"

namespace std {

/*  std::nothrow / std::align_val_t.  Declared in namespace std because that is
 *  where the compiler looks for them when it forms a call.  */
struct nothrow_t {
    explicit nothrow_t() = default;
};
extern const nothrow_t nothrow;

enum class align_val_t : size_t {};

} // namespace std

void *operator new(size_t size);
void *operator new[](size_t size);
void *operator new(size_t size, const std::nothrow_t &) noexcept;
void *operator new[](size_t size, const std::nothrow_t &) noexcept;
void *operator new(size_t size, std::align_val_t align);
void *operator new[](size_t size, std::align_val_t align);

void operator delete(void *ptr) noexcept;
void operator delete[](void *ptr) noexcept;
void operator delete(void *ptr, size_t size) noexcept;
void operator delete[](void *ptr, size_t size) noexcept;
void operator delete(void *ptr, std::align_val_t align) noexcept;
void operator delete[](void *ptr, std::align_val_t align) noexcept;
void operator delete(void *ptr, size_t size, std::align_val_t align) noexcept;
void operator delete[](void *ptr, size_t size, std::align_val_t align) noexcept;

/*  Placement new: construct in storage the caller already owns.  */
inline void *operator new(size_t, void *place) noexcept { return place; }
inline void *operator new[](size_t, void *place) noexcept { return place; }
inline void operator delete(void *, void *) noexcept {}
inline void operator delete[](void *, void *) noexcept {}

#endif
