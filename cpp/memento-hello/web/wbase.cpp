#include "wbase.h"

namespace web {

bool Buf::reserve(size_t want)
{
    if (failed)
        return false;
    if (want <= cap)
        return true;

    size_t n = cap ? cap : 256;
    while (n < want)
        n *= 2;

    void *p = big ? big_realloc(data, n) : web::realloc(data, n);
    if (!p)
    {
        failed = true;
        return false;
    }
    data = (uint8_t *)p;
    cap = n;
    return true;
}

bool Buf::append(const void *src, size_t n)
{
    if (!n)
        return !failed;
    if (!reserve(len + n + 1))
        return false;
    memcpy(data + len, src, n);
    len += n;
    return true;
}

void Buf::release()
{
    if (data)
    {
        if (big)
            big_free(data);
        else
            web::free(data);
    }
    data = nullptr;
    len = cap = 0;
    failed = false;
}

const char *Buf::cstr()
{
    if (!reserve(len + 1))
        return "";
    data[len] = 0;
    return (const char *)data;
}

void scopy(char *dst, const char *src, size_t cap)
{
    if (!cap)
        return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

void scopyn(char *dst, const char *src, size_t n, size_t cap)
{
    if (!cap)
        return;
    size_t i = 0;
    while (i < n && src[i] && i + 1 < cap)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

void scat(char *dst, const char *src, size_t cap)
{
    size_t l = strlen(dst);
    if (l < cap)
        scopy(dst + l, src, cap - l);
}

void scatInt(char *dst, long v, size_t cap)
{
    char tmp[24];
    int i = 0;
    bool neg = v < 0;
    unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
    do
    {
        tmp[i++] = (char)('0' + u % 10);
        u /= 10;
    } while (u && i < 22);
    if (neg)
        tmp[i++] = '-';
    char out[24];
    int j = 0;
    while (i)
        out[j++] = tmp[--i];
    out[j] = 0;
    scat(dst, out, cap);
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool ieq(const char *a, const char *b)
{
    while (*a && *b)
        if (lower(*a++) != lower(*b++))
            return false;
    return *a == *b;
}

bool ieqn(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (lower(a[i]) != lower(b[i]))
            return false;
        if (!a[i])
            return true;
    }
    return true;
}

bool istarts(const char *s, const char *prefix)
{
    while (*prefix)
        if (lower(*s++) != lower(*prefix++))
            return false;
    return true;
}

const char *ifind(const char *hay, size_t hayLen, const char *needle)
{
    size_t n = strlen(needle);
    if (!n || n > hayLen)
        return nullptr;
    for (size_t i = 0; i + n <= hayLen; i++)
        if (ieqn(hay + i, needle, n))
            return hay + i;
    return nullptr;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

} // namespace web
