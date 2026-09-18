#ifndef _R2CXX_TUPLE_HPP_
#define _R2CXX_TUPLE_HPP_

/*
 *  tuple.hpp — the tuple protocol, so that structured bindings work.
 *
 *  `auto [a, b, c] = arr;` makes the compiler look for std::tuple_size and
 *  std::tuple_element by name, and then for a get<I> found by argument-
 *  dependent lookup.  Without the first two, r2::array decomposes into its one
 *  member (the C array inside it) rather than into its elements.
 *
 *  Declaring the primary templates in namespace std is what a freestanding
 *  implementation has to do; the specialisations for r2::array and r2::pair
 *  live beside those types.
 */

#include "types.hpp"

namespace std {

template <class T> struct tuple_size;
template <size_t I, class T> struct tuple_element;

template <class T> struct tuple_size<const T> : tuple_size<T> {};
template <size_t I, class T> struct tuple_element<I, const T> {
    using type = const typename tuple_element<I, T>::type;
};

} // namespace std

#endif
