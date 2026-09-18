#ifndef _R2CXX_UTILITY_HPP_
#define _R2CXX_UTILITY_HPP_

/*
 *  utility.hpp
 *
 *  move, forward, swap, exchange and pair.
 */

#include "type_traits.hpp"

namespace r2 {

template <class T> constexpr remove_reference_t<T> &&move(T &&t) noexcept {
    return static_cast<remove_reference_t<T> &&>(t);
}

template <class T> constexpr T &&forward(remove_reference_t<T> &t) noexcept {
    return static_cast<T &&>(t);
}

template <class T> constexpr T &&forward(remove_reference_t<T> &&t) noexcept {
    static_assert(!is_lvalue_reference<T>::value, "cannot forward an rvalue as an lvalue");
    return static_cast<T &&>(t);
}

template <class T> constexpr void swap(T &a, T &b) noexcept {
    T tmp = move(a);
    a = move(b);
    b = move(tmp);
}

template <class T, size_t N> constexpr void swap(T (&a)[N], T (&b)[N]) noexcept {
    for (size_t i = 0; i < N; i++)
        swap(a[i], b[i]);
}

template <class T, class U = T> constexpr T exchange(T &obj, U &&new_value) {
    T old = move(obj);
    obj = forward<U>(new_value);
    return old;
}

/*  Cast an enum class to its underlying integer.  The kernel ABI is a wall of
 *  integers, so this comes up constantly.  */
template <class E> constexpr underlying_type_t<E> to_underlying(E e) noexcept {
    return static_cast<underlying_type_t<E>>(e);
}

template <class T1, class T2> struct pair {
    using first_type = T1;
    using second_type = T2;

    T1 first{};
    T2 second{};

    constexpr pair() = default;
    constexpr pair(const T1 &a, const T2 &b) : first(a), second(b) {}
    template <class U1, class U2>
    constexpr pair(U1 &&a, U2 &&b) : first(forward<U1>(a)), second(forward<U2>(b)) {}

    constexpr void swap(pair &other) {
        r2::swap(first, other.first);
        r2::swap(second, other.second);
    }
};

template <class T1, class T2> constexpr pair<T1, T2> make_pair(T1 a, T2 b) {
    return pair<T1, T2>(move(a), move(b));
}

template <class T1, class T2>
constexpr bool operator==(const pair<T1, T2> &a, const pair<T1, T2> &b) {
    return a.first == b.first && a.second == b.second;
}

template <class T1, class T2>
constexpr bool operator!=(const pair<T1, T2> &a, const pair<T1, T2> &b) {
    return !(a == b);
}

template <class T1, class T2>
constexpr bool operator<(const pair<T1, T2> &a, const pair<T1, T2> &b) {
    if (a.first < b.first)
        return true;
    if (b.first < a.first)
        return false;
    return a.second < b.second;
}

} // namespace r2

#endif
