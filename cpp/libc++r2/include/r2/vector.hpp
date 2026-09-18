#ifndef _R2CXX_VECTOR_HPP_
#define _R2CXX_VECTOR_HPP_

/*
 *  vector.hpp — a growable array.
 *
 *  Deliberately not std::vector in one respect: every operation that has to
 *  allocate returns whether it succeeded, because there are no exceptions here
 *  and the arena this allocates from is a few hundred kilobytes.  A program
 *  that ignores the return value behaves like one that ignores a malloc
 *  result --- the container stays unchanged and valid, it just did not grow.
 */

#include "initializer_list.hpp"
#include "memory.hpp"
#include "span.hpp"

namespace r2 {

template <class T> class vector {
  public:
    using value_type = T;
    using iterator = T *;
    using const_iterator = const T *;

    vector() noexcept : data_(nullptr), size_(0), cap_(0) {}

    explicit vector(size_t count) : vector() {
        if (reserve(count)) {
            uninitialized_default_construct(data_, data_ + count);
            size_ = count;
        }
    }

    vector(size_t count, const T &value) : vector() {
        if (reserve(count)) {
            uninitialized_fill(data_, data_ + count, value);
            size_ = count;
        }
    }

    vector(std::initializer_list<T> il) : vector() {
        if (reserve(il.size())) {
            uninitialized_copy(il.begin(), il.end(), data_);
            size_ = il.size();
        }
    }

    vector(const vector &other) : vector() {
        if (reserve(other.size_)) {
            uninitialized_copy(other.data_, other.data_ + other.size_, data_);
            size_ = other.size_;
        }
    }

    vector(vector &&other) noexcept
        : data_(other.data_), size_(other.size_), cap_(other.cap_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.cap_ = 0;
    }

    ~vector() {
        destroy(data_, data_ + size_);
        ::operator delete(data_);
    }

    vector &operator=(const vector &other) {
        if (this == &other)
            return *this;
        clear();
        if (reserve(other.size_)) {
            uninitialized_copy(other.data_, other.data_ + other.size_, data_);
            size_ = other.size_;
        }
        return *this;
    }

    vector &operator=(vector &&other) noexcept {
        if (this == &other)
            return *this;
        destroy(data_, data_ + size_);
        ::operator delete(data_);
        data_ = other.data_;
        size_ = other.size_;
        cap_ = other.cap_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.cap_ = 0;
        return *this;
    }

    T &operator[](size_t i) { return data_[i]; }
    const T &operator[](size_t i) const { return data_[i]; }

    /*  Bounds-checked access: nullptr rather than a thrown exception.  */
    T *at(size_t i) { return i < size_ ? data_ + i : nullptr; }
    const T *at(size_t i) const { return i < size_ ? data_ + i : nullptr; }

    T &front() { return data_[0]; }
    const T &front() const { return data_[0]; }
    T &back() { return data_[size_ - 1]; }
    const T &back() const { return data_[size_ - 1]; }

    T *data() noexcept { return data_; }
    const T *data() const noexcept { return data_; }

    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return cap_; }
    bool empty() const noexcept { return size_ == 0; }

    iterator begin() noexcept { return data_; }
    iterator end() noexcept { return data_ + size_; }
    const_iterator begin() const noexcept { return data_; }
    const_iterator end() const noexcept { return data_ + size_; }

    operator span<T>() noexcept { return span<T>(data_, size_); }
    span<T> as_span() noexcept { return span<T>(data_, size_); }
    span<const T> as_span() const noexcept { return span<const T>(data_, size_); }

    [[nodiscard]] bool reserve(size_t new_cap) {
        if (new_cap <= cap_)
            return true;

        void *raw = ::operator new(new_cap * sizeof(T), std::nothrow);
        if (!raw)
            return false;

        T *fresh = static_cast<T *>(raw);
        uninitialized_move(data_, data_ + size_, fresh);
        destroy(data_, data_ + size_);
        ::operator delete(data_);

        data_ = fresh;
        cap_ = new_cap;
        return true;
    }

    [[nodiscard]] bool push_back(const T &value) {
        if (!grow_if_full())
            return false;
        construct_at(data_ + size_, value);
        size_++;
        return true;
    }

    [[nodiscard]] bool push_back(T &&value) {
        if (!grow_if_full())
            return false;
        construct_at(data_ + size_, r2::move(value));
        size_++;
        return true;
    }

    /*  Returns a pointer to the new element, or nullptr if it could not grow. */
    template <class... Args> T *emplace_back(Args &&...args) {
        if (!grow_if_full())
            return nullptr;
        T *slot = construct_at(data_ + size_, forward<Args>(args)...);
        size_++;
        return slot;
    }

    void pop_back() {
        if (size_ > 0) {
            size_--;
            destroy_at(data_ + size_);
        }
    }

    [[nodiscard]] bool resize(size_t count) {
        if (count < size_) {
            destroy(data_ + count, data_ + size_);
            size_ = count;
            return true;
        }
        if (!reserve(count))
            return false;
        uninitialized_default_construct(data_ + size_, data_ + count);
        size_ = count;
        return true;
    }

    [[nodiscard]] bool resize(size_t count, const T &value) {
        if (count < size_) {
            destroy(data_ + count, data_ + size_);
            size_ = count;
            return true;
        }
        if (!reserve(count))
            return false;
        uninitialized_fill(data_ + size_, data_ + count, value);
        size_ = count;
        return true;
    }

    /*  Removes [pos, pos+1); returns an iterator to the element that followed. */
    iterator erase(iterator pos) {
        if (pos < data_ || pos >= data_ + size_)
            return end();
        for (iterator it = pos; it + 1 != end(); ++it)
            *it = r2::move(*(it + 1));
        size_--;
        destroy_at(data_ + size_);
        return pos;
    }

    iterator erase(iterator first, iterator last) {
        if (first >= last)
            return last;
        size_t count = (size_t)(last - first);
        for (iterator it = first; it + count != end(); ++it)
            *it = r2::move(*(it + count));
        destroy(data_ + size_ - count, data_ + size_);
        size_ -= count;
        return first;
    }

    /*  Inserts before pos; returns the inserted element, or nullptr on OOM.  */
    T *insert(iterator pos, const T &value) {
        size_t index = (size_t)(pos - data_);
        if (index > size_)
            return nullptr;
        if (!grow_if_full())
            return nullptr;
        construct_at(data_ + size_);
        for (size_t i = size_; i > index; i--)
            data_[i] = r2::move(data_[i - 1]);
        data_[index] = value;
        size_++;
        return data_ + index;
    }

    void clear() {
        destroy(data_, data_ + size_);
        size_ = 0;
    }

    void swap(vector &other) noexcept {
        r2::swap(data_, other.data_);
        r2::swap(size_, other.size_);
        r2::swap(cap_, other.cap_);
    }

  private:
    bool grow_if_full() {
        if (size_ < cap_)
            return true;
        /*  Doubling, starting at 4: the arena is small, so a big first block
         *  is worse than two reallocations.  */
        return reserve(cap_ == 0 ? 4 : cap_ * 2);
    }

    T *data_;
    size_t size_;
    size_t cap_;
};

template <class T> bool operator==(const vector<T> &a, const vector<T> &b) {
    if (a.size() != b.size())
        return false;
    return equal(a.begin(), a.end(), b.begin());
}

template <class T> bool operator!=(const vector<T> &a, const vector<T> &b) { return !(a == b); }

} // namespace r2

#endif
