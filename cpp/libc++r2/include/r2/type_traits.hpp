#ifndef _R2CXX_TYPE_TRAITS_HPP_
#define _R2CXX_TYPE_TRAITS_HPP_

/*
 *  type_traits.hpp
 *
 *  The traits the containers in this library actually use.  Not a complete
 *  <type_traits>: the ones that need compiler support use the __is_* builtins,
 *  the rest are written out.
 */

#include "types.hpp"

namespace r2 {

template <class T, T v> struct integral_constant {
    static constexpr T value = v;
    using value_type = T;
    using type = integral_constant;
    constexpr operator value_type() const noexcept { return value; }
    constexpr value_type operator()() const noexcept { return value; }
};

using true_type = integral_constant<bool, true>;
using false_type = integral_constant<bool, false>;

template <bool B, class T, class F> struct conditional { using type = T; };
template <class T, class F> struct conditional<false, T, F> { using type = F; };
template <bool B, class T, class F> using conditional_t = typename conditional<B, T, F>::type;

template <bool B, class T = void> struct enable_if {};
template <class T> struct enable_if<true, T> { using type = T; };
template <bool B, class T = void> using enable_if_t = typename enable_if<B, T>::type;

template <class T, class U> struct is_same : false_type {};
template <class T> struct is_same<T, T> : true_type {};
template <class T, class U> inline constexpr bool is_same_v = is_same<T, U>::value;

template <class T> struct remove_reference { using type = T; };
template <class T> struct remove_reference<T &> { using type = T; };
template <class T> struct remove_reference<T &&> { using type = T; };
template <class T> using remove_reference_t = typename remove_reference<T>::type;

template <class T> struct remove_const { using type = T; };
template <class T> struct remove_const<const T> { using type = T; };
template <class T> using remove_const_t = typename remove_const<T>::type;

template <class T> struct remove_volatile { using type = T; };
template <class T> struct remove_volatile<volatile T> { using type = T; };

template <class T> struct remove_cv {
    using type = typename remove_volatile<remove_const_t<T>>::type;
};
template <class T> using remove_cv_t = typename remove_cv<T>::type;

template <class T> struct remove_cvref {
    using type = remove_cv_t<remove_reference_t<T>>;
};
template <class T> using remove_cvref_t = typename remove_cvref<T>::type;

template <class T> struct remove_extent { using type = T; };
template <class T> struct remove_extent<T[]> { using type = T; };
template <class T, size_t N> struct remove_extent<T[N]> { using type = T; };
template <class T> using remove_extent_t = typename remove_extent<T>::type;

template <class T> struct remove_pointer { using type = T; };
template <class T> struct remove_pointer<T *> { using type = T; };
template <class T> struct remove_pointer<T *const> { using type = T; };

template <class T> struct is_array : false_type {};
template <class T> struct is_array<T[]> : true_type {};
template <class T, size_t N> struct is_array<T[N]> : true_type {};
template <class T> inline constexpr bool is_array_v = is_array<T>::value;

template <class T> struct is_lvalue_reference : false_type {};
template <class T> struct is_lvalue_reference<T &> : true_type {};

template <class T> struct is_pointer_helper : false_type {};
template <class T> struct is_pointer_helper<T *> : true_type {};
template <class T> struct is_pointer : is_pointer_helper<remove_cv_t<T>> {};
template <class T> inline constexpr bool is_pointer_v = is_pointer<T>::value;

template <class T> struct is_void : is_same<void, remove_cv_t<T>> {};

template <class T> struct is_const : false_type {};
template <class T> struct is_const<const T> : true_type {};

/*  Compiler-backed traits: no way to write these in the language itself.  */
template <class T>
inline constexpr bool is_trivially_copyable_v = __is_trivially_copyable(T);
template <class T>
inline constexpr bool is_trivially_destructible_v = __has_trivial_destructor(T);
template <class B, class D> inline constexpr bool is_base_of_v = __is_base_of(B, D);
template <class T> inline constexpr bool is_enum_v = __is_enum(T);
template <class T> inline constexpr bool is_class_v = __is_class(T);

template <class T>
inline constexpr bool is_integral_v =
    is_same_v<remove_cv_t<T>, bool> || is_same_v<remove_cv_t<T>, char> ||
    is_same_v<remove_cv_t<T>, signed char> || is_same_v<remove_cv_t<T>, unsigned char> ||
    is_same_v<remove_cv_t<T>, short> || is_same_v<remove_cv_t<T>, unsigned short> ||
    is_same_v<remove_cv_t<T>, int> || is_same_v<remove_cv_t<T>, unsigned int> ||
    is_same_v<remove_cv_t<T>, long> || is_same_v<remove_cv_t<T>, unsigned long> ||
    is_same_v<remove_cv_t<T>, long long> || is_same_v<remove_cv_t<T>, unsigned long long>;

template <class T>
inline constexpr bool is_floating_point_v =
    is_same_v<remove_cv_t<T>, float> || is_same_v<remove_cv_t<T>, double> ||
    is_same_v<remove_cv_t<T>, long double>;

template <class T> inline constexpr bool is_signed_v = is_integral_v<T> && (T(-1) < T(0));

template <class T> struct make_unsigned { using type = T; };
template <> struct make_unsigned<char> { using type = unsigned char; };
template <> struct make_unsigned<signed char> { using type = unsigned char; };
template <> struct make_unsigned<short> { using type = unsigned short; };
template <> struct make_unsigned<int> { using type = unsigned int; };
template <> struct make_unsigned<long> { using type = unsigned long; };
template <> struct make_unsigned<long long> { using type = unsigned long long; };
template <class T> using make_unsigned_t = typename make_unsigned<T>::type;

/*  declval: name a value of T in an unevaluated context.  */
template <class T> T &&declval() noexcept;

template <class T> struct underlying_type { using type = __underlying_type(T); };
template <class T> using underlying_type_t = typename underlying_type<T>::type;

} // namespace r2

#endif
