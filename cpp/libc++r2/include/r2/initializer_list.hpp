#ifndef _R2CXX_INITIALIZER_LIST_HPP_
#define _R2CXX_INITIALIZER_LIST_HPP_

/*
 *  initializer_list.hpp
 *
 *  std::initializer_list is part of the language, not the library: writing
 *  `vector<int> v{1, 2, 3}` makes the compiler look up std::initializer_list
 *  by name and build an object with exactly this layout (a pointer and a
 *  length, in that order, constructed through the private two-argument
 *  constructor).  A freestanding C++ implementation has to supply it.
 */

#include "types.hpp"

namespace std {

template <class T> class initializer_list {
  public:
    using value_type = T;
    using reference = const T &;
    using const_reference = const T &;
    using size_type = size_t;
    using iterator = const T *;
    using const_iterator = const T *;

    constexpr initializer_list() noexcept : array_(nullptr), len_(0) {}

    constexpr size_type size() const noexcept { return len_; }
    constexpr const T *begin() const noexcept { return array_; }
    constexpr const T *end() const noexcept { return array_ + len_; }

  private:
    /*  The compiler calls this one.  Do not reorder the members.  */
    constexpr initializer_list(const T *array, size_type len) noexcept
        : array_(array), len_(len) {}

    const T *array_;
    size_type len_;
};

template <class T> constexpr const T *begin(initializer_list<T> il) noexcept { return il.begin(); }
template <class T> constexpr const T *end(initializer_list<T> il) noexcept { return il.end(); }

} // namespace std

namespace r2 {
template <class T> using init_list = std::initializer_list<T>;
}

#endif
