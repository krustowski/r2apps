/* Host tests for mdnsd's responder: query frames in, reply frames out, checked
 * field by field, then a fuzz pass over mangled queries.  make test builds it
 * with ASan and UBSan; no r2 kernel is needed. */
#include "../mdns.h"

extern int printf(const char *, ...);
extern int strcmp(const char *, const char *);

static int failures, checks;
#define CHECK(c)                                                                                                                                                                                       \
    do {                                                                                                                                                                                               \
        checks++;                                                                                                                                                                                      \
        if (!(c)) {                                                                                                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                                                                                                         \
            failures++;                                                                                                                                                                                \
        }                                                                                                                                                                                              \
    } while (0)

#define T_A 1
#define T_PTR 12
#define T_TXT 16
#define T_AAAA 28
#define T_SRV 33
#define T_ANY 255

static const uint8_t OUR_IP[4] = {10, 3, 4, 2};
static const uint8_t OUR_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8_t PEER_IP[4] = {10, 3, 4, 1};
static const uint8_t PEER_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t GROUP_IP[4] = {224, 0, 0, 251};
static const uint8_t GROUP_MAC[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb};

static Mdns_T m;
static uint8_t out[MDNS_FRAME_MAX];

static uint8_t eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void set_label(uint8_t *dst, uint8_t *len, const char *s) {
    *len = 0;
    while (s[*len]) {
        dst[*len] = (uint8_t)s[*len];
        (*len)++;
    }
}

static void add_service(uint16_t port, const char *type, const char *instance) {
    MdnsService_T *s = &m.services[m.n_services++];
    s->port = port;
    s->type = type;
    set_label(s->instance, &s->instance_len, instance);
}

static void setup(void) {
    uint8_t *p = (uint8_t *)&m;
    for (uint32_t i = 0; i < sizeof(m); i++)
        p[i] = 0;
    set_label(m.host, &m.host_len, "rourex");
    for (int i = 0; i < 4; i++)
        m.ip[i] = OUR_IP[i];
    for (int i = 0; i < 6; i++)
        m.mac[i] = OUR_MAC[i];
    add_service(80, "_http", "rourex garn");
    add_service(8080, "_http", "rourex streamd");
    add_service(23, "_telnet", "rourex tnt");
}

/* ── Queries ─────────────────────────────────────────────────────── */

static uint8_t q[MDNS_FRAME_MAX];
static uint32_t qlen;
static uint16_t qcount;

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

static void q_begin(const uint8_t dst_ip[4], uint16_t sport, uint16_t id, uint16_t flags) {
    for (uint32_t i = 0; i < sizeof(q); i++)
        q[i] = 0;
    const uint8_t *dst_mac = eq(dst_ip, GROUP_IP, 4) ? GROUP_MAC : OUR_MAC;
    for (int i = 0; i < 6; i++) {
        q[i] = dst_mac[i];
        q[6 + i] = PEER_MAC[i];
    }
    q[12] = 0x08;
    q[14] = 0x45;
    q[22] = 255;
    q[23] = 17;
    for (int i = 0; i < 4; i++) {
        q[26 + i] = PEER_IP[i];
        q[30 + i] = dst_ip[i];
    }
    put16(q + 34, sport);
    put16(q + 36, 5353);
    put16(q + 42, id);
    put16(q + 44, flags);
    qlen = 54;
    qcount = 0;
}

static void q_byte(uint8_t b) { q[qlen++] = b; }

static void q_name(const char *dotted) {
    while (*dotted) {
        uint32_t at = qlen++;
        uint8_t n = 0;
        while (*dotted && *dotted != '.') {
            q_byte((uint8_t)*dotted++);
            n++;
        }
        q[at] = n;
        if (*dotted)
            dotted++;
    }
    q_byte(0);
}

static void q_tail(uint16_t type, uint16_t cls) {
    put16(q + qlen, type);
    put16(q + qlen + 2, cls);
    qlen += 4;
    qcount++;
}

static void q_question(const char *name, uint16_t type) {
    q_name(name);
    q_tail(type, 1);
}

static uint32_t q_end(void) {
    put16(q + 16, qlen - 14);
    put16(q + 38, qlen - 34);
    put16(q + 46, qcount);
    return qlen;
}

/* ── Replies ─────────────────────────────────────────────────────── */

