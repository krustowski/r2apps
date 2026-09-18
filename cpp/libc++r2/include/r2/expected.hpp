#ifndef _R2CXX_EXPECTED_HPP_
#define _R2CXX_EXPECTED_HPP_

/*
 *  expected.hpp — a value, or the reason there isn't one.
 *
 *  C++23's vocabulary type for fallible functions, and the natural fit for a
 *  library built without exceptions: r2::optional says something failed,
 *  expected says what failed.
 *
 *      r2::expected<r2::string, r2::SysError> load(r2::string_view path) {
 *          auto text = r2::fs::read_text(path);
 *          if (!text)
 *              return r2::unexpected(r2::SysError::FileNotFound);
 *          return *text;
 *      }
 *
 *      if (auto config = load("/GARN.CFG"))
 *          use(*config);
 *      else
 *          r2::println("load failed: ", (int)config.error());
 *
 *  The monadic operations of C++23 are here too --- and_then, transform,
 *  or_else, transform_error --- so a chain of fallible steps reads as one
 *  expression rather than a ladder of ifs.
 *
 *  value() on an unexpected object is a programming error, not a runtime one:
 *  there is no exception to throw, so it panics.  Check first, or use
 *  value_or().
 */

#include "memory.hpp"
#include "panic.hpp"
#include "utility.hpp"

