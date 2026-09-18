#ifndef _R2CXX_OPTIONAL_HPP_
#define _R2CXX_OPTIONAL_HPP_

/*
 *  optional.hpp — a value that may be absent.
 *
 *  Without exceptions, "it failed" has to travel in the return value, and an
 *  optional says so without a sentinel that the caller might forget to check.
 */

#include "memory.hpp"

namespace r2 {

struct nullopt_t {
    explicit constexpr nullopt_t(int) {}
};
inline constexpr nullopt_t nullopt{0};

template <class T> class optional {
  public:
    constexpr optional() noexcept : engaged_(false) {}
    constexpr optional(nullopt_t) noexcept : engaged_(false) {}

    optional(const T &value) : engaged_(true) { construct_at(ptr(), value); }
    optional(T &&value) : engaged_(true) { construct_at(ptr(), r2::move(value)); }

    optional(const optional &other) : engaged_(other.engaged_) {
        if (engaged_)
            construct_at(ptr(), *other.ptr());
    }

    optional(optional &&other) : engaged_(other.engaged_) {
        if (engaged_) {
            construct_at(ptr(), r2::move(*other.ptr()));
            other.reset();
        }
    }

    ~optional() { reset(); }

    optional &operator=(const optional &other) {
        if (this != &other) {
            reset();
            if (other.engaged_) {
                construct_at(ptr(), *other.ptr());
                engaged_ = true;
            }
        }
        return *this;
    }

    optional &operator=(optional &&other) {
        if (this != &other) {
            reset();
            if (other.engaged_) {
                construct_at(ptr(), r2::move(*other.ptr()));
                engaged_ = true;
                other.reset();
            }
        }
        return *this;
    }

    optional &operator=(nullopt_t) {
        reset();
        return *this;
    }

    template <class... Args> T &emplace(Args &&...args) {
        reset();
        construct_at(ptr(), forward<Args>(args)...);
        engaged_ = true;
        return *ptr();
    }

    bool has_value() const noexcept { return engaged_; }
    explicit operator bool() const noexcept { return engaged_; }

    T &operator*() { return *ptr(); }
    const T &operator*() const { return *ptr(); }
    T *operator->() { return ptr(); }
    const T *operator->() const { return ptr(); }

    T value_or(T fallback) const { return engaged_ ? *ptr() : fallback; }

    void reset() {
        if (engaged_) {
            destroy_at(ptr());
            engaged_ = false;
        }
    }

  private:
    T *ptr() { return reinterpret_cast<T *>(storage_); }
    const T *ptr() const { return reinterpret_cast<const T *>(storage_); }

    alignas(T) unsigned char storage_[sizeof(T)];
    bool engaged_;
};

} // namespace r2

#endif
