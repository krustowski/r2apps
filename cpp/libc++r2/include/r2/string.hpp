#ifndef _R2CXX_STRING_HPP_
#define _R2CXX_STRING_HPP_

/*
 *  string.hpp — an owning, always NUL-terminated character string.
 *
 *  Short strings (up to 23 characters) live inside the object and cost no
 *  allocation, which matters when the whole heap is an arena of a few hundred
 *  kilobytes and most strings are a path or a line of text.
 *
 *  Like r2::vector, the operations that allocate report failure by returning
 *  false rather than throwing; append and operator+= are the exceptions --- they
 *  keep the string unchanged and set the overflow flag, which failed() reports,
 *  so building a message with several appends stays readable.
 */

#include "new.hpp"
#include "string_view.hpp"

namespace r2 {

class string {
  public:
    static constexpr size_t SSO_CAPACITY = 23;

    string() noexcept { init_empty(); }

    string(const char *s) {
        init_empty();
        append(string_view(s));
    }

    string(string_view sv) {
        init_empty();
        append(sv);
    }

    string(const char *s, size_t len) {
        init_empty();
        append(string_view(s, len));
    }

    string(size_t count, char c) {
        init_empty();
        if (reserve(count)) {
            for (size_t i = 0; i < count; i++)
                data_[i] = c;
            size_ = count;
            data_[size_] = '\0';
        }
    }

    string(const string &other) {
        init_empty();
        append(other.view());
    }

    string(string &&other) noexcept {
        if (other.on_heap()) {
            data_ = other.data_;
            cap_ = other.cap_;
            size_ = other.size_;
            failed_ = other.failed_;
            other.init_empty();
        } else {
            init_empty();
            append(other.view());
        }
    }

    ~string() {
        if (on_heap())
            ::operator delete(data_);
    }

    string &operator=(const string &other) {
        if (this != &other) {
            clear();
            append(other.view());
        }
        return *this;
    }

    string &operator=(string &&other) noexcept {
        if (this == &other)
            return *this;
        if (on_heap())
            ::operator delete(data_);
        if (other.on_heap()) {
            data_ = other.data_;
            cap_ = other.cap_;
            size_ = other.size_;
            failed_ = other.failed_;
            other.init_empty();
        } else {
            init_empty();
            append(other.view());
        }
        return *this;
    }

    string &operator=(string_view sv) {
        clear();
        append(sv);
        return *this;
    }

    char *data() noexcept { return data_; }
    const char *data() const noexcept { return data_; }
    const char *c_str() const noexcept { return data_; }

    /*  The kernel ABI takes uint8_t*; this is the crossing point.  */
    const uint8_t *bytes() const noexcept { return (const uint8_t *)data_; }
    uint8_t *bytes() noexcept { return (uint8_t *)data_; }

    size_t size() const noexcept { return size_; }
    size_t length() const noexcept { return size_; }
    size_t capacity() const noexcept { return cap_; }
    bool empty() const noexcept { return size_ == 0; }

    /*  True when an append could not allocate; the string still holds
     *  everything that fitted before that point.  */
    bool failed() const noexcept { return failed_; }
    void clear_failed() noexcept { failed_ = false; }

    char &operator[](size_t i) { return data_[i]; }
    char operator[](size_t i) const { return data_[i]; }
    char front() const { return data_[0]; }
    char back() const { return data_[size_ - 1]; }

    char *begin() noexcept { return data_; }
    char *end() noexcept { return data_ + size_; }
    const char *begin() const noexcept { return data_; }
    const char *end() const noexcept { return data_ + size_; }

    string_view view() const noexcept { return string_view(data_, size_); }
    operator string_view() const noexcept { return view(); }

    [[nodiscard]] bool reserve(size_t new_cap);

    bool append(string_view sv);
    bool append(char c) { return append(string_view(&c, 1)); }
    bool append(const char *s) { return append(string_view(s)); }

    bool push_back(char c) { return append(c); }

    void pop_back() {
        if (size_ > 0) {
            size_--;
            data_[size_] = '\0';
        }
    }

    string &operator+=(string_view sv) {
        append(sv);
        return *this;
    }

    string &operator+=(char c) {
        append(c);
        return *this;
    }

    string &operator+=(const char *s) {
        append(string_view(s));
        return *this;
    }

