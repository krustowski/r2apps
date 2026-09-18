#ifndef _R2CXX_COMPARE_HPP_
#define _R2CXX_COMPARE_HPP_

/*
 *  compare.hpp — the three-way comparison categories.
 *
 *  Like std::initializer_list, these are part of the language rather than the
 *  library: writing `auto operator<=>(const T&) const = default` makes the
 *  compiler look up std::strong_ordering by name, and without it the operator
 *  is silently deleted.  A freestanding C++20 implementation has to supply
 *  them, so here they are.
 *
 *  The layout follows the one the compiler expects: a single signed char
 *  holding -1, 0, 1 or 2 (unordered).  Comparisons are only ever against the
 *  literal 0, which is what __unspec enforces --- it can be constructed from a
 *  null pointer constant and nothing else, so `result < 0` compiles and
 *  `result < 1` does not.
 *
 *  Everything here is inside a C++20 guard, so a translation unit built as
 *  C++17 (cpp/memento-hello, for one) sees nothing at all.
 */

#include "types.hpp"

#if __cplusplus >= 202002L

namespace std {

namespace __r2_cmp {

using value_type = signed char;

enum class Ord : value_type { equivalent = 0, less = -1, greater = 1 };
enum class Ncmp : value_type { unordered = 2 };

/*  Only a literal 0 converts to this, which is how the standard spells
 *  "comparable against 0 and nothing else".  */
struct unspec {
    consteval unspec(unspec *) noexcept {}
};

} // namespace __r2_cmp

class partial_ordering {
  public:
    static const partial_ordering less;
    static const partial_ordering equivalent;
    static const partial_ordering greater;
    static const partial_ordering unordered;

    friend constexpr bool operator==(partial_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ == 0;
    }
    friend constexpr bool operator==(partial_ordering, partial_ordering) noexcept = default;

    friend constexpr bool operator<(partial_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ == -1;
    }
    friend constexpr bool operator>(partial_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ == 1;
    }
    friend constexpr bool operator<=(partial_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ <= 0 && v.value_ != 2;
    }
    friend constexpr bool operator>=(partial_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ == 0 || v.value_ == 1;
    }

    friend constexpr bool operator<(__r2_cmp::unspec, partial_ordering v) noexcept {
        return v.value_ == 1;
    }
    friend constexpr bool operator>(__r2_cmp::unspec, partial_ordering v) noexcept {
        return v.value_ == -1;
    }
    friend constexpr bool operator<=(__r2_cmp::unspec, partial_ordering v) noexcept {
        return v.value_ == 0 || v.value_ == 1;
    }
    friend constexpr bool operator>=(__r2_cmp::unspec, partial_ordering v) noexcept {
        return v.value_ <= 0 && v.value_ != 2;
    }

    friend constexpr partial_ordering operator<=>(partial_ordering v, __r2_cmp::unspec) noexcept {
        return v;
    }
    friend constexpr partial_ordering operator<=>(__r2_cmp::unspec, partial_ordering v) noexcept {
        return partial_ordering(__r2_cmp::value_type(-v.value_));
    }

  private:
    constexpr explicit partial_ordering(__r2_cmp::value_type v) noexcept : value_(v) {}
    constexpr explicit partial_ordering(__r2_cmp::Ord v) noexcept
        : value_((__r2_cmp::value_type)v) {}
    constexpr explicit partial_ordering(__r2_cmp::Ncmp v) noexcept
        : value_((__r2_cmp::value_type)v) {}

    friend class weak_ordering;
    friend class strong_ordering;

    __r2_cmp::value_type value_;
};

inline constexpr partial_ordering partial_ordering::less(__r2_cmp::Ord::less);
inline constexpr partial_ordering partial_ordering::equivalent(__r2_cmp::Ord::equivalent);
inline constexpr partial_ordering partial_ordering::greater(__r2_cmp::Ord::greater);
inline constexpr partial_ordering partial_ordering::unordered(__r2_cmp::Ncmp::unordered);

class weak_ordering {
  public:
    static const weak_ordering less;
    static const weak_ordering equivalent;
    static const weak_ordering greater;

