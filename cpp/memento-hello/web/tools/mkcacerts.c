/*
 *  mkcacerts --- writes the browser's trust anchor file (cacerts.bin).
 *
 *      mkcacerts out.bin roots.pem [more.pem ...]
 *
 *  Every certificate in the PEM files becomes a trust anchor: its subject
 *  name and public key, which is all BearSSL keeps of a root.  The format,
 *  read back by web_tls_load_anchors() in ../tls.c, all numbers little-endian:
 *
 *      "R2TA"  u16 version (1)  u16 count
 *      count times:
 *          u8 flags (1: a CA)   u8 key type (1: RSA, 2: EC)   u16 dn length, dn
 *          RSA: u16 n length, n, u16 e length, e
 *          EC:  u8 curve, u16 point length, point
 *
 *  Built on the host against BearSSL (tools/mkcacerts.sh does it).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl.h"

static unsigned char *out;
static size_t outLen, outCap;

static void put(const void *p, size_t n)
{
    if (outLen + n > outCap) {
        outCap = (outLen + n) * 2 + 4096;
        out = realloc(out, outCap);
        if (!out)
            exit(1);
    }
    memcpy(out + outLen, p, n);
    outLen += n;
}

static void put8(unsigned v)
{
    unsigned char b = (unsigned char)v;
    put(&b, 1);
}

static void put16(unsigned v)
{
    put8(v & 0xFF);
    put8(v >> 8);
}

/*  The subject DN, as the X.509 decoder hands it out in pieces.  */
static unsigned char dn[4096];
static size_t dnLen;

static void dnAppend(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    if (dnLen + len <= sizeof dn) {
        memcpy(dn + dnLen, buf, len);
        dnLen += len;
    }
}

/*  A certificate's DER, likewise.  */
static unsigned char der[65536];
static size_t derLen;

static void derAppend(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    if (derLen + len <= sizeof der) {
        memcpy(der + derLen, buf, len);
        derLen += len;
    }
}

static int count;

static void anchor(void)
{
    br_x509_decoder_context dc;
    const br_x509_pkey *pk;

    dnLen = 0;
    br_x509_decoder_init(&dc, dnAppend, NULL);
    br_x509_decoder_push(&dc, der, derLen);
    pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || br_x509_decoder_last_error(&dc)) {
        fprintf(stderr, "skipping a certificate that does not decode (%d)\n", br_x509_decoder_last_error(&dc));
        return;
    }
    put8(br_x509_decoder_isCA(&dc) ? 1 : 0);
    if (pk->key_type == BR_KEYTYPE_RSA) {
        put8(1);
        put16((unsigned)dnLen);
        put(dn, dnLen);
        put16((unsigned)pk->key.rsa.nlen);
        put(pk->key.rsa.n, pk->key.rsa.nlen);
        put16((unsigned)pk->key.rsa.elen);
        put(pk->key.rsa.e, pk->key.rsa.elen);
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        put8(2);
        put16((unsigned)dnLen);
        put(dn, dnLen);
        put8((unsigned)pk->key.ec.curve);
        put16((unsigned)pk->key.ec.qlen);
        put(pk->key.ec.q, pk->key.ec.qlen);
    } else {
        fprintf(stderr, "skipping a key of unknown type\n");
        outLen -= 1;
        return;
    }
    count++;
}

static void readPem(const char *path)
{
    FILE *f = fopen(path, "rb");
    br_pem_decoder_context pc;
    unsigned char buf[4096];
    size_t n;
    int inCert = 0;

    if (!f) {
        perror(path);
        exit(1);
    }
    br_pem_decoder_init(&pc);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        size_t off = 0;
        while (off < n) {
            off += br_pem_decoder_push(&pc, buf + off, n - off);
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ:
                inCert = !strcmp(br_pem_decoder_name(&pc), "CERTIFICATE") ||
                         !strcmp(br_pem_decoder_name(&pc), "X509 CERTIFICATE");
                derLen = 0;
                br_pem_decoder_setdest(&pc, inCert ? derAppend : NULL, NULL);
                break;
            case BR_PEM_END_OBJ:
                if (inCert)
                    anchor();
                inCert = 0;
                break;
            case BR_PEM_ERROR:
                fprintf(stderr, "%s: not valid PEM\n", path);
                exit(1);
            }
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    FILE *f;
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s out.bin roots.pem [more.pem ...]\n", argv[0]);
        return 2;
    }
    put("R2TA", 4);
    put16(1);
    put16(0); /*  the count, filled in below  */
    for (i = 2; i < argc; i++)
        readPem(argv[i]);
    out[6] = (unsigned char)(count & 0xFF);
    out[7] = (unsigned char)(count >> 8);

    f = fopen(argv[1], "wb");
    if (!f || fwrite(out, 1, outLen, f) != outLen || fclose(f)) {
        perror(argv[1]);
        return 1;
    }
    fprintf(stderr, "%s: %d roots, %zu bytes\n", argv[1], count, outLen);
    return 0;
}
