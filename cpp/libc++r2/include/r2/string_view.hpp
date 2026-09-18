#ifndef _R2CXX_STRING_VIEW_HPP_
#define _R2CXX_STRING_VIEW_HPP_

/*
 *  string_view.hpp — a non-owning view of a character range.
 *
 *  Note that a string_view is not NUL-terminated in general, so it cannot be
 *  handed to a syscall that expects a C string.  r2::string::c_str() is the
 *  way across that boundary.
 */

#include "span.hpp"

namespace r2 {

namespace detail {
constexpr size_t cstr_len(const char *s) {
    size_t n = 0;
    if (s)
        while (s[n])
            n++;
    return n;
}
} // namespace detail

class string_view {
  public:
    using iterator = const char *;

    constexpr string_view() noexcept : ptr_(nullptr), len_(0) {}
    constexpr string_view(const char *s) : ptr_(s), len_(detail::cstr_len(s)) {}
    constexpr string_view(const char *s, size_t len) : ptr_(s), len_(len) {}
    string_view(const uint8_t *s) : ptr_((const char *)s), len_(detail::cstr_len((const char *)s)) {}

    constexpr const char *data() const noexcept { return ptr_; }
    constexpr size_t size() const noexcept { return len_; }
    constexpr size_t length() const noexcept { return len_; }
    constexpr bool empty() const noexcept { return len_ == 0; }

    constexpr char operator[](size_t i) const { return ptr_[i]; }
    constexpr char front() const { return ptr_[0]; }
    constexpr char back() const { return ptr_[len_ - 1]; }

    constexpr iterator begin() const noexcept { return ptr_; }
    constexpr iterator end() const noexcept { return ptr_ + len_; }

    const uint8_t *bytes() const noexcept { return (const uint8_t *)ptr_; }

    constexpr string_view substr(size_t pos, size_t count = npos) const {
        if (pos > len_)
            return string_view();
        size_t n = (count == npos || pos + count > len_) ? len_ - pos : count;
        return string_view(ptr_ + pos, n);
    }

    constexpr void remove_prefix(size_t n) {
        if (n > len_)
            n = len_;
        ptr_ += n;
        len_ -= n;
    }

    constexpr void remove_suffix(size_t n) { len_ -= (n > len_) ? len_ : n; }

    constexpr int compare(string_view other) const {
        size_t n = len_ < other.len_ ? len_ : other.len_;
        for (size_t i = 0; i < n; i++) {
            unsigned char a = (unsigned char)ptr_[i];
            unsigned char b = (unsigned char)other.ptr_[i];
            if (a != b)
                return a < b ? -1 : 1;
        }
        if (len_ == other.len_)
            return 0;
        return len_ < other.len_ ? -1 : 1;
    }

    constexpr bool starts_with(string_view prefix) const {
        return len_ >= prefix.len_ && substr(0, prefix.len_).compare(prefix) == 0;
    }

    constexpr bool ends_with(string_view suffix) const {
        return len_ >= suffix.len_ && substr(len_ - suffix.len_).compare(suffix) == 0;
    }

    constexpr size_t find(char c, size_t pos = 0) const {
        for (size_t i = pos; i < len_; i++)
            if (ptr_[i] == c)
                return i;
        return npos;
    }

    constexpr size_t rfind(char c) const {
        for (size_t i = len_; i > 0; i--)
            if (ptr_[i - 1] == c)
                return i - 1;
        return npos;
    }

    constexpr size_t find(string_view needle, size_t pos = 0) const {
        if (needle.len_ == 0)
            return pos <= len_ ? pos : npos;
        if (needle.len_ > len_)
            return npos;
        for (size_t i = pos; i + needle.len_ <= len_; i++)
            if (substr(i, needle.len_).compare(needle) == 0)
                return i;
        return npos;
    }

    constexpr bool contains(string_view needle) const { return find(needle) != npos; }

    /*  Splits at the first occurrence of sep.  Returns {before, after}; when
     *  sep is absent the whole view is `first` and `second` is empty.  */
    constexpr pair<string_view, string_view> split(char sep) const {
        size_t at = find(sep);
        if (at == npos)
            return {*this, string_view()};
        return {substr(0, at), substr(at + 1)};
    }

    constexpr string_view trim() const {
        size_t b = 0;
        size_t e = len_;
        auto is_space = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
        };
        while (b < e && is_space(ptr_[b]))
            b++;
        while (e > b && is_space(ptr_[e - 1]))
            e--;
        return string_view(ptr_ + b, e - b);
    }

  private:
    const char *ptr_;
    size_t len_;
};

constexpr bool operator==(string_view a, string_view b) { return a.compare(b) == 0; }
constexpr bool operator!=(string_view a, string_view b) { return a.compare(b) != 0; }
constexpr bool operator<(string_view a, string_view b) { return a.compare(b) < 0; }
constexpr bool operator>(string_view a, string_view b) { return a.compare(b) > 0; }

namespace literals {
constexpr string_view operator"" _sv(const char *s, size_t len) { return string_view(s, len); }
}

} // namespace r2

#endif