    void clear() noexcept {
        size_ = 0;
        data_[0] = '\0';
        failed_ = false;
    }

    [[nodiscard]] bool resize(size_t count, char fill = '\0') {
        if (count <= size_) {
            size_ = count;
            data_[size_] = '\0';
            return true;
        }
        if (!reserve(count))
            return false;
        for (size_t i = size_; i < count; i++)
            data_[i] = fill;
        size_ = count;
        data_[size_] = '\0';
        return true;
    }

    string substr(size_t pos, size_t count = npos) const { return string(view().substr(pos, count)); }

    size_t find(char c, size_t pos = 0) const { return view().find(c, pos); }
    size_t find(string_view needle, size_t pos = 0) const { return view().find(needle, pos); }
    size_t rfind(char c) const { return view().rfind(c); }
    bool starts_with(string_view prefix) const { return view().starts_with(prefix); }
    bool ends_with(string_view suffix) const { return view().ends_with(suffix); }
    bool contains(string_view needle) const { return view().contains(needle); }
    int compare(string_view other) const { return view().compare(other); }

    void to_upper() {
        for (size_t i = 0; i < size_; i++)
            if (data_[i] >= 'a' && data_[i] <= 'z')
                data_[i] = (char)(data_[i] - 'a' + 'A');
    }

    void to_lower() {
        for (size_t i = 0; i < size_; i++)
            if (data_[i] >= 'A' && data_[i] <= 'Z')
                data_[i] = (char)(data_[i] - 'A' + 'a');
    }

    void swap(string &other) noexcept {
        string tmp = r2::move(*this);
        *this = r2::move(other);
        other = r2::move(tmp);
    }

  private:
    void init_empty() noexcept {
        data_ = sso_;
        cap_ = SSO_CAPACITY;
        size_ = 0;
        failed_ = false;
        sso_[0] = '\0';
    }

    bool on_heap() const noexcept { return data_ != sso_; }

    char *data_;
    size_t size_;
    size_t cap_; /*  usable characters, not counting the NUL  */
    bool failed_;
    char sso_[SSO_CAPACITY + 1];
};

inline bool operator==(const string &a, string_view b) { return a.view().compare(b) == 0; }
inline bool operator==(string_view a, const string &b) { return a.compare(b.view()) == 0; }
inline bool operator==(const string &a, const string &b) { return a.view().compare(b.view()) == 0; }
inline bool operator!=(const string &a, const string &b) { return !(a == b); }
inline bool operator!=(const string &a, string_view b) { return !(a == b); }
inline bool operator<(const string &a, const string &b) { return a.view().compare(b.view()) < 0; }
inline bool operator>(const string &a, const string &b) { return a.view().compare(b.view()) > 0; }

#if __cplusplus >= 202002L
inline std::strong_ordering operator<=>(const string &a, const string &b) {
    return a.view() <=> b.view();
}

inline std::strong_ordering operator<=>(const string &a, string_view b) { return a.view() <=> b; }
#endif

inline string operator+(string_view a, string_view b) {
    string out;
    if (out.reserve(a.size() + b.size())) {
        out.append(a);
        out.append(b);
    }
    return out;
}

inline string operator+(const string &a, string_view b) { return a.view() + b; }
inline string operator+(const string &a, const char *b) { return a.view() + string_view(b); }

/*
 *  Number formatting.  base is 2..16; width pads on the left with `pad`.
 */
string to_string(int64_t value, unsigned base = 10, size_t width = 0, char pad = ' ');
string to_string(uint64_t value, unsigned base = 10, size_t width = 0, char pad = ' ');
inline string to_string(int value, unsigned base = 10) { return to_string((int64_t)value, base); }
inline string to_string(unsigned value, unsigned base = 10) {
    return to_string((uint64_t)value, base);
}

/*  Fixed-point rendering of a double with `decimals` digits after the point.  */
string to_string(double value, unsigned decimals = 3);

/*
 *  Parsing.  Returns false and leaves `out` untouched when the text is not a
 *  number in the given base; trailing characters after the digits are an error.
 */
bool parse_int(string_view text, int64_t &out, unsigned base = 10);
bool parse_uint(string_view text, uint64_t &out, unsigned base = 10);

} // namespace r2

#endif