namespace r2 {

template <class E> class unexpected {
  public:
    constexpr explicit unexpected(const E &error) : error_(error) {}
    constexpr explicit unexpected(E &&error) : error_(r2::move(error)) {}

    constexpr const E &error() const &noexcept { return error_; }
    constexpr E &error() &noexcept { return error_; }
    constexpr E &&error() &&noexcept { return r2::move(error_); }

  private:
    E error_;
};

/*  Deduction guide, so `return unexpected(err);` works without spelling E.  */
template <class E> unexpected(E) -> unexpected<E>;

template <class T, class E> class expected {
  public:
    using value_type = T;
    using error_type = E;

    constexpr expected() : has_value_(true) { construct_at(value_ptr()); }

    constexpr expected(const T &value) : has_value_(true) { construct_at(value_ptr(), value); }
    constexpr expected(T &&value) : has_value_(true) {
        construct_at(value_ptr(), r2::move(value));
    }

    constexpr expected(const unexpected<E> &error) : has_value_(false) {
        construct_at(error_ptr(), error.error());
    }
    constexpr expected(unexpected<E> &&error) : has_value_(false) {
        construct_at(error_ptr(), r2::move(error).error());
    }

    constexpr expected(const expected &other) : has_value_(other.has_value_) {
        if (has_value_)
            construct_at(value_ptr(), *other.value_ptr());
        else
            construct_at(error_ptr(), *other.error_ptr());
    }

    constexpr expected(expected &&other) : has_value_(other.has_value_) {
        if (has_value_)
            construct_at(value_ptr(), r2::move(*other.value_ptr()));
        else
            construct_at(error_ptr(), r2::move(*other.error_ptr()));
    }

    ~expected() { destroy_current(); }

    constexpr expected &operator=(const expected &other) {
        if (this != &other) {
            destroy_current();
            has_value_ = other.has_value_;
            if (has_value_)
                construct_at(value_ptr(), *other.value_ptr());
            else
                construct_at(error_ptr(), *other.error_ptr());
        }
        return *this;
    }

    constexpr expected &operator=(expected &&other) {
        if (this != &other) {
            destroy_current();
            has_value_ = other.has_value_;
            if (has_value_)
                construct_at(value_ptr(), r2::move(*other.value_ptr()));
            else
                construct_at(error_ptr(), r2::move(*other.error_ptr()));
        }
        return *this;
    }

    constexpr bool has_value() const noexcept { return has_value_; }
    constexpr explicit operator bool() const noexcept { return has_value_; }

    constexpr T &operator*() & noexcept { return *value_ptr(); }
    constexpr const T &operator*() const &noexcept { return *value_ptr(); }
    constexpr T *operator->() noexcept { return value_ptr(); }
    constexpr const T *operator->() const noexcept { return value_ptr(); }

    /*  Panics when there is no value; there is no exception to throw here.  */
    constexpr T &value() & {
        if (!has_value_)
            panic(string_view("expected::value() on an unexpected result"));
        return *value_ptr();
    }

    constexpr const T &value() const & {
        if (!has_value_)
            panic(string_view("expected::value() on an unexpected result"));
        return *value_ptr();
    }

    constexpr E &error() &noexcept { return *error_ptr(); }
    constexpr const E &error() const &noexcept { return *error_ptr(); }

    template <class U> constexpr T value_or(U &&fallback) const & {
        return has_value_ ? *value_ptr() : static_cast<T>(forward<U>(fallback));
    }

    /*  f(T) -> expected<U, E>  */
    template <class F> constexpr auto and_then(F &&f) const & {
        using Result = decltype(f(*value_ptr()));
        return has_value_ ? f(*value_ptr()) : Result(unexpected<E>(*error_ptr()));
    }

    /*  f(T) -> U, wrapped back into expected<U, E>  */
    template <class F> constexpr auto transform(F &&f) const & {
        using U = decltype(f(*value_ptr()));
        using Result = expected<U, E>;
        return has_value_ ? Result(f(*value_ptr())) : Result(unexpected<E>(*error_ptr()));
    }

    /*  f(E) -> expected<T, G>, the recovery path  */
    template <class F> constexpr auto or_else(F &&f) const & {
        using Result = decltype(f(*error_ptr()));
        return has_value_ ? Result(*value_ptr()) : f(*error_ptr());
    }

    /*  f(E) -> G, for translating one error type into another  */
    template <class F> constexpr auto transform_error(F &&f) const & {
        using G = decltype(f(*error_ptr()));
        using Result = expected<T, G>;
        return has_value_ ? Result(*value_ptr()) : Result(unexpected<G>(f(*error_ptr())));
    }

  private:
    T *value_ptr() { return reinterpret_cast<T *>(storage_); }
    const T *value_ptr() const { return reinterpret_cast<const T *>(storage_); }
    E *error_ptr() { return reinterpret_cast<E *>(storage_); }
    const E *error_ptr() const { return reinterpret_cast<const E *>(storage_); }

    void destroy_current() {
        if (has_value_)
            destroy_at(value_ptr());
        else
            destroy_at(error_ptr());
    }

    static constexpr size_t STORAGE_SIZE = sizeof(T) > sizeof(E) ? sizeof(T) : sizeof(E);
    static constexpr size_t STORAGE_ALIGN = alignof(T) > alignof(E) ? alignof(T) : alignof(E);

    alignas(STORAGE_ALIGN) unsigned char storage_[STORAGE_SIZE];
    bool has_value_;
};

/*
 *  expected<void, E> — for an operation that either worked or did not, which
 *  is most of the filesystem and networking API.
 */
template <class E> class expected<void, E> {
  public:
    using value_type = void;
    using error_type = E;

    constexpr expected() noexcept : has_value_(true) {}

    constexpr expected(const unexpected<E> &error) : has_value_(false) {
        construct_at(error_ptr(), error.error());
    }
    constexpr expected(unexpected<E> &&error) : has_value_(false) {
        construct_at(error_ptr(), r2::move(error).error());
    }

    constexpr expected(const expected &other) : has_value_(other.has_value_) {
        if (!has_value_)
            construct_at(error_ptr(), *other.error_ptr());
    }

    ~expected() {
        if (!has_value_)
            destroy_at(error_ptr());
    }

    constexpr expected &operator=(const expected &other) {
        if (this != &other) {
            if (!has_value_)
                destroy_at(error_ptr());
            has_value_ = other.has_value_;
            if (!has_value_)
                construct_at(error_ptr(), *other.error_ptr());
        }
        return *this;
    }

    constexpr bool has_value() const noexcept { return has_value_; }
    constexpr explicit operator bool() const noexcept { return has_value_; }

    constexpr void value() const {
        if (!has_value_)
            panic(string_view("expected<void>::value() on an unexpected result"));
    }

    constexpr E &error() &noexcept { return *error_ptr(); }
    constexpr const E &error() const &noexcept { return *error_ptr(); }

    template <class F> constexpr auto and_then(F &&f) const {
        using Result = decltype(f());
        return has_value_ ? f() : Result(unexpected<E>(*error_ptr()));
    }

    template <class F> constexpr auto or_else(F &&f) const {
        using Result = decltype(f(*error_ptr()));
        return has_value_ ? Result() : f(*error_ptr());
    }

  private:
    E *error_ptr() { return reinterpret_cast<E *>(storage_); }
    const E *error_ptr() const { return reinterpret_cast<const E *>(storage_); }

    alignas(E) unsigned char storage_[sizeof(E)];
    bool has_value_;
};

template <class T, class E>
constexpr bool operator==(const expected<T, E> &a, const expected<T, E> &b) {
    if (a.has_value() != b.has_value())
        return false;
    return a.has_value() ? (*a == *b) : (a.error() == b.error());
}

} // namespace r2

#endif