    constexpr operator partial_ordering() const noexcept { return partial_ordering(value_); }

    friend constexpr bool operator==(weak_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ == 0;
    }
    friend constexpr bool operator==(weak_ordering, weak_ordering) noexcept = default;

    friend constexpr bool operator<(weak_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ < 0;
    }
    friend constexpr bool operator>(weak_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ > 0;
    }
    friend constexpr bool operator<=(weak_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ <= 0;
    }
    friend constexpr bool operator>=(weak_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ >= 0;
    }

    friend constexpr bool operator<(__r2_cmp::unspec, weak_ordering v) noexcept {
        return v.value_ > 0;
    }
    friend constexpr bool operator>(__r2_cmp::unspec, weak_ordering v) noexcept {
        return v.value_ < 0;
    }
    friend constexpr bool operator<=(__r2_cmp::unspec, weak_ordering v) noexcept {
        return v.value_ >= 0;
    }
    friend constexpr bool operator>=(__r2_cmp::unspec, weak_ordering v) noexcept {
        return v.value_ <= 0;
    }

    friend constexpr weak_ordering operator<=>(weak_ordering v, __r2_cmp::unspec) noexcept {
        return v;
    }
    friend constexpr weak_ordering operator<=>(__r2_cmp::unspec, weak_ordering v) noexcept {
        return weak_ordering(__r2_cmp::value_type(-v.value_));
    }

  private:
    constexpr explicit weak_ordering(__r2_cmp::value_type v) noexcept : value_(v) {}
    constexpr explicit weak_ordering(__r2_cmp::Ord v) noexcept
        : value_((__r2_cmp::value_type)v) {}

    friend class strong_ordering;

    __r2_cmp::value_type value_;
};

inline constexpr weak_ordering weak_ordering::less(__r2_cmp::Ord::less);
inline constexpr weak_ordering weak_ordering::equivalent(__r2_cmp::Ord::equivalent);
inline constexpr weak_ordering weak_ordering::greater(__r2_cmp::Ord::greater);

class strong_ordering {
  public:
    static const strong_ordering less;
    static const strong_ordering equal;
    static const strong_ordering equivalent;
    static const strong_ordering greater;

    constexpr operator partial_ordering() const noexcept { return partial_ordering(value_); }
    constexpr operator weak_ordering() const noexcept { return weak_ordering(value_); }

    friend constexpr bool operator==(strong_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ == 0;
    }
    friend constexpr bool operator==(strong_ordering, strong_ordering) noexcept = default;

    friend constexpr bool operator<(strong_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ < 0;
    }
    friend constexpr bool operator>(strong_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ > 0;
    }
    friend constexpr bool operator<=(strong_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ <= 0;
    }
    friend constexpr bool operator>=(strong_ordering v, __r2_cmp::unspec) noexcept {
        return v.value_ >= 0;
    }

    friend constexpr bool operator<(__r2_cmp::unspec, strong_ordering v) noexcept {
        return v.value_ > 0;
    }
    friend constexpr bool operator>(__r2_cmp::unspec, strong_ordering v) noexcept {
        return v.value_ < 0;
    }
    friend constexpr bool operator<=(__r2_cmp::unspec, strong_ordering v) noexcept {
        return v.value_ >= 0;
    }
    friend constexpr bool operator>=(__r2_cmp::unspec, strong_ordering v) noexcept {
        return v.value_ <= 0;
    }

    friend constexpr strong_ordering operator<=>(strong_ordering v, __r2_cmp::unspec) noexcept {
        return v;
    }
    friend constexpr strong_ordering operator<=>(__r2_cmp::unspec, strong_ordering v) noexcept {
        return strong_ordering(__r2_cmp::value_type(-v.value_));
    }

  private:
    constexpr explicit strong_ordering(__r2_cmp::value_type v) noexcept : value_(v) {}
    constexpr explicit strong_ordering(__r2_cmp::Ord v) noexcept
        : value_((__r2_cmp::value_type)v) {}

