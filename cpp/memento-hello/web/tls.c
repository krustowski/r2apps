/*
 *  tls.c --- the BearSSL client behind tls.h.
 *
 *  What is configured here, and why:
 *
 *  - TLS 1.2 only.  BearSSL has no 1.3, and nothing worth fetching still
 *    needs 1.0 or 1.1; leaving them out also leaves out MD5 and the old PRF.
 *
 *  - ECDHE with AES-GCM or ChaCha20-Poly1305 first, then a couple of CBC and
 *    static-RSA suites for old servers.  The implementations are BearSSL's
 *    portable constant-time ones: the build has no SSE or AES-NI code at all,
 *    because this kernel does not promise to preserve vector registers across
 *    a context switch, and crypto is the last place to find that out.
 *
 *  - Certificates are checked against the roots in a file the platform
 *    supplies (on r2, /mnt/iso/opt/memento/cacerts.bin), read on the first
 *    handshake: they live on the CD, not in a 2 MiB process.  A chain that
 *    ends elsewhere is refused unless the user asks for the insecure retry,
 *    which accepts an unknown root and nothing else: names and dates are
 *    still checked.
 */

#include <string.h>

#include "bearssl.h"
#include "tls.h"

/*
 *  The trust anchors, from the platform's file.  Each anchor points into the
 *  file's bytes, which the platform keeps for the life of the program.
 */
static br_x509_trust_anchor *anchors;
static int anchorCount = -1;
static int anchorsTried;

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

static void loadAnchors(void)
{
    const unsigned char *d;
    unsigned long len, off;
    unsigned count, k;

    anchorsTried = 1;
    if (web_tls_platform_anchors(&d, &len) || len < 8 || memcmp(d, "R2TA", 4) || rd16(d + 4) != 1)
        return;
    count = rd16(d + 6);
    anchors = (br_x509_trust_anchor *)web_tls_alloc(sizeof(br_x509_trust_anchor) * (count ? count : 1));
    if (!anchors)
        return;
    off = 8;
    for (k = 0; k < count; k++) {
        br_x509_trust_anchor *ta = &anchors[k];
        unsigned type, l;
        if (off + 4 > len)
            break;
        ta->flags = d[off] & 1 ? BR_X509_TA_CA : 0;
        type = d[off + 1];
        l = rd16(d + off + 2);
        off += 4;
        if (off + l > len)
            break;
        ta->dn.data = (unsigned char *)d + off;
        ta->dn.len = l;
        off += l;
        if (type == 1) {
            if (off + 2 > len)
                break;
            l = rd16(d + off);
            off += 2;
            if (off + l + 2 > len)
                break;
            ta->pkey.key_type = BR_KEYTYPE_RSA;
            ta->pkey.key.rsa.n = (unsigned char *)d + off;
            ta->pkey.key.rsa.nlen = l;
            off += l;
            l = rd16(d + off);
            off += 2;
            if (off + l > len)
                break;
            ta->pkey.key.rsa.e = (unsigned char *)d + off;
            ta->pkey.key.rsa.elen = l;
            off += l;
        } else if (type == 2) {
            if (off + 3 > len)
                break;
            ta->pkey.key_type = BR_KEYTYPE_EC;
            ta->pkey.key.ec.curve = d[off];
            l = rd16(d + off + 1);
            off += 3;
            if (off + l > len)
                break;
            ta->pkey.key.ec.q = (unsigned char *)d + off;
            ta->pkey.key.ec.qlen = l;
            off += l;
        } else
            break;
    }
    /*  A file cut short keeps the roots read before the cut.  */
    anchorCount = (int)k;
}

int web_tls_anchor_count(void)
{
    if (!anchorsTried)
        loadAnchors();
    return anchorCount;
}

/*
 *  The insecure retry: an X.509 engine that forwards everything to the
 *  minimal one and forgives exactly one verdict.  BearSSL's minimal engine
 *  still hands out the server key after BR_ERR_X509_NOT_TRUSTED (it exists for
 *  this), so the handshake can go on with the key from the certificate.
 */
typedef struct {
    const br_x509_class *vtable;
    const br_x509_class **inner;
} x509_lenient;

static void xl_start_chain(const br_x509_class **ctx, const char *server_name)
{
    x509_lenient *x = (x509_lenient *)(void *)ctx;
    (*x->inner)->start_chain(x->inner, server_name);
}

static void xl_start_cert(const br_x509_class **ctx, uint32_t length)
{
    x509_lenient *x = (x509_lenient *)(void *)ctx;
    (*x->inner)->start_cert(x->inner, length);
}

