#ifndef _R2CXX_TYPES_HPP_
#define _R2CXX_TYPES_HPP_

/*
 *  types.hpp
 *
 *  Fundamental types.  libc++r2 is built -nostdinc -nostdinc++, so nothing
 *  here comes from a system header; the widths are the compiler's own.
 *
 *  The int8_t/uint8_t family is spelled exactly as c/libcr2/types.h spells it
 *  (int8_t is char, not signed char, and int64_t is long).  Repeating a
 *  typedef is legal only when it names the same type, so matching libcr2 byte
 *  for byte is what lets a translation unit include both headers --- which an
 *  app doing C++ with libcr2's TCP stack underneath has to do.
 */

typedef char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long int64_t;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTPTR_TYPE__ intptr_t;

namespace r2 {

/*  Short aliases, for code that is written for r2 rather than ported to it.  */
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using usize = size_t;
using isize = ptrdiff_t;

using f32 = float;
using f64 = double;

using nullptr_t = decltype(nullptr);

/*  The byte type the kernel ABI speaks: every string and buffer crossing a
 *  syscall boundary is uint8_t*, never char*.  */
using byte = uint8_t;

static_assert(sizeof(i64) == 8, "int64_t must be 64 bits wide");
static_assert(sizeof(usize) == 8, "libc++r2 targets x86-64 only");

/*  npos, the "no position" marker shared by string and string_view.  */
inline constexpr usize npos = (usize)-1;

} // namespace r2

#endif