typedef struct {
    char name[256];
    uint16_t type, cls;
    uint32_t ttl;
    const uint8_t *rd;
    uint16_t rdlen;
    char target[256]; /* a PTR's or SRV's name */
} Rr_T;

typedef struct {
    uint16_t id, flags, qd, an, ns, ar;
    char qname[8][256];
    Rr_T rr[40];
    uint32_t n;
    const uint8_t *dst_mac, *dst_ip;
    uint16_t dst_port;
    uint8_t ip_ttl;
} Reply_T;

static Reply_T r;

/* The name at <off>, dotted, into <s>; past it, or 0.  Replies are written uncompressed, so a
 * pointer is a failure too. */
static uint32_t dotted(const uint8_t *msg, uint32_t len, uint32_t off, char *s) {
    uint32_t n = 0;
    for (;;) {
        if (off >= len || msg[off] >= 64)
            return 0;
        uint8_t b = msg[off++];
        if (!b)
            break;
        if (off + b > len)
            return 0;
        if (n)
            s[n++] = '.';
        for (uint8_t i = 0; i < b; i++)
            s[n++] = (char)msg[off++];
    }
    s[n] = 0;
    return off;
}

static uint16_t ip_sum(const uint8_t *p) {
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2)
        sum += get16(p + i);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)sum;
}

/* Takes the reply frame apart; 0 if any of it is not as a frame should be. */
static int parse(uint32_t len) {
    r.n = 0;
    if (len < 54 || len > MDNS_FRAME_MAX || out[12] != 0x08 || out[13] != 0)
        return 0;
    const uint8_t *ip = out + 14;
    if (ip[0] != 0x45 || ip[9] != 17 || get16(ip + 2) != len - 14 || ip_sum(ip) != 0xffff || !eq(ip + 12, OUR_IP, 4) || !eq(out + 6, OUR_MAC, 6))
        return 0;
    const uint8_t *udp = ip + 20;
    if (get16(udp) != 5353 || get16(udp + 4) != len - 34)
        return 0;
    r.dst_mac = out;
    r.dst_ip = ip + 16;
    r.dst_port = get16(udp + 2);
    r.ip_ttl = ip[8];

    const uint8_t *msg = udp + 8;
    uint32_t mlen = len - 42, off = 12;
    r.id = get16(msg);
    r.flags = get16(msg + 2);
    r.qd = get16(msg + 4);
    r.an = get16(msg + 6);
    r.ns = get16(msg + 8);
    r.ar = get16(msg + 10);
    for (uint16_t i = 0; i < r.qd; i++) {
        if (i >= 8 || !(off = dotted(msg, mlen, off, r.qname[i])) || off + 4 > mlen)
            return 0;
        off += 4;
    }
    for (uint32_t i = 0; i < (uint32_t)r.an + r.ns + r.ar; i++) {
        Rr_T *rr = &r.rr[r.n++];
        if (r.n > 40 || !(off = dotted(msg, mlen, off, rr->name)) || off + 10 > mlen)
            return 0;
        rr->type = get16(msg + off);
        rr->cls = get16(msg + off + 2);
        rr->ttl = get32(msg + off + 4);
        rr->rdlen = get16(msg + off + 8);
        off += 10;
        rr->rd = msg + off;
        if (off + rr->rdlen > mlen)
            return 0;
        rr->target[0] = 0;
        if (rr->type == T_PTR && dotted(msg, off + rr->rdlen, off, rr->target) != off + rr->rdlen)
            return 0;
        if (rr->type == T_SRV && (rr->rdlen < 7 || dotted(msg, off + rr->rdlen, off + 6, rr->target) != off + rr->rdlen))
            return 0;
        off += rr->rdlen;
    }
    return off == mlen;
}

/* The first record in the reply with this name and type, from record <from> on. */
static const Rr_T *find(uint32_t from, uint32_t to, const char *name, uint16_t type) {
    for (uint32_t i = from; i < to && i < r.n; i++)
        if (r.rr[i].type == type && !strcmp(r.rr[i].name, name))
            return &r.rr[i];
    return 0;
}

static uint32_t handle(void) { return mdns_handle(&m, q, q_end(), out); }

static int to_group(void) { return eq(r.dst_mac, GROUP_MAC, 6) && eq(r.dst_ip, GROUP_IP, 4) && r.dst_port == 5353; }

/* ── Tests ───────────────────────────────────────────────────────── */