static void xl_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    x509_lenient *x = (x509_lenient *)(void *)ctx;
    (*x->inner)->append(x->inner, buf, len);
}

static void xl_end_cert(const br_x509_class **ctx)
{
    x509_lenient *x = (x509_lenient *)(void *)ctx;
    (*x->inner)->end_cert(x->inner);
}

static unsigned xl_end_chain(const br_x509_class **ctx)
{
    x509_lenient *x = (x509_lenient *)(void *)ctx;
    unsigned r = (*x->inner)->end_chain(x->inner);
    return r == BR_ERR_X509_NOT_TRUSTED ? 0 : r;
}

static const br_x509_pkey *xl_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    const x509_lenient *x = (const x509_lenient *)(const void *)ctx;
    return (*x->inner)->get_pkey(x->inner, usages);
}

static const br_x509_class x509_lenient_vtable = {
    sizeof(x509_lenient), xl_start_chain, xl_start_cert, xl_append, xl_end_cert, xl_end_chain, xl_get_pkey};

struct web_tls {
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    x509_lenient lenient;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

static const uint16_t kSuites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
    BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
    BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_RSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_RSA_WITH_AES_128_CBC_SHA,
};

web_tls *web_tls_new(const char *host, int insecure, const unsigned char *seed, unsigned long seed_len,
                     unsigned long days, unsigned long seconds)
{
    web_tls *t = (web_tls *)web_tls_alloc(sizeof(web_tls));
    br_ssl_engine_context *eng;
    br_x509_minimal_context *xc;

    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));

    eng = &t->cc.eng;
    xc = &t->xc;

    br_ssl_client_zero(&t->cc);
    br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);

    /*  The X.509 engine hashes names with SHA-256 to compare them.  */
    web_tls_anchor_count();
    br_x509_minimal_init(xc, &br_sha256_vtable, anchors, anchorCount > 0 ? (size_t)anchorCount : 0);

    br_ssl_engine_set_suites(eng, kSuites, sizeof(kSuites) / sizeof(kSuites[0]));
    br_ssl_client_set_default_rsapub(&t->cc);
    br_ssl_engine_set_default_rsavrfy(eng);
    br_ssl_engine_set_default_ecdsa(eng);
    br_x509_minimal_set_rsa(xc, br_ssl_engine_get_rsavrfy(eng));
    br_x509_minimal_set_ecdsa(xc, br_ssl_engine_get_ec(eng), br_ssl_engine_get_ecdsa(eng));

    /*  SHA-1 still signs a few intermediates, and names the CBC suites' MAC.  */
    br_ssl_engine_set_hash(eng, br_sha1_ID, &br_sha1_vtable);
    br_ssl_engine_set_hash(eng, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(eng, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(xc, br_sha1_ID, &br_sha1_vtable);
    br_x509_minimal_set_hash(xc, br_sha224_ID, &br_sha224_vtable);
    br_x509_minimal_set_hash(xc, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(xc, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(xc, br_sha512_ID, &br_sha512_vtable);

    br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(eng, &br_tls12_sha384_prf);

    br_ssl_engine_set_default_aes_gcm(eng);
    br_ssl_engine_set_default_aes_cbc(eng);
    br_ssl_engine_set_default_chapol(eng);

    if (days)
        br_x509_minimal_set_time(xc, (uint32_t)days, (uint32_t)seconds);

    if (insecure) {
        t->lenient.vtable = &x509_lenient_vtable;
        t->lenient.inner = &xc->vtable;
        br_ssl_engine_set_x509(eng, &t->lenient.vtable);
    } else {
        br_ssl_engine_set_x509(eng, &xc->vtable);
    }

    br_ssl_engine_set_buffer(eng, t->iobuf, sizeof(t->iobuf), 1);

    /*  The PRNG is seeded from this and nothing else: there is no system
     *  source for BearSSL to find on this machine.  */
    br_ssl_engine_inject_entropy(eng, seed, (size_t)seed_len);

    /*  A failed reset leaves the engine closed with its error set, which the
     *  caller reports like any other TLS failure.  */
    br_ssl_client_reset(&t->cc, host, 0);
    return t;
}

void web_tls_free(web_tls *t)
{
    if (t)
        web_tls_release(t);
}

unsigned web_tls_state(web_tls *t)
{
    unsigned st = br_ssl_engine_current_state(&t->cc.eng);
    unsigned out = 0;
    if (st & BR_SSL_CLOSED)
        out |= WEB_TLS_CLOSED;
    if (st & BR_SSL_SENDREC)
        out |= WEB_TLS_SENDREC;
    if (st & BR_SSL_RECVREC)
        out |= WEB_TLS_RECVREC;
    if (st & BR_SSL_SENDAPP)
        out |= WEB_TLS_SENDAPP;
    if (st & BR_SSL_RECVAPP)
        out |= WEB_TLS_RECVAPP;
    return out;
}

unsigned char *web_tls_sendrec_buf(web_tls *t, unsigned long *len)
{
    size_t l = 0;
    unsigned char *p = br_ssl_engine_sendrec_buf(&t->cc.eng, &l);
    *len = (unsigned long)l;
    return p;
}

void web_tls_sendrec_ack(web_tls *t, unsigned long n) { br_ssl_engine_sendrec_ack(&t->cc.eng, (size_t)n); }

unsigned char *web_tls_recvrec_buf(web_tls *t, unsigned long *len)
{
    size_t l = 0;
    unsigned char *p = br_ssl_engine_recvrec_buf(&t->cc.eng, &l);
    *len = (unsigned long)l;
    return p;
}

void web_tls_recvrec_ack(web_tls *t, unsigned long n) { br_ssl_engine_recvrec_ack(&t->cc.eng, (size_t)n); }

unsigned char *web_tls_sendapp_buf(web_tls *t, unsigned long *len)
{
    size_t l = 0;
    unsigned char *p = br_ssl_engine_sendapp_buf(&t->cc.eng, &l);
    *len = (unsigned long)l;
    return p;
}

void web_tls_sendapp_ack(web_tls *t, unsigned long n) { br_ssl_engine_sendapp_ack(&t->cc.eng, (size_t)n); }

unsigned char *web_tls_recvapp_buf(web_tls *t, unsigned long *len)
{
    size_t l = 0;
    unsigned char *p = br_ssl_engine_recvapp_buf(&t->cc.eng, &l);
    *len = (unsigned long)l;
    return p;
}

void web_tls_recvapp_ack(web_tls *t, unsigned long n) { br_ssl_engine_recvapp_ack(&t->cc.eng, (size_t)n); }

void web_tls_flush(web_tls *t) { br_ssl_engine_flush(&t->cc.eng, 0); }

void web_tls_close(web_tls *t) { br_ssl_engine_close(&t->cc.eng); }

int web_tls_error(web_tls *t) { return br_ssl_engine_last_error(&t->cc.eng); }

int web_tls_established(web_tls *t)
{
    /*  The engine accepts application data only once the handshake is done.  */
    return (br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_SENDAPP) != 0 ||
           (br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_RECVAPP) != 0;
}

int web_tls_error_is_untrusted(int err) { return err == BR_ERR_X509_NOT_TRUSTED; }

const char *web_tls_error_text(int err)
{
    switch (err) {
    case BR_ERR_OK:
        return "no error";
    case BR_ERR_X509_NOT_TRUSTED:
        return "the certificate chain does not lead to a known root";
    case BR_ERR_X509_EXPIRED:
        return "the certificate has expired or is not yet valid (is the clock right?)";
    case BR_ERR_X509_BAD_SERVER_NAME:
        return "the certificate is for a different host name";
    case BR_ERR_X509_TIME_UNKNOWN:
        return "the time is unknown, so no certificate can be checked";
    case BR_ERR_X509_UNSUPPORTED:
    case BR_ERR_X509_BAD_SIGNATURE:
        return "the certificate uses a key or signature this client cannot check";
    case BR_ERR_X509_LIMIT_EXCEEDED:
        return "the certificate chain is too long";
    case BR_ERR_BAD_VERSION:
    case BR_ERR_UNSUPPORTED_VERSION:
        return "the server does not speak TLS 1.2";
    case BR_ERR_BAD_CIPHER_SUITE:
        return "no cipher suite in common with the server";
    case BR_ERR_BAD_MAC:
        return "a record failed its integrity check";
    case BR_ERR_IO:
        return "the connection failed during the handshake";
    case BR_ERR_BAD_PARAM:
        return "bad parameter (host name too long?)";
    case BR_ERR_TOO_LARGE:
        return "a record was larger than the buffer";
    default:
        if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_RECV_FATAL_ALERT + 256)
            return "the server refused the handshake (fatal alert)";
        if (err >= BR_ERR_SEND_FATAL_ALERT && err < BR_ERR_SEND_FATAL_ALERT + 256)
            return "this client aborted the handshake (fatal alert)";
        return "TLS failure";
    }
}
