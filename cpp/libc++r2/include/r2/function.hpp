#ifndef _R2CXX_FUNCTION_HPP_
#define _R2CXX_FUNCTION_HPP_

/*
 *  function.hpp — a callable holder for callbacks.
 *
 *  Storage is inline and fixed: a lambda capturing more than INLINE_STORAGE
 *  bytes will not compile rather than silently reaching for the heap, which
 *  keeps event handlers out of the arena and makes the cost visible where the
 *  callback is written.  Capture a pointer to your state if you need more.
 */

#include "memory.hpp"

namespace r2 {

template <class Signature> class function;

template <class R, class... Args> class function<R(Args...)> {
  public:
    static constexpr size_t INLINE_STORAGE = 32;

    function() noexcept : vtable_(nullptr) {}
    function(nullptr_t) noexcept : vtable_(nullptr) {}

    template <class F> function(F callable) {
        static_assert(sizeof(F) <= INLINE_STORAGE,
                      "callable is too large for r2::function; capture a pointer instead");
        static_assert(alignof(F) <= alignof(void *) * 2, "callable is over-aligned");
        ::new ((void *)storage_) F(r2::move(callable));
        vtable_ = &vtable_for<F>;
    }

    function(const function &other) : vtable_(other.vtable_) {
        if (vtable_)
            vtable_->copy(storage_, other.storage_);
    }

    function(function &&other) : vtable_(other.vtable_) {
        if (vtable_) {
            vtable_->copy(storage_, other.storage_);
            other.reset();
        }
    }

    ~function() { reset(); }

    function &operator=(const function &other) {
        if (this != &other) {
            reset();
            vtable_ = other.vtable_;
            if (vtable_)
                vtable_->copy(storage_, other.storage_);
        }
        return *this;
    }

    function &operator=(function &&other) {
        if (this != &other) {
            reset();
            vtable_ = other.vtable_;
            if (vtable_) {
                vtable_->copy(storage_, other.storage_);
                other.reset();
            }
        }
        return *this;
    }

    explicit operator bool() const noexcept { return vtable_ != nullptr; }

    R operator()(Args... args) const {
        return vtable_->invoke(const_cast<unsigned char *>(storage_), forward<Args>(args)...);
    }

    void reset() {
        if (vtable_) {
            vtable_->destroy(storage_);
            vtable_ = nullptr;
        }
    }

  private:
    struct vtable {
        R (*invoke)(void *, Args...);
        void (*copy)(void *, const void *);
        void (*destroy)(void *);
    };

    template <class F> static R invoke_impl(void *self, Args... args) {
        return (*static_cast<F *>(self))(forward<Args>(args)...);
    }

    template <class F> static void copy_impl(void *dst, const void *src) {
        ::new (dst) F(*static_cast<const F *>(src));
    }

    template <class F> static void destroy_impl(void *self) { static_cast<F *>(self)->~F(); }

    template <class F>
    static constexpr vtable vtable_for = {&invoke_impl<F>, &copy_impl<F>, &destroy_impl<F>};

    alignas(void *) unsigned char storage_[INLINE_STORAGE];
    const vtable *vtable_;
};

} // namespace r2

#endif
