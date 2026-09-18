#ifndef _R2CXX_MEMORY_HPP_
#define _R2CXX_MEMORY_HPP_

/*
 *  memory.hpp
 *
 *  Object lifetime helpers and unique_ptr.  There is no shared_ptr: reference
 *  counting is rarely what a program with one thread, a fixed arena and a
 *  2 MiB address space wants, and leaving it out keeps ownership visible.
 */

#include "new.hpp"
#include "utility.hpp"

namespace r2 {

template <class T> constexpr T *addressof(T &arg) noexcept { return __builtin_addressof(arg); }

template <class T, class... Args> T *construct_at(T *place, Args &&...args) {
    return ::new ((void *)place) T(forward<Args>(args)...);
}

template <class T> void destroy_at(T *p) {
    if constexpr (!is_trivially_destructible_v<T>)
        p->~T();
}

template <class It> void destroy(It first, It last) {
    for (; first != last; ++first)
        destroy_at(addressof(*first));
}

template <class In, class Out> Out uninitialized_copy(In first, In last, Out out) {
    for (; first != last; ++first, ++out)
        construct_at(addressof(*out), *first);
    return out;
}

template <class In, class Out> Out uninitialized_move(In first, In last, Out out) {
    for (; first != last; ++first, ++out)
        construct_at(addressof(*out), r2::move(*first));
    return out;
}

template <class It, class T> void uninitialized_fill(It first, It last, const T &value) {
    for (; first != last; ++first)
        construct_at(addressof(*first), value);
}

template <class It> void uninitialized_default_construct(It first, It last) {
    for (; first != last; ++first)
        construct_at(addressof(*first));
}

template <class T> struct default_delete {
    constexpr default_delete() noexcept = default;
    void operator()(T *p) const { delete p; }
};

template <class T> struct default_delete<T[]> {
    constexpr default_delete() noexcept = default;
    void operator()(T *p) const { delete[] p; }
};

/*
 *  unique_ptr --- sole ownership, no overhead beyond the pointer itself when
 *  the deleter is empty.
 */
template <class T, class D = default_delete<T>> class unique_ptr {
  public:
    using pointer = T *;
    using element_type = T;
    using deleter_type = D;

    constexpr unique_ptr() noexcept : ptr_(nullptr) {}
    constexpr unique_ptr(nullptr_t) noexcept : ptr_(nullptr) {}
    explicit unique_ptr(pointer p) noexcept : ptr_(p) {}
    unique_ptr(pointer p, D d) noexcept : ptr_(p), del_(r2::move(d)) {}

    unique_ptr(unique_ptr &&other) noexcept : ptr_(other.release()), del_(r2::move(other.del_)) {}

    template <class U, class E>
    unique_ptr(unique_ptr<U, E> &&other) noexcept : ptr_(other.release()) {}

    unique_ptr(const unique_ptr &) = delete;
    unique_ptr &operator=(const unique_ptr &) = delete;

    unique_ptr &operator=(unique_ptr &&other) noexcept {
        if (this != &other) {
            reset(other.release());
            del_ = r2::move(other.del_);
        }
        return *this;
    }

    unique_ptr &operator=(nullptr_t) noexcept {
        reset();
        return *this;
    }

    ~unique_ptr() { reset(); }

    pointer get() const noexcept { return ptr_; }
    deleter_type &get_deleter() noexcept { return del_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T &operator*() const { return *ptr_; }
    pointer operator->() const noexcept { return ptr_; }

    pointer release() noexcept { return exchange(ptr_, nullptr); }

    void reset(pointer p = nullptr) noexcept {
        pointer old = exchange(ptr_, p);
        if (old)
            del_(old);
    }

    void swap(unique_ptr &other) noexcept {
        r2::swap(ptr_, other.ptr_);
        r2::swap(del_, other.del_);
    }

  private:
    pointer ptr_;
    [[no_unique_address]] D del_{};
};

template <class T, class D> class unique_ptr<T[], D> {
  public:
    using pointer = T *;
    using element_type = T;

    constexpr unique_ptr() noexcept : ptr_(nullptr) {}
    constexpr unique_ptr(nullptr_t) noexcept : ptr_(nullptr) {}
    explicit unique_ptr(pointer p) noexcept : ptr_(p) {}

    unique_ptr(unique_ptr &&other) noexcept : ptr_(other.release()) {}
    unique_ptr(const unique_ptr &) = delete;
    unique_ptr &operator=(const unique_ptr &) = delete;

    unique_ptr &operator=(unique_ptr &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    ~unique_ptr() { reset(); }

    pointer get() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T &operator[](size_t i) const { return ptr_[i]; }

    pointer release() noexcept { return exchange(ptr_, nullptr); }

    void reset(pointer p = nullptr) noexcept {
        pointer old = exchange(ptr_, p);
        if (old)
            D{}(old);
    }

  private:
    pointer ptr_;
};

template <class T, class U, class D1, class D2>
bool operator==(const unique_ptr<T, D1> &a, const unique_ptr<U, D2> &b) {
    return a.get() == b.get();
}
template <class T, class D> bool operator==(const unique_ptr<T, D> &a, nullptr_t) {
    return a.get() == nullptr;
}
template <class T, class D> bool operator!=(const unique_ptr<T, D> &a, nullptr_t) {
    return a.get() != nullptr;
}

/*
 *  make_unique returns an empty unique_ptr when the heap is exhausted:
 *  operator new returns nullptr here rather than throwing, so the caller has
 *  to check, exactly as it would after a malloc.
 */
template <class T, class... Args> unique_ptr<T> make_unique(Args &&...args) {
    void *raw = ::operator new(sizeof(T), std::nothrow);
    if (!raw)
        return unique_ptr<T>();
    return unique_ptr<T>(::new (raw) T(forward<Args>(args)...));
}

/*
 *  make_unique_array<T>(n) --- an owned T[n].  The new-expression is the real
 *  one, so the compiler lays down the array cookie that the matching delete[]
 *  in unique_ptr<T[]> expects.
 */
template <class T> unique_ptr<T[]> make_unique_array(size_t n) {
    return unique_ptr<T[]>(::new (std::nothrow) T[n]());
}

} // namespace r2

#endif
