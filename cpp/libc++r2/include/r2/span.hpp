#ifndef _R2CXX_SPAN_HPP_
#define _R2CXX_SPAN_HPP_

/*
 *  span.hpp — a non-owning view of contiguous elements.
 *
 *  The natural parameter type for anything that takes a buffer: it carries the
 *  length along with the pointer, which is the half of every kernel ABI call
 *  that is easiest to get wrong.
 */

#include "array.hpp"

namespace r2 {

template <class T> class span {
  public:
    using value_type = remove_cv_t<T>;
    using iterator = T *;

    constexpr span() noexcept : ptr_(nullptr), len_(0) {}
    constexpr span(T *ptr, size_t len) noexcept : ptr_(ptr), len_(len) {}
    constexpr span(T *first, T *last) noexcept : ptr_(first), len_((size_t)(last - first)) {}

    template <size_t N> constexpr span(T (&arr)[N]) noexcept : ptr_(arr), len_(N) {}
    template <size_t N> constexpr span(array<value_type, N> &arr) noexcept
        : ptr_(arr.data()), len_(N) {}

    constexpr T *data() const noexcept { return ptr_; }
    constexpr size_t size() const noexcept { return len_; }
    constexpr size_t size_bytes() const noexcept { return len_ * sizeof(T); }
    constexpr bool empty() const noexcept { return len_ == 0; }

    constexpr T &operator[](size_t i) const { return ptr_[i]; }
    constexpr T &front() const { return ptr_[0]; }
    constexpr T &back() const { return ptr_[len_ - 1]; }

    constexpr iterator begin() const noexcept { return ptr_; }
    constexpr iterator end() const noexcept { return ptr_ + len_; }

    constexpr span subspan(size_t offset, size_t count = npos) const {
        if (offset > len_)
            return span();
        size_t n = (count == npos || offset + count > len_) ? len_ - offset : count;
        return span(ptr_ + offset, n);
    }

    constexpr span first(size_t count) const { return subspan(0, count); }
    constexpr span last(size_t count) const {
        return count >= len_ ? *this : subspan(len_ - count, count);
    }

  private:
    T *ptr_;
    size_t len_;
};

using byte_span = span<uint8_t>;
using const_byte_span = span<const uint8_t>;

/*  Reinterpret any object as the bytes the kernel ABI wants.  */
template <class T> const_byte_span as_bytes(const T &obj) {
    return const_byte_span(reinterpret_cast<const uint8_t *>(&obj), sizeof(T));
}

template <class T> byte_span as_writable_bytes(T &obj) {
    return byte_span(reinterpret_cast<uint8_t *>(&obj), sizeof(T));
}

} // namespace r2

#endif