static void test_address(void) {
    setup();
    q_begin(GROUP_IP, 5353, 0x1234, 0);
    q_question("rourex.local", T_A);
    CHECK(parse(handle()));
    CHECK(to_group());
    CHECK(r.ip_ttl == 255);
    CHECK(r.id == 0 && r.flags == 0x8400 && r.qd == 0 && r.an == 1 && r.ar == 0);
    const Rr_T *a = find(0, 1, "rourex.local", T_A);
    CHECK(a && a->rdlen == 4 && eq(a->rd, OUR_IP, 4));
    CHECK(a && a->cls == 0x8001 && a->ttl == 120);

    /* DNS names do not care for case. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("ROUREX.Local", T_A);
    CHECK(parse(handle()) && r.an == 1);

    /* An IPv4 header with options (IHL 6): the UDP header starts later. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex.local", T_A);
    uint32_t len = q_end();
    for (uint32_t i = len; i-- > 34;)
        q[i + 4] = q[i];
    q[34] = 0x94; /* router alert, as IGMP carries it */
    q[35] = 0x04;
    q[36] = q[37] = 0;
    q[14] = 0x46;
    put16(q + 16, len + 4 - 14);
    CHECK(parse(mdns_handle(&m, q, len + 4, out)) && r.an == 1);

    /* QU questions are answered too, to the group. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_name("rourex.local");
    q_tail(T_ANY, 0x8001);
    CHECK(parse(handle()) && r.an == 1 && to_group());
}

static void test_not_ours(void) {
    setup();
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("other.local", T_A);
    CHECK(handle() == 0);

    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex.local", T_AAAA);
    CHECK(handle() == 0);

    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex.local.local", T_A);
    CHECK(handle() == 0);

    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex", T_A);
    CHECK(handle() == 0);

    /* Responses, and queries with an opcode or rcode, are not for answering. */
    q_begin(GROUP_IP, 5353, 0, 0x8400);
    q_question("rourex.local", T_A);
    CHECK(handle() == 0);
    q_begin(GROUP_IP, 5353, 0, 0x2000);
    q_question("rourex.local", T_A);
    CHECK(handle() == 0);
    q_begin(GROUP_IP, 5353, 0, 0x0003);
    q_question("rourex.local", T_A);
    CHECK(handle() == 0);

    /* Class CH: not ours. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_name("rourex.local");
    q_tail(T_A, 3);
    CHECK(handle() == 0);

    /* Sent to some other host, or from this one. */
    const uint8_t elsewhere[4] = {10, 3, 4, 9};
    q_begin(elsewhere, 5353, 0, 0);
    q_question("rourex.local", T_A);
    CHECK(handle() == 0);
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex.local", T_A);
    for (int i = 0; i < 4; i++)
        q[26 + i] = OUR_IP[i];
    CHECK(handle() == 0);

    /* No address yet: silence. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex.local", T_A);
    m.ip[0] = m.ip[1] = m.ip[2] = m.ip[3] = 0;
    CHECK(handle() == 0);
}

static void test_legacy(void) {
    setup();
    q_begin(GROUP_IP, 40000, 0xbeef, 0x0100);
    q_question("rourex.local", T_A);
    CHECK(parse(handle()));
    CHECK(eq(r.dst_mac, PEER_MAC, 6) && eq(r.dst_ip, PEER_IP, 4) && r.dst_port == 40000);
    CHECK(r.id == 0xbeef && r.qd == 1 && !strcmp(r.qname[0], "rourex.local"));
    const Rr_T *a = find(0, 1, "rourex.local", T_A);
    CHECK(a && a->cls == 1 && a->ttl == 10);

    /* Straight to this host from 5353: a unicast answer, cache-flush and all. */
    q_begin(OUR_IP, 5353, 0, 0);
    q_question("rourex.local", T_A);
    CHECK(parse(handle()));
    CHECK(eq(r.dst_mac, PEER_MAC, 6) && eq(r.dst_ip, PEER_IP, 4) && r.dst_port == 5353);
    CHECK(r.qd == 0 && r.an == 1 && r.rr[0].cls == 0x8001 && r.rr[0].ttl == 120);
}

