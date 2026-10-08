#include "syscall.h"

#include "auth.h"

#define SESSION_PATH ((const uint8_t *)"/mnt/tmp/SESSION.CFG")
#define SESSION_ROUNDS 4096 /* as Memento's SESSION_ROUNDS */

/*
 *  SHA-256 (FIPS 180-4), just enough of it for the session hash: Memento
 *  makes it with BearSSL, which tnt does not link.
 */

typedef struct {
    uint32_t h[8];
    uint8_t block[64];
    uint32_t used;  /* bytes in block */
    uint64_t total; /* bytes hashed */
} Sha256_T;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256_compress(Sha256_T *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) |
               (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

static void sha256_init(Sha256_T *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (int i = 0; i < 8; i++)
        c->h[i] = iv[i];
    c->used = 0;
    c->total = 0;
}

static void sha256_update(Sha256_T *c, const uint8_t *p, uint32_t n) {
    c->total += n;
    while (n--) {
        c->block[c->used++] = *p++;
        if (c->used == 64) {
            sha256_compress(c, c->block);
            c->used = 0;
        }
    }
}

static void sha256_final(Sha256_T *c, uint8_t out[32]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->used != 56)
        sha256_update(c, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

/*
 *  SESSION.CFG
 */

static uint8_t cfg[512];

/*  The file, NUL-terminated in cfg; its length, or 0 when it is not there.  */
static uint32_t read_session(void) {
    for (uint32_t i = 0; i < sizeof(cfg); i++)
        cfg[i] = 0;
    int64_t n = read_file_at(SESSION_PATH, cfg, 0, sizeof(cfg) - 1);
    return n > 0 ? (uint32_t)n : 0;
}

/*  `key` (as "salt=") followed by `len` lowercase hex digits, into `out`.  */
static int field(const uint8_t *key, uint8_t *out, uint32_t len) {
    uint32_t kl = 0;
    while (key[kl])
        kl++;
    for (uint32_t at = 0; cfg[at]; at++) {
        uint32_t k = 0;
        while (k < kl && cfg[at + k] == key[k])
            k++;
        if (k < kl)
            continue;
        const uint8_t *p = cfg + at + kl;
        for (uint32_t i = 0; i < len; i++) {
            uint8_t c = p[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return 0;
            out[i] = c;
        }
        out[len] = 0;
        return 1;
    }
    return 0;
}

int auth_required(void) {
    if (!read_session())
        return 0;
    return !auth_check((const uint8_t *)"", 0, (const uint8_t *)"", 0) &&
           !auth_check((const uint8_t *)"root", 4, (const uint8_t *)"", 0);
}

int auth_check(const uint8_t *login, uint32_t login_len, const uint8_t *pass, uint32_t pass_len) {
    uint8_t salt[33], want[65];
    if (!read_session() || !field((const uint8_t *)"salt=", salt, 32) ||
        !field((const uint8_t *)"hash=", want, 64))
        return 0;

    /*  Memento's digest: SESSION_ROUNDS times SHA-256 over the last round's
     *  hash (zeros the first time), the salt as its hex text, the login, a
     *  newline and the password.  */
    uint8_t h[32] = {0};
    for (int round = 0; round < SESSION_ROUNDS; round++) {
        Sha256_T c;
        sha256_init(&c);
        sha256_update(&c, h, 32);
        sha256_update(&c, salt, 32);
        sha256_update(&c, login, login_len);
        sha256_update(&c, (const uint8_t *)"\n", 1);
        sha256_update(&c, pass, pass_len);
        sha256_final(&c, h);
    }

    /*  Compared whole, not up to the first difference.  */
    static const uint8_t hex[] = "0123456789abcdef";
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= (uint8_t)((hex[h[i] >> 4] ^ want[2 * i]) | (hex[h[i] & 15] ^ want[2 * i + 1]));
    return diff == 0;
}
