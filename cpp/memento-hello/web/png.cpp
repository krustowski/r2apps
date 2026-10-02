#include "png.h"

namespace web {

namespace {

uint32_t crcTable[256];
bool crcReady = false;

uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crcReady)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crcTable[i] = v;
        }
        crcReady = true;
    }
    for (size_t i = 0; i < n; i++)
        c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void be32(Buf &b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    b.append(x, 4);
}

//  A chunk: length, type, data, and the CRC of type and data.
void chunk(Buf &out, const char *type, const uint8_t *data, size_t n)
{
    be32(out, (uint32_t)n);
    out.append(type, 4);
    if (n)
        out.append(data, n);
    uint32_t c = crc(0xFFFFFFFFu, (const uint8_t *)type, 4);
    c = crc(c, data, n);
    be32(out, c ^ 0xFFFFFFFFu);
}

//  Deflate with the fixed codes (RFC 1951, 3.2.6) and runs of one byte:
//  after a literal, the same byte again three or more times is a match at
//  distance 1.
class Deflater
{
public:
    explicit Deflater(Buf &out) : out_(out) { bits(1, 1), bits(1, 2); } // the last block, fixed codes

    void byte(uint8_t b)
    {
        adlerA_ = (adlerA_ + b) % 65521;
        adlerB_ = (adlerB_ + adlerA_) % 65521;
        if (have_ && b == last_ && run_ < 258)
        {
            run_++;
            return;
        }
        flushRun();
        literal(b);
        last_ = b;
        have_ = true;
    }

    void finish()
    {
        flushRun();
        symbol(256);
        if (nbits_)
            out_.push((uint8_t)acc_);
        acc_ = nbits_ = 0;
    }

    uint32_t adler() const { return (adlerB_ << 16) | adlerA_; }

private:
    Buf &out_;
    uint32_t acc_ = 0;
    int nbits_ = 0;
    uint8_t last_ = 0;
    bool have_ = false;
    int run_ = 0;
    uint32_t adlerA_ = 1, adlerB_ = 0;

    //  n bits of v, least significant first, as deflate packs everything.
    void bits(uint32_t v, int n)
    {
        acc_ |= v << nbits_;
        nbits_ += n;
        while (nbits_ >= 8)
        {
            out_.push((uint8_t)acc_);
            acc_ >>= 8;
            nbits_ -= 8;
        }
    }

    //  A Huffman code goes out most significant bit first.
    void code(uint32_t c, int n)
    {
        uint32_t r = 0;
        for (int i = 0; i < n; i++)
            r |= ((c >> i) & 1) << (n - 1 - i);
        bits(r, n);
    }

    void symbol(int s)
    {
        if (s < 144)
            code(0x30 + s, 8);
        else if (s < 256)
            code(0x190 + (s - 144), 9);
        else if (s < 280)
            code(s - 256, 7);
        else
            code(0xC0 + (s - 280), 8);
    }

    void literal(uint8_t b) { symbol(b); }

    void match(int len)
    {
        static const uint16_t base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        int i = 28;
        while (base[i] > len)
            i--;
        symbol(257 + i);
        if (extra[i])
            bits((uint32_t)(len - base[i]), extra[i]);
        code(0, 5); // distance code 0: distance 1
    }

    void flushRun()
    {
        if (run_ >= 3)
            match(run_);
        else
            for (int i = 0; i < run_; i++)
                literal(last_);
        run_ = 0;
    }
};

} // namespace

bool encodePng(const uint8_t *pixels, int width, int height, const uint8_t *palette, int colours, Buf &out)
{
    out.clear();
    if (width <= 0 || height <= 0 || colours < 1 || colours > 256)
        return false;
    const int depth = colours <= 16 ? 4 : 8;
    const size_t rowBytes = depth == 4 ? (size_t)(width + 1) / 2 : (size_t)width;

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.append(sig, 8);

    uint8_t ihdr[13] = {(uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
                        (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
                        (uint8_t)depth, 3, 0, 0, 0};
    chunk(out, "IHDR", ihdr, sizeof(ihdr));
    chunk(out, "PLTE", palette, (size_t)colours * 3);

    //  IDAT: its length is known only at the end, so it goes in its own
    //  buffer first.
    Buf z{true};
    static const uint8_t zhead[2] = {0x78, 0x01};
    z.append(zhead, 2);
    {
        Deflater d(z);
        uint8_t *prev = (uint8_t *)big_alloc(rowBytes * 2);
        if (!prev)
            return false;
        uint8_t *cur = prev + rowBytes;
        memset(prev, 0, rowBytes);
        for (int y = 0; y < height; y++)
        {
            const uint8_t *src = pixels + (size_t)y * width;
            if (depth == 4)
                for (size_t i = 0; i < rowBytes; i++)
                {
                    uint8_t hi = src[2 * i] & 15;
                    uint8_t lo = 2 * i + 1 < (size_t)width ? src[2 * i + 1] & 15 : 0;
                    cur[i] = (uint8_t)(hi << 4 | lo);
                }
            else
                memcpy(cur, src, rowBytes);
            d.byte(2); // filter: Up
            for (size_t i = 0; i < rowBytes; i++)
                d.byte((uint8_t)(cur[i] - prev[i]));
            memcpy(prev, cur, rowBytes);
        }
        big_free(prev);
        d.finish();
        be32(z, d.adler());
    }
    if (z.failed)
        return false;
    chunk(out, "IDAT", z.data, z.len);
    chunk(out, "IEND", nullptr, 0);
    return !out.failed;
}

} // namespace web