static void test_browse(void) {
    setup();
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("_services._dns-sd._udp.local", T_PTR);
    CHECK(parse(handle()) && to_group());
    CHECK(r.an == 2 && r.ar == 0);
    CHECK(!strcmp(r.rr[0].target, "_http._tcp.local") && r.rr[0].cls == 1 && r.rr[0].ttl == 4500);
    CHECK(!strcmp(r.rr[1].target, "_telnet._tcp.local"));

    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("_http._tcp.local", T_PTR);
    CHECK(parse(handle()));
    CHECK(r.an == 2 && r.ar == 5);
    CHECK(!strcmp(r.rr[0].target, "rourex garn._http._tcp.local") && r.rr[0].cls == 1);
    CHECK(!strcmp(r.rr[1].target, "rourex streamd._http._tcp.local"));
    const Rr_T *srv = find(r.an, r.n, "rourex streamd._http._tcp.local", T_SRV);
    CHECK(srv && get16(srv->rd + 4) == 8080 && !strcmp(srv->target, "rourex.local") && srv->cls == 0x8001 && srv->ttl == 120);
    const Rr_T *txt = find(r.an, r.n, "rourex garn._http._tcp.local", T_TXT);
    CHECK(txt && txt->rdlen == 1 && txt->rd[0] == 0 && txt->ttl == 4500);
    CHECK(find(r.an, r.n, "rourex.local", T_A) != 0);

    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex tnt._telnet._tcp.local", T_SRV);
    CHECK(parse(handle()) && r.an == 1 && r.ar == 1);
    CHECK(r.rr[0].type == T_SRV && get16(r.rr[0].rd + 4) == 23);
    CHECK(r.rr[1].type == T_A);

    /* ANY on an instance: SRV and TXT, the address as additional. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("Rourex Garn._HTTP._tcp.local", T_ANY);
    CHECK(parse(handle()) && r.an == 2 && r.ar == 1);
    CHECK(find(0, 2, "rourex garn._http._tcp.local", T_SRV) && find(0, 2, "rourex garn._http._tcp.local", T_TXT));

    /* Two questions, the second one compressed against the first: nothing given twice. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("_telnet._tcp.local", T_PTR);
    q_name("rourex");
    qlen--; /* the name goes on at "local" in the first question: offset 12 + 8 + 5 */
    q_byte(0xc0);
    q_byte(12 + 8 + 5);
    q_tail(T_A, 1);
    CHECK(parse(handle()));
    CHECK(r.an == 2 && r.ar == 2);
    CHECK(find(0, r.an, "rourex.local", T_A) && !find(r.an, r.n, "rourex.local", T_A));
    CHECK(find(r.an, r.n, "rourex tnt._telnet._tcp.local", T_SRV) && find(r.an, r.n, "rourex tnt._telnet._tcp.local", T_TXT));
}

static void test_malformed(void) {
    setup();
    /* A pointer to itself. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_byte(0xc0);
    q_byte(12);
    q_tail(T_A, 1);
    CHECK(handle() == 0);

    /* A label running past the end. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_byte(40);
    q_byte('x');
    qcount = 1;
    CHECK(handle() == 0);

    /* A question cut short before its type. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_name("rourex.local");
    qcount = 1;
    CHECK(handle() == 0);

    /* The 0x40 label type. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_byte(0x41);
    q_byte(0);
    q_tail(T_A, 1);
    CHECK(handle() == 0);

    /* A name of more than 255 bytes. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_byte(63);
    for (int i = 0; i < 63; i++)
        q_byte('a');
    for (int i = 0; i < 6; i++) {
        q_byte(63);
        for (int j = 0; j < 63; j++)
            q_byte('b');
    }
    q_byte(0);
    q_tail(T_A, 1);
    CHECK(handle() == 0);

    /* More questions claimed than there are. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex.local", T_A);
    qcount = 3;
    CHECK(handle() == 0);

    /* IP and UDP lengths that do not agree with the frame. */
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex.local", T_A);
    q_end();
    put16(q + 16, qlen);
    CHECK(mdns_handle(&m, q, qlen, out) == 0);
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex.local", T_A);
    q_end();
    put16(q + 38, qlen);
    CHECK(mdns_handle(&m, q, qlen, out) == 0);
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("rourex.local", T_A);
    q_end();
    put16(q + 20, 0x2000); /* more fragments */
    CHECK(mdns_handle(&m, q, qlen, out) == 0);
    CHECK(mdns_handle(&m, q, 40, out) == 0);
}

