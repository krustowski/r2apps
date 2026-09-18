#ifndef _R2CXX_ARRAY_HPP_
#define _R2CXX_ARRAY_HPP_

/*
 *  array.hpp — a fixed-size array that knows its own length.
 */

#include "algorithm.hpp"
#include "compare.hpp"
#include "tuple.hpp"

namespace r2 {

template <class T, size_t N> struct array {
    using value_type = T;
    using iterator = T *;
    using const_iterator = const T *;

    T elems_[N];

    constexpr T &operator[](size_t i) { return elems_[i]; }
    constexpr const T &operator[](size_t i) const { return elems_[i]; }

    constexpr T *at(size_t i) { return i < N ? elems_ + i : nullptr; }
    constexpr const T *at(size_t i) const { return i < N ? elems_ + i : nullptr; }

    constexpr T &front() { return elems_[0]; }
    constexpr const T &front() const { return elems_[0]; }
    constexpr T &back() { return elems_[N - 1]; }
    constexpr const T &back() const { return elems_[N - 1]; }

    constexpr T *data() noexcept { return elems_; }
    constexpr const T *data() const noexcept { return elems_; }

    constexpr size_t size() const noexcept { return N; }
    constexpr bool empty() const noexcept { return N == 0; }

    constexpr iterator begin() noexcept { return elems_; }
    constexpr iterator end() noexcept { return elems_ + N; }
    constexpr const_iterator begin() const noexcept { return elems_; }
    constexpr const_iterator end() const noexcept { return elems_ + N; }

    constexpr void fill(const T &value) {
        for (size_t i = 0; i < N; i++)
            elems_[i] = value;
    }
};

template <class T, size_t N>
constexpr bool operator==(const array<T, N> &a, const array<T, N> &b) {
    for (size_t i = 0; i < N; i++)
        if (!(a[i] == b[i]))
            return false;
    return true;
}

template <class T, size_t N>
constexpr bool operator!=(const array<T, N> &a, const array<T, N> &b) {
    return !(a == b);
}

/*  get<I>(arr) --- the third piece of the tuple protocol, found by ADL.  */
template <size_t I, class T, size_t N> constexpr T &get(array<T, N> &a) noexcept {
    static_assert(I < N, "array index out of range");
    return a.elems_[I];
}

template <size_t I, class T, size_t N> constexpr const T &get(const array<T, N> &a) noexcept {
    static_assert(I < N, "array index out of range");
    return a.elems_[I];
}

template <size_t I, class T, size_t N> constexpr T &&get(array<T, N> &&a) noexcept {
    static_assert(I < N, "array index out of range");
    return static_cast<T &&>(a.elems_[I]);
}

#if __cplusplus >= 202002L
template <class T, size_t N>
constexpr auto operator<=>(const array<T, N> &a, const array<T, N> &b) {
    for (size_t i = 0; i < N; i++) {
        if (auto order = a[i] <=> b[i]; order != 0)
            return order;
    }
    return decltype(a[0] <=> b[0])::equivalent;
}
#endif

} // namespace r2

namespace std {

template <class T, r2::usize N> struct tuple_size<r2::array<T, N>> {
    static constexpr r2::usize value = N;
};

template <r2::usize I, class T, r2::usize N> struct tuple_element<I, r2::array<T, N>> {
    using type = T;
};

} // namespace std

#endif
