#ifndef WEB_TLS_H
#define WEB_TLS_H

/*
 *  tls.h --- the browser's view of BearSSL.
 *
 *  A TLS 1.2 client on BearSSL's low-level engine, which never blocks and never
 *  touches a socket: ciphertext goes in and comes out through buffers the
 *  engine owns, and whoever holds the connection moves it.  That fits Memento,
 *  where nothing may block the event loop, and it keeps this file free of
 *  anything that belongs to the machine.
 *
 *  Plain C, so that it is compiled with the same flags as BearSSL itself; the
 *  C++ side sees only these few functions and an opaque handle.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct web_tls web_tls;

/*  What the engine can do next.  Several bits are usually set at once.  */
enum {
    WEB_TLS_CLOSED  = 0x0001,
    WEB_TLS_SENDREC = 0x0002, /*  has ciphertext for the network  */
    WEB_TLS_RECVREC = 0x0004, /*  wants ciphertext from the network  */
    WEB_TLS_SENDAPP = 0x0008, /*  will take plaintext to send  */
    WEB_TLS_RECVAPP = 0x0010  /*  has plaintext for us  */
};

/*
 *  Starts a handshake with `host` (used for SNI and for checking the
 *  certificate's name).  `seed` is entropy for the engine's PRNG --- at least
 *  32 bytes of it; the platform decides where it comes from.  `days` and
 *  `seconds` are the current UTC time as BearSSL counts it (days since
 *  0000-01-01, seconds into the day); zero days means "unknown", and every
 *  certificate is then rejected.  With `insecure` set, a chain that does not
 *  lead to a known root is accepted; nothing else is relaxed.
 *
 *  Returns NULL when there is no memory for the context (about 40 KiB).
 */
web_tls *web_tls_new(const char *host, int insecure, const unsigned char *seed, unsigned long seed_len,
                     unsigned long days, unsigned long seconds);
void web_tls_free(web_tls *t);

unsigned web_tls_state(web_tls *t);

/*  The engine's buffers, as BearSSL exposes them.  Each _buf call returns
 *  where the bytes are and how many; the matching _ack says how many were
 *  used.  See br_ssl_engine_sendrec_buf() and friends.  */
unsigned char *web_tls_sendrec_buf(web_tls *t, unsigned long *len);
void web_tls_sendrec_ack(web_tls *t, unsigned long n);
unsigned char *web_tls_recvrec_buf(web_tls *t, unsigned long *len);
void web_tls_recvrec_ack(web_tls *t, unsigned long n);
unsigned char *web_tls_sendapp_buf(web_tls *t, unsigned long *len);
void web_tls_sendapp_ack(web_tls *t, unsigned long n);
unsigned char *web_tls_recvapp_buf(web_tls *t, unsigned long *len);
void web_tls_recvapp_ack(web_tls *t, unsigned long n);

/*  Pushes out whatever plaintext has been written so far as a record.  */
void web_tls_flush(web_tls *t);

/*  Starts a polite close (close_notify).  */
void web_tls_close(web_tls *t);

/*  BearSSL's error code once the engine is closed; 0 for a clean close.  */
int web_tls_error(web_tls *t);

/*  Nonzero once the handshake has completed.  */
int web_tls_established(web_tls *t);

/*  A short explanation of an error code, for the error page.  */
const char *web_tls_error_text(int err);

/*  True when the error is one the "insecure" retry would get past.  */
int web_tls_error_is_untrusted(int err);

/*  The trust anchors, read on the first handshake from what the platform
 *  supplies (tools/mkcacerts.c describes the format).  The number of roots,
 *  or -1 when there were none to read.  */
int web_tls_anchor_count(void);

/*  Filled in by the platform (web_r2.cpp, or the host test).  */
void *web_tls_alloc(unsigned long n);
void web_tls_release(void *p);

/*  The anchor file's bytes, which must stay where they are for good; 0 on
 *  success.  And where they come from, for the error page.  */
int web_tls_platform_anchors(const unsigned char **data, unsigned long *len);
const char *web_tls_platform_anchor_path(void);

#ifdef __cplusplus
}
#endif

#endif