    __r2_cmp::value_type value_;
};

inline constexpr strong_ordering strong_ordering::less(__r2_cmp::Ord::less);
inline constexpr strong_ordering strong_ordering::equal(__r2_cmp::Ord::equivalent);
inline constexpr strong_ordering strong_ordering::equivalent(__r2_cmp::Ord::equivalent);
inline constexpr strong_ordering strong_ordering::greater(__r2_cmp::Ord::greater);

/*
 *  common_comparison_category — the return type the compiler forms for a
 *  defaulted operator<=> over several members: the weakest category among
 *  them, and void if any member is not comparable at all.
 *
 *  Written as explicit specialisations rather than anything clever, because
 *  this is exactly nine combinations and the compiler consults it while
 *  synthesising operators.
 */
namespace __r2_cmp {

template <class A, class B> struct merge {
    using type = void; /*  anything involving void stays void  */
};

template <> struct merge<strong_ordering, strong_ordering> {
    using type = strong_ordering;
};
template <> struct merge<strong_ordering, weak_ordering> {
    using type = weak_ordering;
};
template <> struct merge<strong_ordering, partial_ordering> {
    using type = partial_ordering;
};
template <> struct merge<weak_ordering, strong_ordering> {
    using type = weak_ordering;
};
template <> struct merge<weak_ordering, weak_ordering> {
    using type = weak_ordering;
};
template <> struct merge<weak_ordering, partial_ordering> {
    using type = partial_ordering;
};
template <> struct merge<partial_ordering, strong_ordering> {
    using type = partial_ordering;
};
template <> struct merge<partial_ordering, weak_ordering> {
    using type = partial_ordering;
};
template <> struct merge<partial_ordering, partial_ordering> {
    using type = partial_ordering;
};

template <class... Ts> struct common_category {
    using type = void;
};

template <> struct common_category<> {
    using type = strong_ordering;
};

template <> struct common_category<strong_ordering> {
    using type = strong_ordering;
};
template <> struct common_category<weak_ordering> {
    using type = weak_ordering;
};
template <> struct common_category<partial_ordering> {
    using type = partial_ordering;
};

template <class T, class U, class... Rest> struct common_category<T, U, Rest...> {
    using type = typename merge<typename common_category<T>::type,
                                typename common_category<U, Rest...>::type>::type;
};

template <class T> T declval() noexcept;

} // namespace __r2_cmp

template <class... Ts> struct common_comparison_category {
    using type = typename __r2_cmp::common_category<Ts...>::type;
};

template <class... Ts>
using common_comparison_category_t = typename common_comparison_category<Ts...>::type;

template <class T, class U = T> struct compare_three_way_result {
    using type = decltype(__r2_cmp::declval<const T &>() <=> __r2_cmp::declval<const U &>());
};

template <class T, class U = T>
using compare_three_way_result_t = typename compare_three_way_result<T, U>::type;

} // namespace std

namespace r2 {

/*  The named checks, for code that would rather not compare against 0.  */
constexpr bool is_eq(std::partial_ordering o) noexcept { return o == 0; }
constexpr bool is_neq(std::partial_ordering o) noexcept { return o != 0; }
constexpr bool is_lt(std::partial_ordering o) noexcept { return o < 0; }
constexpr bool is_lteq(std::partial_ordering o) noexcept { return o <= 0; }
constexpr bool is_gt(std::partial_ordering o) noexcept { return o > 0; }
constexpr bool is_gteq(std::partial_ordering o) noexcept { return o >= 0; }

/*  The ordering of two values of a built-in or user type.  */
template <class T, class U> constexpr auto compare(const T &a, const U &b) { return a <=> b; }

/*  Three-way comparison built out of < for types that predate <=>.  */
template <class T> constexpr std::strong_ordering compare_fallback(const T &a, const T &b) {
    if (a < b)
        return std::strong_ordering::less;
    if (b < a)
        return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

} // namespace r2

#endif /*  __cplusplus >= 202002L  */

#endif
