#ifndef _R2CXX_COROUTINE_HPP_
#define _R2CXX_COROUTINE_HPP_

/*
 *  coroutine.hpp — the library half of coroutines, plus a generator built on
 *  top of it.
 *
 *  `co_yield` and `co_await` are language features, but the compiler looks up
 *  std::coroutine_traits and std::coroutine_handle by name to make them work,
 *  so a freestanding implementation has to provide both.  The handle is a thin
 *  wrapper over the __builtin_coro_* intrinsics.
 *
 *  A coroutine frame is heap-allocated, and on r2 an allocation can simply
 *  fail.  Every promise type here defines get_return_object_on_allocation_failure,
 *  which makes the compiler use the nothrow operator new and hand back an empty
 *  object instead of dereferencing null --- so a generator that could not be
 *  created reports itself as done rather than crashing.
 *
 *      r2::generator<int> squares(int upto) {
 *          for (int i = 1; i <= upto; i++)
 *              co_yield i * i;
 *      }
 *
 *      for (int value : squares(5))
 *          r2::println(value);
 *
 *  C++20 and later only; built as C++17 this header is empty.
 */

#include "types.hpp"

#if __cplusplus >= 202002L

#include "new.hpp"
#include "utility.hpp"

namespace std {

template <class Promise = void> struct coroutine_handle;

template <> struct coroutine_handle<void> {
    constexpr coroutine_handle() noexcept : frame_(nullptr) {}
    constexpr coroutine_handle(decltype(nullptr)) noexcept : frame_(nullptr) {}

    static constexpr coroutine_handle from_address(void *address) noexcept {
        coroutine_handle handle;
        handle.frame_ = address;
        return handle;
    }

    constexpr void *address() const noexcept { return frame_; }
    constexpr explicit operator bool() const noexcept { return frame_ != nullptr; }

    bool done() const { return __builtin_coro_done(frame_); }
    void resume() const { __builtin_coro_resume(frame_); }
    void destroy() const { __builtin_coro_destroy(frame_); }
    void operator()() const { resume(); }

    friend constexpr bool operator==(coroutine_handle a, coroutine_handle b) noexcept {
        return a.frame_ == b.frame_;
    }

  protected:
    void *frame_;
};

template <class Promise> struct coroutine_handle : coroutine_handle<void> {
    using coroutine_handle<void>::coroutine_handle;

    static coroutine_handle from_promise(Promise &promise) noexcept {
        coroutine_handle handle;
        handle.frame_ = __builtin_coro_promise((char *)&promise, alignof(Promise), true);
        return handle;
    }

    static constexpr coroutine_handle from_address(void *address) noexcept {
        coroutine_handle handle;
        handle.frame_ = address;
        return handle;
    }

    Promise &promise() const {
        return *(Promise *)__builtin_coro_promise(frame_, alignof(Promise), false);
    }
};

/*  The compiler asks this for the promise type of a coroutine.  */
template <class Result, class...> struct coroutine_traits {
    using promise_type = typename Result::promise_type;
};

struct suspend_always {
    constexpr bool await_ready() const noexcept { return false; }
    constexpr void await_suspend(coroutine_handle<>) const noexcept {}
    constexpr void await_resume() const noexcept {}
};

struct suspend_never {
    constexpr bool await_ready() const noexcept { return true; }
    constexpr void await_suspend(coroutine_handle<>) const noexcept {}
    constexpr void await_resume() const noexcept {}
};

} // namespace std

namespace r2 {

/*
 *  generator<T> — a lazy sequence.  The coroutine runs only as far as the next
 *  value each time the iterator is advanced, so a generator over a directory
 *  or a file costs one element of memory rather than a whole vector.
 */
template <class T> class generator {
  public:
    struct promise_type {
        T current{};
        bool failed = false;

        generator get_return_object() {
            return generator(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        /*  The arena can be full; this is what keeps that from being a crash. */
        static generator get_return_object_on_allocation_failure() { return generator(); }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        std::suspend_always yield_value(T value) {
            current = r2::move(value);
            return {};
        }

        void return_void() noexcept {}

        /*  Required by the coroutine machinery; unreachable with
         *  -fno-exceptions.  */
        void unhandled_exception() noexcept {}
    };

    using handle_type = std::coroutine_handle<promise_type>;

    generator() noexcept : handle_(nullptr) {}
    explicit generator(handle_type handle) noexcept : handle_(handle) {}

    generator(const generator &) = delete;
    generator &operator=(const generator &) = delete;

    generator(generator &&other) noexcept : handle_(other.handle_) {
        other.handle_ = handle_type(nullptr);
    }

    generator &operator=(generator &&other) noexcept {
        if (this != &other) {
            if (handle_)
                handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = handle_type(nullptr);
        }
        return *this;
    }

    ~generator() {
        if (handle_)
            handle_.destroy();
    }

    /*  False when the coroutine frame could not be allocated.  */
    explicit operator bool() const noexcept { return (bool)handle_; }

    class iterator {
      public:
        iterator() noexcept : handle_(nullptr) {}
        explicit iterator(handle_type handle) noexcept : handle_(handle) {}

        iterator &operator++() {
            handle_.resume();
            if (handle_.done())
                handle_ = handle_type(nullptr);
            return *this;
        }

        const T &operator*() const { return handle_.promise().current; }
        const T *operator->() const { return &handle_.promise().current; }

        bool operator==(const iterator &other) const noexcept {
            return handle_.address() == other.handle_.address();
        }

      private:
        handle_type handle_;
    };

    iterator begin() {
        if (!handle_)
            return iterator();

        handle_.resume();
        if (handle_.done())
            return iterator();

        return iterator(handle_);
    }

    iterator end() noexcept { return iterator(); }

    /*  Pull one value without the iterator dance; false when exhausted.  */
    bool next(T &out) {
        if (!handle_ || handle_.done())
            return false;

        handle_.resume();
        if (handle_.done())
            return false;

        out = handle_.promise().current;
        return true;
    }

  private:
    handle_type handle_;
};

} // namespace r2

#endif /*  __cplusplus >= 202002L  */

#endif
