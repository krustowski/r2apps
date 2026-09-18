#ifndef _R2CXX_ALGORITHM_HPP_
#define _R2CXX_ALGORITHM_HPP_

/*
 *  algorithm.hpp
 *
 *  Range algorithms over raw iterators (which, in this library, are always
 *  pointers).  sort() is an introsort: insertion sort for short ranges,
 *  median-of-three quicksort for the rest, and a heapsort fallback once
 *  recursion gets deeper than 2*log2(n), so a hostile input cannot turn into
 *  quadratic time or blow the fixed stack.
 */

#include "utility.hpp"

namespace r2 {

template <class T = void> struct less {
    constexpr bool operator()(const T &a, const T &b) const { return a < b; }
};
template <> struct less<void> {
    template <class A, class B> constexpr bool operator()(const A &a, const B &b) const {
        return a < b;
    }
};

template <class T = void> struct greater {
    constexpr bool operator()(const T &a, const T &b) const { return a > b; }
};
template <> struct greater<void> {
    template <class A, class B> constexpr bool operator()(const A &a, const B &b) const {
        return a > b;
    }
};

template <class T> constexpr const T &min(const T &a, const T &b) { return b < a ? b : a; }
template <class T> constexpr const T &max(const T &a, const T &b) { return a < b ? b : a; }

template <class T> constexpr const T &clamp(const T &v, const T &lo, const T &hi) {
    return v < lo ? lo : (hi < v ? hi : v);
}

template <class T> constexpr T abs_diff(const T &a, const T &b) { return a < b ? b - a : a - b; }

template <class In, class Out> constexpr Out copy(In first, In last, Out out) {
    while (first != last)
        *out++ = *first++;
    return out;
}

template <class In, class Out> constexpr Out copy_n(In first, size_t n, Out out) {
    while (n--)
        *out++ = *first++;
    return out;
}

template <class In, class Out> constexpr Out copy_backward(In first, In last, Out out_last) {
    while (first != last)
        *--out_last = *--last;
    return out_last;
}

template <class In, class Out> constexpr Out move_range(In first, In last, Out out) {
    while (first != last)
        *out++ = r2::move(*first++);
    return out;
}

template <class It, class T> constexpr void fill(It first, It last, const T &value) {
    for (; first != last; ++first)
        *first = value;
}

template <class It, class T> constexpr It fill_n(It first, size_t n, const T &value) {
    while (n--)
        *first++ = value;
    return first;
}

template <class It, class T> constexpr It find(It first, It last, const T &value) {
    for (; first != last; ++first)
        if (*first == value)
            return first;
    return last;
}

template <class It, class Pred> constexpr It find_if(It first, It last, Pred pred) {
    for (; first != last; ++first)
        if (pred(*first))
            return first;
    return last;
}

template <class It, class Pred> constexpr bool any_of(It first, It last, Pred pred) {
    return find_if(first, last, pred) != last;
}

template <class It, class Pred> constexpr bool all_of(It first, It last, Pred pred) {
    for (; first != last; ++first)
        if (!pred(*first))
            return false;
    return true;
}

template <class It, class T> constexpr size_t count(It first, It last, const T &value) {
    size_t n = 0;
    for (; first != last; ++first)
        if (*first == value)
            n++;
    return n;
}

template <class A, class B> constexpr bool equal(A first1, A last1, B first2) {
    for (; first1 != last1; ++first1, ++first2)
        if (!(*first1 == *first2))
            return false;
    return true;
}

template <class It> constexpr void reverse(It first, It last) {
    while (first < last)
        swap(*first++, *--last);
}

template <class It, class Cmp> constexpr It min_element(It first, It last, Cmp cmp) {
    if (first == last)
        return last;
    It best = first;
    for (++first; first != last; ++first)
        if (cmp(*first, *best))
            best = first;
    return best;
}

template <class It> constexpr It min_element(It first, It last) {
    return min_element(first, last, less<>{});
}

template <class It, class Cmp> constexpr It max_element(It first, It last, Cmp cmp) {
    if (first == last)
        return last;
    It best = first;
    for (++first; first != last; ++first)
        if (cmp(*best, *first))
            best = first;
    return best;
}

template <class It> constexpr It max_element(It first, It last) {
    return max_element(first, last, less<>{});
}

namespace detail {

template <class It, class Cmp> void insertion_sort(It first, It last, Cmp cmp) {
    if (first == last)
        return;
    for (It i = first + 1; i < last; ++i) {
        auto key = r2::move(*i);
        It j = i;
        while (j > first && cmp(key, *(j - 1))) {
            *j = r2::move(*(j - 1));
            --j;
        }
        *j = r2::move(key);
    }
}

template <class It, class Cmp> void sift_down(It first, ptrdiff_t root, ptrdiff_t n, Cmp cmp) {
    while (true) {
        ptrdiff_t child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && cmp(first[child], first[child + 1]))
            child++;
        if (!cmp(first[root], first[child]))
            return;
        swap(first[root], first[child]);
        root = child;
    }
}

template <class It, class Cmp> void heap_sort(It first, It last, Cmp cmp) {
    ptrdiff_t n = last - first;
    for (ptrdiff_t i = n / 2 - 1; i >= 0; --i)
        sift_down(first, i, n, cmp);
    for (ptrdiff_t end = n - 1; end > 0; --end) {
        swap(first[0], first[end]);
        sift_down(first, 0, end, cmp);
    }
}

/*  Hoare partition around the median of first, middle and last.  */
template <class It, class Cmp> It partition_range(It first, It last, Cmp cmp) {
    It mid = first + (last - first) / 2;
    It hi = last - 1;
    if (cmp(*mid, *first))
        swap(*mid, *first);
    if (cmp(*hi, *mid)) {
        swap(*hi, *mid);
        if (cmp(*mid, *first))
            swap(*mid, *first);
    }
    auto pivot = *mid; /*  a copy: the range is about to be shuffled  */

    It i = first;
    It j = hi;
    while (true) {
        while (cmp(*i, pivot))
            ++i;
        while (cmp(pivot, *j))
            --j;
        if (i >= j)
            return j;
        swap(*i, *j);
        ++i;
        --j;
    }
}

template <class It, class Cmp> void introsort(It first, It last, Cmp cmp, int depth_budget) {
    constexpr ptrdiff_t SMALL_RANGE = 16;

    while (last - first > SMALL_RANGE) {
        if (depth_budget == 0) {
            heap_sort(first, last, cmp);
            return;
        }
        depth_budget--;

        It split = partition_range(first, last, cmp);
        /*  Recurse into the smaller half, loop on the larger one: recursion
         *  depth stays logarithmic even in the worst case.  */
        if (split - first < last - (split + 1)) {
            introsort(first, split + 1, cmp, depth_budget);
            first = split + 1;
        } else {
            introsort(split + 1, last, cmp, depth_budget);
            last = split + 1;
        }
    }
    insertion_sort(first, last, cmp);
}

} // namespace detail

/*  Sorts [first, last).  The value type must be copyable (the pivot is held
 *  by value) and movable.  */
template <class It, class Cmp> void sort(It first, It last, Cmp cmp) {
    ptrdiff_t n = last - first;
    if (n < 2)
        return;

    int log2n = 0;
    for (ptrdiff_t v = n; v > 1; v >>= 1)
        log2n++;

    detail::introsort(first, last, cmp, 2 * log2n);
}

template <class It> void sort(It first, It last) { sort(first, last, less<>{}); }

template <class It, class T, class Cmp> It lower_bound(It first, It last, const T &value, Cmp cmp) {
    ptrdiff_t len = last - first;
    while (len > 0) {
        ptrdiff_t half = len / 2;
        It mid = first + half;
        if (cmp(*mid, value)) {
            first = mid + 1;
            len -= half + 1;
        } else {
            len = half;
        }
    }
    return first;
}

template <class It, class T> It lower_bound(It first, It last, const T &value) {
    return lower_bound(first, last, value, less<>{});
}

template <class It, class T> bool binary_search(It first, It last, const T &value) {
    It it = lower_bound(first, last, value);
    return it != last && !(value < *it);
}

} // namespace r2

#endif
