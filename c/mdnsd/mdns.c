#include "mdns.h"
#include "bytes.h"

/*
 *  mdns.c
 *
 *  Questions in, answers out; and for browse.c the other way round, questions
 *  out and the records of the answers in.  What a responder needs and nothing
 *  more: no probing for a name already taken (the host name is the operator's
 *  to pick with --name), no known-answer suppression, no response delays, no
 *  NSEC for the records it does not have.  Names are written uncompressed; a
 *  record that would not fit the frame is left out, and so is everything
 *  after it.
 */

#define ETH_LEN 14
#define IP_LEN 20
#define UDP_LEN 8
#define HDR_LEN (ETH_LEN + IP_LEN + UDP_LEN)
#define DNS_MAX (MDNS_FRAME_MAX - HDR_LEN)

#define T_A MDNS_T_A
#define T_PTR MDNS_T_PTR
#define T_TXT MDNS_T_TXT
#define T_SRV MDNS_T_SRV
#define T_ANY 255
#define C_IN 1
#define C_TOP 0x8000 /* cache-flush in a record's class, unicast-response in a question's */

#define LEGACY_TTL 10 /* the most a legacy resolver is given (RFC 6762 §6.7) */
#define MAX_QUESTIONS 8
#define NAME_MAX MDNS_NAME_MAX

static const uint8_t NO_IP[4] = {0, 0, 0, 0};
static const uint8_t GROUP_IP[4] = {224, 0, 0, 251};
static const uint8_t GROUP_MAC[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb};

/* ── Bytes ───────────────────────────────────────────────────────── */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void wr16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void copy(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        dst[i] = src[i];
}

static uint8_t eq4(const uint8_t *a, const uint8_t *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }

