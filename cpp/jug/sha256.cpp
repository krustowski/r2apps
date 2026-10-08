//
//  sha256.cpp --- SHA-256 (FIPS 180-4), for the sums in the server's list.
//
//  The same digest `sha256sum` prints, so a list can be made with that tool
//  as well as with mksums.py.
//

#include "jug.h"

#include <r2/libc.hpp>

namespace jug {

namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

} // namespace

bool Digest::operator==(const Digest &o) const { return memcmp(b, o.b, sizeof(b)) == 0; }

void Digest::hex(char *out, size_t digits) const
{
    static const char HEX[] = "0123456789abcdef";
    if (digits > 64)
        digits = 64;
    for (size_t i = 0; i < digits; i++)
        out[i] = HEX[(b[i / 2] >> (i % 2 ? 0 : 4)) & 15];
    out[digits] = 0;
}

bool Digest::parse(r2::string_view hex, Digest &out)
{
    if (hex.size() != 64)
        return false;
    for (size_t i = 0; i < 32; i++)
    {
        int hi = hexValue(hex[2 * i]), lo = hexValue(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.b[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

Sha256::Sha256()
{
    static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h_, H0, sizeof(h_));
}

void Sha256::compress(const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
    {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++)
    {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d;
    h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::update(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    total_ += n;
    if (used_)
    {
        size_t take = 64 - used_ < n ? 64 - used_ : n;
        memcpy(block_ + used_, p, take);
        used_ += take;
        p += take;
        n -= take;
        if (used_ < 64)
            return;
        compress(block_);
        used_ = 0;
    }
    for (; n >= 64; p += 64, n -= 64)
        compress(p);
    memcpy(block_, p, n);
    used_ = n;
}

Digest Sha256::finish()
{
    uint64_t bits = total_ * 8;
    block_[used_++] = 0x80;
    if (used_ > 56)
    {
        memset(block_ + used_, 0, 64 - used_);
        compress(block_);
        used_ = 0;
    }
    memset(block_ + used_, 0, 56 - used_);
    for (int i = 0; i < 8; i++)
        block_[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    compress(block_);

    Digest d;
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++)
            d.b[4 * i + k] = (uint8_t)(h_[i] >> (24 - 8 * k));
    return d;
}

Digest Sha256::of(const void *data, size_t n)
{
    Sha256 s;
    s.update(data, n);
    return s.finish();
}

} // namespace jug
