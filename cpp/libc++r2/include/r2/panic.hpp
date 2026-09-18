#ifndef _R2CXX_PANIC_HPP_
#define _R2CXX_PANIC_HPP_

/*
 *  panic.hpp — giving up.
 *
 *  Separate from process.hpp so that the low-level pieces (expected, the
 *  containers) can report a programming error without pulling in the whole
 *  process API.
 */

#include "source_location.hpp"
#include "string_view.hpp"

namespace r2 {

/*
 *  Prints a message, unbuffered, and exits with code 1 without running any
 *  destructor --- whatever went wrong may well be the heap.  This is what the
 *  runtime calls for a pure virtual call, a failed assertion, or an attempt to
 *  throw with exceptions disabled.
 */
[[noreturn]] void panic(string_view message);

/*  The same, with the position of the caller.  */
[[noreturn]] void panic_at(string_view message, source_location where = source_location::current());

} // namespace r2

/*
 *  R2_ASSERT — panics with file and line when the condition does not hold.
 *  Compiled out entirely when R2_NDEBUG is defined.
 */
#define R2_STRINGIFY_(x) #x
#define R2_STRINGIFY(x) R2_STRINGIFY_(x)

#ifdef R2_NDEBUG
#define R2_ASSERT(cond) ((void)0)
#else
#define R2_ASSERT(cond)                                                                            \
    ((cond) ? (void)0                                                                              \
            : ::r2::panic(::r2::string_view(__FILE__ ":" R2_STRINGIFY(__LINE__) ": " #cond)))
#endif

#endif