static uint32_t cstr_len(const char *s) {
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static uint8_t same_type(const MdnsService_T *a, const MdnsService_T *b) {
    uint32_t i = 0;
    while (a->type[i] && a->type[i] == b->type[i])
        i++;
    return a->type[i] == b->type[i];
}

/* ── Names ───────────────────────────────────────────────────────── */

/* A name as its labels, at most four: <instance>.<type>._tcp.local. */
typedef struct {
    const uint8_t *label[4];
    uint8_t len[4];
    uint8_t n;
} Name_T;

static void add_label(Name_T *nm, const uint8_t *label, uint32_t len) {
    nm->label[nm->n] = label;
    nm->len[nm->n] = (uint8_t)len;
    nm->n++;
}

static void add_str(Name_T *nm, const char *s) { add_label(nm, (const uint8_t *)s, cstr_len(s)); }

/* <host>.local */
static void host_name(const Mdns_T *m, Name_T *nm) {
    nm->n = 0;
    add_label(nm, m->host, m->host_len);
    add_str(nm, "local");
}

/* _services._dns-sd._udp.local, what a browser asks to learn every type there is (RFC 6763 §9) */
static void meta_name(Name_T *nm) {
    nm->n = 0;
    add_str(nm, "_services");
    add_str(nm, "_dns-sd");
    add_str(nm, "_udp");
    add_str(nm, "local");
}

/* <type>._tcp.local */
static void type_name(const MdnsService_T *s, Name_T *nm) {
    nm->n = 0;
    add_str(nm, s->type);
    add_str(nm, "_tcp");
    add_str(nm, "local");
}

/* <instance>.<type>._tcp.local */
static void instance_name(const MdnsService_T *s, Name_T *nm) {
    nm->n = 0;
    add_label(nm, s->instance, s->instance_len);
    add_str(nm, s->type);
    add_str(nm, "_tcp");
    add_str(nm, "local");
}

/* Whether <wire>, a name as read_name() leaves it, is <nm>; ASCII case does not count. */
static uint8_t name_eq(const uint8_t *wire, const Name_T *nm) {
    uint32_t p = 0;
    for (uint8_t i = 0; i < nm->n; i++) {
        if (wire[p] != nm->len[i])
            return 0;
        for (uint8_t j = 0; j < nm->len[i]; j++)
            if (lower(wire[p + 1 + j]) != lower(nm->label[i][j]))
                return 0;
        p += 1u + nm->len[i];
    }
    return wire[p] == 0;
}

/* Copies the name at <off> in the message into <out> uncompressed: length-prefixed labels and a
 * zero.  Compression pointers are followed, at most 16 of them.  Returns the offset just past the
 * name where it sits in the message, or 0 when it is malformed or longer than a name may be. */
static uint32_t read_name(const uint8_t *msg, uint32_t len, uint32_t off, uint8_t out[NAME_MAX]) {
    uint32_t end = 0, n = 0, jumps = 0;
    for (;;) {
        if (off >= len)
            return 0;
        uint8_t b = msg[off];
        if ((b & 0xc0) == 0xc0) {
            if (off + 1 >= len || ++jumps > 16)
                return 0;
            if (!end)
                end = off + 2;
            off = (uint32_t)(b & 0x3f) << 8 | msg[off + 1];
            continue;
        }
        if (b & 0xc0) /* 0x40 and 0x80: label types nobody uses */
            return 0;
        if (off + 1 + b > len || n + 1 + b + 1 > NAME_MAX)
            return 0;
        out[n++] = b;
        if (!b)
            return end ? end : off + 1;
        copy(out + n, msg + off + 1, b);
        n += b;
        off += 1u + b;
    }
}

/* ── Writing a message ───────────────────────────────────────────── */

typedef struct {
    uint8_t *buf; /* the DNS message, after the frame's headers */
    uint32_t len;
    uint8_t full; /* a record did not fit: nothing more goes in */
    uint8_t legacy;  /* for a legacy resolver: short TTLs, no cache-flush bits */
    uint8_t goodbye; /* TTL 0 */
    uint16_t count;  /* records written in the current section */
} Msg_T;

static void put8(Msg_T *g, uint32_t v) {
    if (g->len < DNS_MAX)
        g->buf[g->len++] = (uint8_t)v;
    else
        g->full = 1;
}

static void put16(Msg_T *g, uint32_t v) {
    put8(g, v >> 8);
    put8(g, v);
}

static void put_name(Msg_T *g, const Name_T *nm) {
    for (uint8_t i = 0; i < nm->n; i++) {
        put8(g, nm->len[i]);
        for (uint8_t j = 0; j < nm->len[i]; j++)
            put8(g, nm->label[i][j]);
    }
    put8(g, 0);
}

/* A record's name, type, class and TTL, and room for its data length.  Returns where the data
 * starts, for rr_end(). */
static uint32_t rr_begin(Msg_T *g, const Name_T *nm, uint16_t type, uint8_t unique, uint32_t ttl) {
    if (g->goodbye)
        ttl = 0;
    else if (g->legacy && ttl > LEGACY_TTL)
        ttl = LEGACY_TTL;
    put_name(g, nm);
    put16(g, type);
    put16(g, C_IN | (unique && !g->legacy ? C_TOP : 0));
    put16(g, ttl >> 16);
    put16(g, ttl);
    put16(g, 0);
    return g->len;
}

/* Fills in the length of the record begun at <start>, or takes it out again if it did not fit. */
static void rr_end(Msg_T *g, uint32_t start, uint32_t data) {
    if (g->full) {
        g->len = start;
        return;
    }
    wr16(g->buf + data - 2, g->len - data);
    g->count++;
}

/* <host>.local A <ip> */
static void rr_a(Msg_T *g, const Mdns_T *m) {
    Name_T nm;
    host_name(m, &nm);
    uint32_t start = g->len;
    uint32_t data = rr_begin(g, &nm, T_A, 1, MDNS_TTL_HOST);
    for (uint32_t i = 0; i < 4; i++)
        put8(g, m->ip[i]);
    rr_end(g, start, data);
}

/* _services._dns-sd._udp.local PTR <type>._tcp.local */
static void rr_meta(Msg_T *g, const MdnsService_T *s) {
    Name_T meta, type;
    meta_name(&meta);
    type_name(s, &type);
    uint32_t start = g->len;
    uint32_t data = rr_begin(g, &meta, T_PTR, 0, MDNS_TTL_OTHER);
    put_name(g, &type);
    rr_end(g, start, data);
}

/* <type>._tcp.local PTR <instance>.<type>._tcp.local */
static void rr_ptr(Msg_T *g, const MdnsService_T *s) {
    Name_T type, inst;
    type_name(s, &type);
    instance_name(s, &inst);
    uint32_t start = g->len;
    uint32_t data = rr_begin(g, &type, T_PTR, 0, MDNS_TTL_OTHER);
    put_name(g, &inst);
    rr_end(g, start, data);
}

/* <instance>.<type>._tcp.local SRV 0 0 <port> <host>.local */
static void rr_srv(Msg_T *g, const Mdns_T *m, const MdnsService_T *s) {
    Name_T inst, host;
    instance_name(s, &inst);
    host_name(m, &host);
    uint32_t start = g->len;
    uint32_t data = rr_begin(g, &inst, T_SRV, 1, MDNS_TTL_HOST);
    put16(g, 0); /* priority */
    put16(g, 0); /* weight */
    put16(g, s->port);
    put_name(g, &host);
    rr_end(g, start, data);
}

/* <instance>.<type>._tcp.local TXT "": no keys, but DNS-SD wants the record (RFC 6763 §6.1) */
static void rr_txt(Msg_T *g, const MdnsService_T *s) {
    Name_T inst;
    instance_name(s, &inst);
    uint32_t start = g->len;
    uint32_t data = rr_begin(g, &inst, T_TXT, 1, MDNS_TTL_OTHER);
    put8(g, 0);
    rr_end(g, start, data);
}

static void begin_message(Msg_T *g, uint8_t *out, uint16_t id) {
    g->buf = out + HDR_LEN;
    g->len = 0;
    g->full = g->legacy = g->goodbye = 0;
    g->count = 0;
    put16(g, id);
    put16(g, 0x8400); /* a response, authoritative */
    for (uint32_t i = 0; i < 4; i++)
        put16(g, 0); /* the four counts, filled in as the sections are written */
}

/* Puts the Ethernet, IPv4 and UDP headers in front of the DNS message.  IP TTL 255, as every
 * mDNS packet has it (RFC 6762 §11). */
static uint32_t wrap(Mdns_T *m, const Msg_T *g, uint8_t *out, const uint8_t mac[6], const uint8_t ip[4], uint16_t port) {
    copy(out, mac, 6);
    copy(out + 6, m->mac, 6);
    out[12] = 0x08;
    out[13] = 0x00;

    uint8_t *iph = out + ETH_LEN;
    iph[0] = 0x45;
    iph[1] = 0;
    wr16(iph + 2, IP_LEN + UDP_LEN + g->len);
    wr16(iph + 4, m->ip_id++);
    wr16(iph + 6, 0);
    iph[8] = 255;
    iph[9] = 17;
    wr16(iph + 10, 0);
    copy(iph + 12, m->ip, 4);
    copy(iph + 16, ip, 4);
    wr16(iph + 10, inet_cksum(iph, IP_LEN));

    uint8_t *udp = iph + IP_LEN;
    wr16(udp, MDNS_PORT);
    wr16(udp + 2, port);
    wr16(udp + 4, UDP_LEN + g->len);
    wr16(udp + 6, 0); /* RFC 768 allows no checksum */
    return HDR_LEN + g->len;
}

/* ── Answering ───────────────────────────────────────────────────── */

/* Which records go in a section; bit i of a mask stands for services[i]. */
typedef struct {
    uint8_t a;
    uint32_t meta, ptr, srv, txt;
} Set_T;

static void write_set(Msg_T *g, const Mdns_T *m, const Set_T *s) {
    if (s->a)
        rr_a(g, m);
    for (uint8_t i = 0; i < m->n_services; i++)
        if (s->meta >> i & 1)
            rr_meta(g, &m->services[i]);
    for (uint8_t i = 0; i < m->n_services; i++) {
        if (s->ptr >> i & 1)
            rr_ptr(g, &m->services[i]);
        if (s->srv >> i & 1)
            rr_srv(g, m, &m->services[i]);
        if (s->txt >> i & 1)
            rr_txt(g, &m->services[i]);
    }
}

/* What answers the question <qname> <qtype>, added to <ans>. */
static void match(const Mdns_T *m, const uint8_t *qname, uint16_t qtype, Set_T *ans) {
    uint8_t any = qtype == T_ANY;
    Name_T nm;

    host_name(m, &nm);
    if ((any || qtype == T_A) && name_eq(qname, &nm))
        ans->a = 1;

    meta_name(&nm);
    if ((any || qtype == T_PTR) && name_eq(qname, &nm)) {
        /* One PTR per type, from the first service of it. */
        for (uint8_t i = 0; i < m->n_services; i++) {
            uint8_t seen = 0;
            for (uint8_t j = 0; j < i; j++)
                seen |= same_type(&m->services[i], &m->services[j]);
            if (!seen)
                ans->meta |= 1u << i;
        }
    }

    for (uint8_t i = 0; i < m->n_services; i++) {
        const MdnsService_T *s = &m->services[i];
        type_name(s, &nm);
        if ((any || qtype == T_PTR) && name_eq(qname, &nm))
            ans->ptr |= 1u << i;
        instance_name(s, &nm);
        if (name_eq(qname, &nm)) {
            if (any || qtype == T_SRV)
                ans->srv |= 1u << i;
            if (any || qtype == T_TXT)
                ans->txt |= 1u << i;
        }
    }
}

uint32_t mdns_handle(Mdns_T *m, const uint8_t *frame, uint32_t len, uint8_t *out) {
    if (eq4(m->ip, NO_IP) || len < HDR_LEN + 12 || frame[12] != 0x08 || frame[13] != 0x00)
        return 0;

    const uint8_t *ip = frame + ETH_LEN;
    uint32_t ihl = (ip[0] & 0x0fu) * 4;
    uint32_t total = rd16(ip + 2);
    if (ip[0] >> 4 != 4 || ihl < IP_LEN || ip[9] != 17 || total < ihl + UDP_LEN + 12 || total > len - ETH_LEN || (rd16(ip + 6) & 0x3fff))
        return 0;
    const uint8_t *src = ip + 12;
    uint8_t to_group = eq4(ip + 16, GROUP_IP);
    if ((!to_group && !eq4(ip + 16, m->ip)) || eq4(src, m->ip))
        return 0;

    const uint8_t *udp = ip + ihl;
    uint16_t sport = rd16(udp);
    uint32_t ulen = rd16(udp + 4);
    if (rd16(udp + 2) != MDNS_PORT || ulen < UDP_LEN + 12 || ulen > total - ihl)
        return 0;

    const uint8_t *msg = udp + UDP_LEN;
    uint32_t mlen = ulen - UDP_LEN;
    /* Queries only: QR, opcode and rcode all zero (RFC 6762 §18.3, §18.11). */
    if (rd16(msg + 2) & 0xf80f)
        return 0;

    uint8_t legacy = sport != MDNS_PORT;
    uint8_t qnames[MAX_QUESTIONS][NAME_MAX];
    uint16_t qtypes[MAX_QUESTIONS], qclasses[MAX_QUESTIONS];
    uint32_t nq = 0;
    Set_T ans = {0, 0, 0, 0, 0};

    uint32_t off = 12;
    for (uint32_t q = rd16(msg + 4); q && nq < MAX_QUESTIONS; q--, nq++) {
        off = read_name(msg, mlen, off, qnames[nq]);
        if (!off || off + 4 > mlen)
            return 0;
        qtypes[nq] = rd16(msg + off);
        qclasses[nq] = rd16(msg + off + 2);
        off += 4;
        uint16_t qclass = qclasses[nq] & ~C_TOP;
        if (qclass == C_IN || qclass == T_ANY)
            match(m, qnames[nq], qtypes[nq], &ans);
    }

    /* What a client asks next, sent along (RFC 6763 §12): a PTR's instance and its address. */
    Set_T add = {0, 0, 0, 0, 0};
    add.srv = ans.ptr & ~ans.srv;
    add.txt = ans.ptr & ~ans.txt;
    add.a = !ans.a && (ans.ptr | ans.srv);

    Msg_T g;
    begin_message(&g, out, legacy ? rd16(msg) : 0);
    g.legacy = legacy;
    if (legacy) {
        /* A legacy resolver wants its questions back (RFC 6762 §6.7). */
        for (uint32_t i = 0; i < nq; i++) {
            for (uint32_t p = 0; qnames[i][p]; p += 1u + qnames[i][p])
                for (uint32_t j = 0; j <= qnames[i][p]; j++)
                    put8(&g, qnames[i][p + j]);
            put8(&g, 0);
            put16(&g, qtypes[i]);
            put16(&g, qclasses[i]);
        }
        if (g.full)
            return 0;
        wr16(g.buf + 4, nq);
    }

    write_set(&g, m, &ans);
    if (!g.count)
        return 0;
    wr16(g.buf + 6, g.count);
    g.count = 0;
    write_set(&g, m, &add);
    wr16(g.buf + 10, g.count);

    if (legacy || !to_group)
        return wrap(m, &g, out, frame + 6, src, sport);
    return wrap(m, &g, out, GROUP_MAC, GROUP_IP, MDNS_PORT);
}

uint32_t mdns_announce(Mdns_T *m, const MdnsService_T *svc, uint8_t goodbye, uint8_t *out) {
    if (eq4(m->ip, NO_IP))
        return 0;
    Msg_T g;
    begin_message(&g, out, 0);
    g.goodbye = goodbye;
    if (svc) {
        rr_ptr(&g, svc);
        rr_srv(&g, m, svc);
        rr_txt(&g, svc);
    }
    if (!goodbye) {
        if (svc)
            rr_meta(&g, svc);
        rr_a(&g, m);
    }
    if (!g.count)
        return 0;
    wr16(g.buf + 6, g.count);
    return wrap(m, &g, out, GROUP_MAC, GROUP_IP, MDNS_PORT);
}

/* ── Asking ──────────────────────────────────────────────────────── */

/* The length of <name> in wire format, its zero included. */
static uint32_t wire_len(const uint8_t *name) {
    uint32_t n = 0;
    while (n < NAME_MAX && name[n])
        n += 1u + name[n];
    return n + 1;
}

uint32_t mdns_query(Mdns_T *m, const MdnsQuestion_T *q, uint32_t *n, uint8_t *out) {
    if (eq4(m->ip, NO_IP)) {
        *n = 0;
        return 0;
    }
    Msg_T g;
    begin_message(&g, out, 0);
    wr16(g.buf + 2, 0); /* a query: no flags */
    uint32_t asked = 0;
    for (; asked < *n; asked++) {
        uint32_t start = g.len, len = wire_len(q[asked].name);
        if (len > NAME_MAX)
            break;
        for (uint32_t i = 0; i < len; i++)
            put8(&g, q[asked].name[i]);
        put16(&g, q[asked].type);
        put16(&g, C_IN); /* QM: answers go to the group, where every browser sees them */
        if (g.full) {
            g.len = start;
            break;
        }
    }
    *n = asked;
    if (!asked)
        return 0;
    wr16(g.buf + 4, asked);
    return wrap(m, &g, out, GROUP_MAC, GROUP_IP, MDNS_PORT);
}

/* ── Reading answers ─────────────────────────────────────────────── */

uint8_t mdns_response(const Mdns_T *m, const uint8_t *frame, uint32_t len, MdnsRecords_T *it) {
    if (eq4(m->ip, NO_IP) || len < HDR_LEN + 12 || frame[12] != 0x08 || frame[13] != 0x00)
        return 0;

    const uint8_t *ip = frame + ETH_LEN;
    uint32_t ihl = (ip[0] & 0x0fu) * 4;
    uint32_t total = rd16(ip + 2);
    if (ip[0] >> 4 != 4 || ihl < IP_LEN || ip[9] != 17 || total < ihl + UDP_LEN + 12 || total > len - ETH_LEN || (rd16(ip + 6) & 0x3fff))
        return 0;
    if ((!eq4(ip + 16, GROUP_IP) && !eq4(ip + 16, m->ip)) || eq4(ip + 12, m->ip))
        return 0;

    const uint8_t *udp = ip + ihl;
    uint32_t ulen = rd16(udp + 4);
    if (rd16(udp) != MDNS_PORT || rd16(udp + 2) != MDNS_PORT || ulen < UDP_LEN + 12 || ulen > total - ihl)
        return 0;

    const uint8_t *msg = udp + UDP_LEN;
    uint32_t mlen = ulen - UDP_LEN;
    /* Responses only: QR set, opcode and rcode zero (RFC 6762 §18.2, §18.3, §18.11). */
    if ((rd16(msg + 2) & 0xf80f) != 0x8000)
        return 0;

    uint32_t off = 12;
    uint8_t name[NAME_MAX];
    for (uint32_t q = rd16(msg + 4); q; q--) {
        off = read_name(msg, mlen, off, name);
        if (!off || off + 4 > mlen)
            return 0;
        off += 4;
    }
    it->msg = msg;
    it->len = mlen;
    it->off = off;
    it->left = (uint32_t)rd16(msg + 6) + rd16(msg + 8) + rd16(msg + 10);
    return 1;
}

uint8_t mdns_next_record(MdnsRecords_T *it, MdnsRecord_T *r) {
    while (it->left) {
        it->left--;
        uint32_t off = read_name(it->msg, it->len, it->off, r->name);
        if (!off || off + 10 > it->len)
            break;
        r->type = rd16(it->msg + off);
        uint16_t cls = rd16(it->msg + off + 2) & ~C_TOP;
        r->ttl = (uint32_t)rd16(it->msg + off + 4) << 16 | rd16(it->msg + off + 6);
        uint32_t rdlen = rd16(it->msg + off + 8);
        uint32_t data = off + 10, end = data + rdlen;
        if (end > it->len)
            break;
        it->off = end;
        if (cls != C_IN)
            continue;

        r->target[0] = 0;
        r->port = 0;
        copy(r->ip, NO_IP, 4);
        if (r->type == T_PTR) {
            uint32_t past = read_name(it->msg, it->len, data, r->target);
            if (!past || past > end)
                break;
        } else if (r->type == T_SRV) {
            if (rdlen < 7)
                break;
            r->port = rd16(it->msg + data + 4);
            uint32_t past = read_name(it->msg, it->len, data + 6, r->target);
            if (!past || past > end)
                break;
        } else if (r->type == T_A) {
            if (rdlen != 4)
                break;
            copy(r->ip, it->msg + data, 4);
        }
        return 1;
    }
    it->left = 0;
    return 0;
}
