#ifndef _R2CXX_CONCEPTS_HPP_
#define _R2CXX_CONCEPTS_HPP_

/*
 *  concepts.hpp — the constraints the library uses on its own templates, and
 *  that applications can use on theirs.
 *
 *  Concepts are a language feature, so these need no compiler support beyond
 *  C++20 itself; they are written with requires-expressions rather than
 *  __is_* builtins so that they mean exactly what they say.
 *
 *  The whole header is inside a C++20 guard.  Built as C++17 it is empty, and
 *  R2_REQUIRES (types.hpp) erases the constraints at the use sites, so the
 *  same library serves both.
 */

#include "type_traits.hpp"

#if __cplusplus >= 202002L

namespace r2 {

namespace detail {
template <class To> void takes_implicitly(To) noexcept;
}

template <class T, class U>
concept same_as = is_same_v<T, U> && is_same_v<U, T>;

template <class T, class U>
concept different_from = !same_as<remove_cvref_t<T>, remove_cvref_t<U>>;

template <class From, class To>
concept convertible_to = requires(From &&from) {
    detail::takes_implicitly<To>(static_cast<From &&>(from));
};

template <class T>
concept integral = is_integral_v<T>;

template <class T>
concept signed_integral = integral<T> && is_signed_v<T>;

template <class T>
concept unsigned_integral = integral<T> && !is_signed_v<T>;

template <class T>
concept floating_point = is_floating_point_v<T>;

template <class T>
concept arithmetic = integral<T> || floating_point<T>;

template <class T>
concept enumeration = is_enum_v<T>;

template <class T>
concept class_type = is_class_v<T>;

template <class T>
concept pointer = is_pointer_v<T>;

/*  A type whose bytes may be copied with memcpy.  */
template <class T>
concept trivially_copyable = is_trivially_copyable_v<T>;

/*  char and the byte-ish types the kernel ABI speaks.  */
template <class T>
concept character = same_as<remove_cv_t<T>, char> || same_as<remove_cv_t<T>, unsigned char> ||
                    same_as<remove_cv_t<T>, signed char>;

template <class T>
concept default_initializable = requires { T{}; };

template <class T>
concept move_constructible = requires(T &&value) { T(static_cast<T &&>(value)); };

template <class T>
concept copy_constructible = requires(const T &value) { T(value); };

template <class T, class... Args>
concept constructible_from = requires(Args &&...args) { T(static_cast<Args &&>(args)...); };

template <class T, class U>
concept assignable_from = requires(T target, U &&source) {
    { target = static_cast<U &&>(source) } -> same_as<T>;
};

template <class T>
concept equality_comparable = requires(const T &a, const T &b) {
    { a == b } -> convertible_to<bool>;
    { a != b } -> convertible_to<bool>;
};

template <class T, class U>
concept equality_comparable_with = requires(const T &a, const U &b) {
    { a == b } -> convertible_to<bool>;
};

template <class T>
concept totally_ordered = equality_comparable<T> && requires(const T &a, const T &b) {
    { a < b } -> convertible_to<bool>;
    { a > b } -> convertible_to<bool>;
};

template <class F, class... Args>
concept invocable = requires(F &&f, Args &&...args) {
    static_cast<F &&>(f)(static_cast<Args &&>(args)...);
};

template <class F, class... Args>
concept predicate = invocable<F, Args...> && requires(F &&f, Args &&...args) {
    { static_cast<F &&>(f)(static_cast<Args &&>(args)...) } -> convertible_to<bool>;
};

/*  A comparison function usable by sort(): strict weak ordering over T.  */
template <class C, class T>
concept comparator = predicate<C, const T &, const T &>;

/*  Anything with data() and size(): vector, string, span, array --- the shape
 *  every buffer-taking function in this library really wants.  */
template <class R>
concept contiguous_range = requires(R &range) {
    range.data();
    { range.size() } -> convertible_to<size_t>;
};

template <class R>
concept byte_range = contiguous_range<R> && requires(R &range) {
    requires sizeof(*range.data()) == 1;
};

/*  An iterator this library's algorithms can drive: pointers, in practice.  */
template <class It>
concept forward_iterator = requires(It it) {
    *it;
    { ++it } -> same_as<It &>;
    { it != it } -> convertible_to<bool>;
};

template <class It>
concept random_access_iterator = forward_iterator<It> && requires(It it, ptrdiff_t n) {
    { it + n } -> same_as<It>;
    { it - it } -> convertible_to<ptrdiff_t>;
    { it[n] };
};

} // namespace r2

#endif /*  __cplusplus >= 202002L  */

#endif