static void test_announce(void) {
    setup();
    CHECK(parse(mdns_announce(&m, 0, 0, out)) && to_group());
    CHECK(r.id == 0 && r.flags == 0x8400 && r.an == 1 && r.rr[0].type == T_A && r.rr[0].cls == 0x8001);

    CHECK(parse(mdns_announce(&m, &m.services[1], 0, out)) && to_group());
    CHECK(r.an == 5 && r.ar == 0);
    CHECK(find(0, 5, "_http._tcp.local", T_PTR) && find(0, 5, "rourex streamd._http._tcp.local", T_SRV));
    CHECK(find(0, 5, "rourex streamd._http._tcp.local", T_TXT) && find(0, 5, "rourex.local", T_A));
    const Rr_T *meta = find(0, 5, "_services._dns-sd._udp.local", T_PTR);
    CHECK(meta && !strcmp(meta->target, "_http._tcp.local"));

    /* A goodbye: the service's own records, TTL 0; the address stays. */
    CHECK(parse(mdns_announce(&m, &m.services[2], 1, out)));
    CHECK(r.an == 3);
    for (uint32_t i = 0; i < r.n; i++)
        CHECK(r.rr[i].ttl == 0 && r.rr[i].type != T_A);
    CHECK(mdns_announce(&m, 0, 1, out) == 0);

    /* IPv4 identification moves on with every frame. */
    uint16_t id = m.ip_id;
    mdns_announce(&m, 0, 0, out);
    CHECK(get16(out + 18) == id && m.ip_id == (uint16_t)(id + 1));

    m.ip[0] = m.ip[1] = m.ip[2] = m.ip[3] = 0;
    CHECK(mdns_announce(&m, &m.services[0], 0, out) == 0);
}

static void test_full(void) {
    /* Sixteen services with the longest names there may be: not every PTR fits, but what goes
     * out is whole records in one frame. */
    setup();
    m.n_services = 0;
    char inst[MDNS_LABEL_MAX + 1];
    for (int i = 0; i < MDNS_LABEL_MAX; i++)
        inst[i] = 'x';
    inst[MDNS_LABEL_MAX] = 0;
    for (int i = 0; i < MDNS_MAX_SERVICES; i++) {
        inst[0] = (char)('a' + i);
        add_service((uint16_t)(1000 + i), "_http", inst);
    }
    q_begin(GROUP_IP, 5353, 0, 0);
    q_question("_http._tcp.local", T_PTR);
    uint32_t len = handle();
    CHECK(len > 0 && len <= MDNS_FRAME_MAX && parse(len));
    CHECK(r.an > 8 && r.an < MDNS_MAX_SERVICES && r.ar == 0);
    for (int i = 0; i < MDNS_MAX_SERVICES; i++)
        CHECK(parse(mdns_announce(&m, &m.services[i], 0, out)) && r.an == 5);
}

static uint32_t rng = 0x2545f491;
static uint32_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void test_fuzz(void) {
    /* Mangled queries must never read or write out of bounds (ASan), nor reply with a bad frame. */
    static const char *names[] = {"rourex.local", "_http._tcp.local", "_services._dns-sd._udp.local", "rourex garn._http._tcp.local"};
    static const uint16_t types[] = {T_A, T_PTR, T_ANY, T_SRV};
    for (uint32_t round = 0; round < 200000; round++) {
        setup();
        q_begin(next() & 1 ? GROUP_IP : OUR_IP, next() & 1 ? 5353 : 1024, (uint16_t)next(), 0);
        for (uint32_t n = 1 + next() % 3; n; n--)
            q_question(names[next() % 4], types[next() % 4]);
        q_end();
        uint32_t len = qlen;
        for (uint32_t k = next() % 6; k; k--) {
            uint32_t at = 42 + next() % (len - 42);
            q[at] = (uint8_t)(next() & 3 ? next() : (next() & 1 ? 0xc0 : 0));
        }
        if (next() % 4 == 0)
            len = 42 + next() % (len - 41);
        uint32_t got = mdns_handle(&m, q, len, out);
        if (got)
            CHECK(got <= MDNS_FRAME_MAX && parse(got));
    }
}

int main(void) {
    test_address();
    test_not_ours();
    test_legacy();
    test_browse();
    test_malformed();
    test_announce();
    test_full();
    test_fuzz();
    printf("mdns_test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
