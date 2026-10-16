/* Host tests for mdnsd's browser: queries out, a responder's answers in (our own mdns.c as the
 * other machine, and frames written by hand where a responder may differ), the list out, the TTLs
 * run down; then a fuzz pass over mangled answers.  make sanitizer-test runs it with ASan and
 * UBSan; no r2 kernel is needed. */
#include "../browse.h"

extern int printf(const char *, ...);
extern char *strstr(const char *, const char *);
extern unsigned long strlen(const char *);

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
#define T_SRV 33

static const uint8_t OUR_IP[4] = {10, 3, 4, 2};
static const uint8_t OUR_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8_t PEER_IP[4] = {10, 3, 4, 1};
static const uint8_t PEER_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t GROUP_IP[4] = {224, 0, 0, 251};
static const uint8_t GROUP_MAC[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb};

static Mdns_T us, peer;
static Browse_T b;
static uint8_t frame[MDNS_FRAME_MAX], answer[MDNS_FRAME_MAX];
static uint8_t list[BROWSE_LIST_SIZE];

static void zero(void *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        ((uint8_t *)p)[i] = 0;
}

static void set_label(uint8_t *dst, uint8_t *len, const char *s) {
    *len = 0;
    while (s[*len]) {
        dst[*len] = (uint8_t)s[*len];
        (*len)++;
    }
}

static void machine(Mdns_T *m, const char *host, const uint8_t ip[4], const uint8_t mac[6]) {
    zero(m, sizeof(*m));
    set_label(m->host, &m->host_len, host);
    for (int i = 0; i < 4; i++)
        m->ip[i] = ip[i];
    for (int i = 0; i < 6; i++)
        m->mac[i] = mac[i];
}

static void add_service(Mdns_T *m, uint16_t port, const char *type, const char *instance) {
    MdnsService_T *s = &m->services[m->n_services++];
    s->port = port;
    s->type = type;
    set_label(s->instance, &s->instance_len, instance);
}

static void setup(void) {
    machine(&us, "rourex", OUR_IP, OUR_MAC);
    add_service(&us, 80, "_http", "rourex garn");
    machine(&peer, "lab", PEER_IP, PEER_MAC);
    add_service(&peer, 80, "_http", "lab garn");
    add_service(&peer, 23, "_telnet", "lab tnt");
    browse_start(&b, 0);
}

static const char *listed(void) {
    browse_list(&b, &us, 7, list);
    return (const char *)list;
}

static uint8_t has(const char *line) { return strstr(listed(), line) != 0; }

/* ── Reading our queries ─────────────────────────────────────────── */

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* Whether the query in frame[0..len) asks <dotted> <type>.  Queries are written uncompressed. */
static uint8_t asks(uint32_t len, const char *dotted, uint16_t type) {
    if (len < 54)
        return 0;
    const uint8_t *msg = frame + 42;
    uint32_t off = 12;
    for (uint32_t q = get16(msg + 4); q; q--) {
        char name[256];
        uint32_t n = 0;
        while (msg[off]) {
            if (n)
                name[n++] = '.';
            for (uint8_t i = 0; i < msg[off]; i++)
                name[n++] = (char)msg[off + 1 + i];
            off += 1u + msg[off];
        }
        name[n] = 0;
        off++;
        uint16_t qtype = get16(msg + off);
        off += 4;
        uint32_t i = 0;
        while (dotted[i] && dotted[i] == name[i])
            i++;
        if (!dotted[i] && !name[i] && qtype == type)
            return 1;
    }
    return 0;
}

static uint16_t questions(uint32_t len) { return len < 54 ? 0 : get16(frame + 42 + 4); }

/* Our query answered by the peer's responder, and the answer taken in. */
static uint32_t ask_peer(uint64_t now) {
    uint32_t len = browse_query(&b, &us, now, frame);
    if (!len)
        return 0;
    uint32_t alen = mdns_handle(&peer, frame, len, answer);
    if (alen)
        browse_input(&b, &us, answer, alen, now);
    return len;
}

/* ── Answers written by hand ─────────────────────────────────────── */

static uint8_t a[MDNS_FRAME_MAX];
static uint32_t alen;
static uint16_t acount;

static void a_begin(const uint8_t src_ip[4], uint16_t sport, uint16_t flags) {
    zero(a, sizeof(a));
    for (int i = 0; i < 6; i++) {
        a[i] = GROUP_MAC[i];
        a[6 + i] = PEER_MAC[i];
    }
    a[12] = 0x08;
    a[14] = 0x45;
    a[22] = 255;
    a[23] = 17;
    for (int i = 0; i < 4; i++) {
        a[26 + i] = src_ip[i];
        a[30 + i] = GROUP_IP[i];
    }
    put16(a + 34, sport);
    put16(a + 36, 5353);
    put16(a + 44, flags);
    alen = 54;
    acount = 0;
}

static void a_byte(uint8_t v) { a[alen++] = v; }

/* <dotted> as labels; a part "@<offset>" ends the name with a pointer to that offset in the message. */
static void a_name(const char *dotted) {
    while (*dotted) {
        if (*dotted == '@') {
            uint32_t off = 0;
            for (dotted++; *dotted >= '0' && *dotted <= '9'; dotted++)
                off = off * 10 + (uint32_t)(*dotted - '0');
            a_byte((uint8_t)(0xc0 | off >> 8));
            a_byte((uint8_t)off);
            return;
        }
        uint32_t at = alen++;
        uint8_t n = 0;
        while (*dotted && *dotted != '.') {
            a_byte((uint8_t)*dotted++);
            n++;
        }
        a[at] = n;
        if (*dotted)
            dotted++;
    }
    a_byte(0);
}

static uint32_t rd_at;
static void a_rr(const char *name, uint16_t type, uint16_t cls, uint32_t ttl) {
    a_name(name);
    put16(a + alen, type);
    put16(a + alen + 2, cls);
    put16(a + alen + 4, ttl >> 16);
    put16(a + alen + 6, ttl);
    alen += 10;
    rd_at = alen;
    acount++;
}

static void a_rr_end(void) { put16(a + rd_at - 2, alen - rd_at); }

static void a_ptr(const char *name, const char *target, uint32_t ttl) {
    a_rr(name, T_PTR, 1, ttl);
    a_name(target);
    a_rr_end();
}

static void a_srv(const char *name, uint16_t port, const char *target, uint32_t ttl) {
    a_rr(name, T_SRV, 0x8001, ttl);
    a_byte(0);
    a_byte(0);
    a_byte(0);
    a_byte(0);
    a_byte((uint8_t)(port >> 8));
    a_byte((uint8_t)port);
    a_name(target);
    a_rr_end();
}

static void a_a(const char *name, uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3, uint32_t ttl) {
    a_rr(name, T_A, 0x8001, ttl);
    a_byte(a0);
    a_byte(a1);
    a_byte(a2);
    a_byte(a3);
    a_rr_end();
}

static uint32_t a_end(void) {
    put16(a + 16, alen - 14);
    put16(a + 38, alen - 34);
    put16(a + 48, acount); /* all in the answer section */
    return alen;
}

static void a_send(uint64_t now) { browse_input(&b, &us, a, a_end(), now); }

/* ── Tests ───────────────────────────────────────────────────────── */

static void test_first_query(void) {
    setup();
    uint32_t len = browse_query(&b, &us, 0, frame);
    CHECK(len > 54);
    CHECK(frame[0] == 0x01 && frame[5] == 0xfb);                    /* to the group */
    CHECK(frame[30] == 224 && frame[33] == 251 && frame[22] == 255); /* IP TTL 255 */
    CHECK(get16(frame + 34) == 5353 && get16(frame + 36) == 5353);
    CHECK(get16(frame + 44) == 0); /* a query */
    CHECK(asks(len, "_services._dns-sd._udp.local", T_PTR));
    CHECK(asks(len, "_http._tcp.local", T_PTR) && asks(len, "_telnet._tcp.local", T_PTR));
    CHECK(asks(len, "_rntp._tcp.local", T_PTR) && asks(len, "_ssh._tcp.local", T_PTR));
    CHECK(questions(len) == 5);
    /* Asked: nothing more until a second later, then again after 2 s, 4 s ... */
    CHECK(!browse_query(&b, &us, 500, frame));
    CHECK(!browse_query(&b, &us, 999, frame)); /* never early: at least a second (RFC 6762 §5.2) */
    CHECK(questions(browse_query(&b, &us, 1000, frame)) == 5);
    CHECK(!browse_query(&b, &us, 2500, frame));
    CHECK(questions(browse_query(&b, &us, 3000, frame)) == 5);
    CHECK(questions(browse_query(&b, &us, 7000, frame)) == 5);

    /* No address, no query. */
    setup();
    zero(us.ip, 4);
    CHECK(!browse_query(&b, &us, 0, frame));
}

static void test_found(void) {
    setup();
    CHECK(ask_peer(0));
    browse_expire(&b, 0);
    const char *l = listed();
    CHECK(strstr(l, "# mdnsd 7\n") == l);
    CHECK(has("rourex garn\t_http._tcp\trourex.local\t10.3.4.2\t80\n"));
    CHECK(has("lab garn\t_http._tcp\tlab.local\t10.3.4.1\t80\n"));
    CHECK(has("lab tnt\t_telnet._tcp\tlab.local\t10.3.4.1\t23\n"));
    CHECK(has("# end 7 3\n"));
    CHECK(strstr(l, "rourex garn") < strstr(l, "lab garn")); /* our own first */
    CHECK(list[BROWSE_LIST_SIZE - 1] == 0 && strlen(l) < BROWSE_LIST_SIZE);

    /* A goodbye takes it off the list at once, and out of the cache a second later. */
    browse_input(&b, &us, answer, mdns_announce(&peer, &peer.services[1], 1, answer), 5000);
    CHECK(!has("lab tnt") && has("lab garn") && has("# end 7 2\n"));
    CHECK(b.n_services == 2);
    browse_expire(&b, 6000);
    CHECK(b.n_services == 1);

    /* A server that starts later announces itself. */
    add_service(&peer, 8080, "_http", "lab streamd");
    browse_input(&b, &us, answer, mdns_announce(&peer, &peer.services[2], 0, answer), 7000);
    CHECK(has("lab streamd\t_http._tcp\tlab.local\t10.3.4.1\t8080\n"));
}

/* The doubling schedule up to 127 s, all answered: the next scheduled question is at 255 s. */
static void ask_until_127s(void) {
    CHECK(ask_peer(0));
    for (uint64_t t = 1000; t <= 127000; t = t * 2 + 1000)
        CHECK(ask_peer(t));
}

static void test_refresh(void) {
    setup();
    ask_until_127s();
    /* 120 s SRV and A records from t = 127 s: refreshed at 80 % (223 s) ... */
    CHECK(!browse_query(&b, &us, 222000, frame));
    uint32_t len = browse_query(&b, &us, 223000, frame);
    CHECK(asks(len, "_http._tcp.local", T_PTR) && asks(len, "_telnet._tcp.local", T_PTR));
    CHECK(!asks(len, "_rntp._tcp.local", T_PTR)); /* nothing to refresh there */
    /* ... 85 %, 90 %, 95 %, unanswered ... */
    CHECK(!browse_query(&b, &us, 224000, frame));
    CHECK(asks(browse_query(&b, &us, 229000, frame), "_http._tcp.local", T_PTR));
    CHECK(asks(browse_query(&b, &us, 235000, frame), "_http._tcp.local", T_PTR));
    CHECK(asks(browse_query(&b, &us, 241000, frame), "_http._tcp.local", T_PTR));
    CHECK(!browse_query(&b, &us, 242000, frame));
    /* ... so the SRV expires at 247 s: off the list, and its SRV asked for. */
    browse_expire(&b, 246000);
    CHECK(has("lab garn"));
    browse_expire(&b, 247000);
    CHECK(!has("lab garn") && has("rourex garn"));
    len = browse_query(&b, &us, 247000, frame);
    CHECK(asks(len, "lab garn._http._tcp.local", T_SRV));
    /* The peer answers that, and it is back. */
    browse_input(&b, &us, answer, mdns_handle(&peer, frame, len, answer), 247000);
    CHECK(has("lab garn\t_http._tcp\tlab.local\t10.3.4.1\t80\n"));

    /* An answered refresh keeps it past 247 s, and the next refresh is 80 % of the new TTL on. */
    setup();
    ask_until_127s();
    CHECK(ask_peer(223000));
    browse_expire(&b, 250000);
    CHECK(has("lab garn") && has("lab tnt"));
    CHECK(!browse_query(&b, &us, 254000, frame));
    CHECK(ask_peer(255000)); /* scheduled, renews to 375 s */
    CHECK(!browse_query(&b, &us, 350000, frame));
    CHECK(asks(browse_query(&b, &us, 351000, frame), "_http._tcp.local", T_PTR));
}

static void test_missing(void) {
    setup();
    /* A PTR alone: not listed until its SRV comes, which is asked for. */
    a_begin(PEER_IP, 5353, 0x8400);
    a_ptr("_http._tcp.local", "box web._http._tcp.local", 4500);
    a_send(0);
    CHECK(!has("box web"));
    uint32_t len = browse_query(&b, &us, 0, frame);
    CHECK(asks(len, "box web._http._tcp.local", T_SRV));
    CHECK(!asks(browse_query(&b, &us, 1000, frame), "box web._http._tcp.local", T_SRV)); /* 2 s later */
    CHECK(asks(browse_query(&b, &us, 2000, frame), "box web._http._tcp.local", T_SRV));

    /* The SRV without an address: listed without one, and the address asked for, once for both. */
    a_begin(PEER_IP, 5353, 0x8400);
    a_srv("box web._http._tcp.local", 8000, "box.local", 120);
    a_ptr("_ssh._tcp.local", "box ssh._ssh._tcp.local", 4500);
    a_srv("box ssh._ssh._tcp.local", 22, "Box.Local", 120);
    a_send(2500);
    CHECK(has("box web\t_http._tcp\tbox.local\t\t8000\n"));
    CHECK(has("box ssh\t_ssh._tcp\tBox.Local\t\t22\n"));
    len = browse_query(&b, &us, 2500, frame);
    CHECK(asks(len, "box.local", T_A) && !asks(len, "Box.Local", T_A));

    /* An address for a name nobody's SRV points at is not kept; the target's is, case aside. */
    a_begin(PEER_IP, 5353, 0x8400);
    a_a("other.local", 10, 3, 4, 9, 120);
    a_a("BOX.local", 10, 3, 4, 7, 120);
    a_send(3000);
    CHECK(b.n_hosts == 1);
    CHECK(has("box web\t_http._tcp\tbox.local\t10.3.4.7\t8000\n"));
    CHECK(has("box ssh\t_ssh._tcp\tBox.Local\t10.3.4.7\t22\n"));
}

static void test_types(void) {
    setup();
    /* _services names a type nobody asked for: it is asked for next, as a PTR to it adds it too. */
    a_begin(PEER_IP, 5353, 0x8400);
    a_ptr("_services._dns-sd._udp.local", "_ipp._tcp.local", 4500);
    a_ptr("_services._dns-sd._udp.local", "_sleep-proxy._udp.local", 4500);
    a_ptr("_services._dns-sd._udp.local", "not-a-type.local", 4500);
    a_ptr("_printer._sub._http._tcp.local", "x._http._tcp.local", 4500); /* a subtype: not browsed */
    a_send(0);
    uint32_t len = browse_query(&b, &us, 0, frame);
    CHECK(asks(len, "_ipp._tcp.local", T_PTR) && asks(len, "_sleep-proxy._udp.local", T_PTR));
    CHECK(b.n_types == 6 && b.n_services == 0);

    a_begin(PEER_IP, 5353, 0x8400);
    a_ptr("_sleep-proxy._udp.local", "70-35-60-63 Bedroom._sleep-proxy._udp.local", 4500);
    a_srv("70-35-60-63 Bedroom._sleep-proxy._udp.local", 50000, "Bedroom.local", 120);
    a_ptr("_ipp._tcp.local", "x._http._tcp.local", 4500); /* an instance of another type */
    a_send(100);
    CHECK(has("70-35-60-63 Bedroom\t_sleep-proxy._udp\tBedroom.local\t\t50000\n"));
    CHECK(b.n_services == 1);
}

/* "@<off>": a name that is only a pointer to <off> in the DNS message. */
static void pointer_to(char *out, uint32_t off) {
    char digits[8];
    uint32_t k = 0, n = 0;
    do {
        digits[k++] = (char)('0' + off % 10);
        off /= 10;
    } while (off);
    out[n++] = '@';
    while (k)
        out[n++] = digits[--k];
    out[n] = 0;
}

static void test_compressed(void) {
    char name[16], target[16];
    setup();
    /* As Avahi writes them: every name after the first points back into the message.  The type's
     * name is at 12, right after the header, and its "local" at 23. */
    a_begin(PEER_IP, 5353, 0x8400);
    a_ptr("_http._tcp.local", "Caf\xc3\xa9\tweb.@12", 120);
    pointer_to(name, rd_at - 42);
    a_srv(name, 81, "pi.@23", 120);
    pointer_to(target, rd_at + 6 - 42);
    a_a(target, 192, 168, 1, 20, 120);
    a_send(0);
    /* Non-ASCII is one '?' per character, and a tab cannot break the line. */
    CHECK(has("Caf??web\t_http._tcp\tpi.local\t192.168.1.20\t81\n"));
}

static void test_ignored(void) {
    setup();
    /* From our own address, from a port other than 5353, a query, to another host, another class. */
    a_begin(OUR_IP, 5353, 0x8400);
    a_ptr("_http._tcp.local", "a._http._tcp.local", 120);
    a_send(0);
    a_begin(PEER_IP, 1234, 0x8400);
    a_ptr("_http._tcp.local", "b._http._tcp.local", 120);
    a_send(0);
    a_begin(PEER_IP, 5353, 0x0000);
    a_ptr("_http._tcp.local", "c._http._tcp.local", 120);
    a_send(0);
    a_begin(PEER_IP, 5353, 0x8400);
    a[33] = 77;
    a_ptr("_http._tcp.local", "d._http._tcp.local", 120);
    a_send(0);
    a_begin(PEER_IP, 5353, 0x8400);
    a_rr("_http._tcp.local", T_PTR, 3, 120); /* CHAOS */
    a_name("e._http._tcp.local");
    a_rr_end();
    a_ptr("_http._tcp.local", "f._http._tcp.local", 120);
    a_send(0);
    CHECK(b.n_services == 1 && b.services[0].instance[0] == 'f');

    /* Sent to us alone, it counts. */
    a_begin(PEER_IP, 5353, 0x8400);
    for (int i = 0; i < 4; i++)
        a[30 + i] = OUR_IP[i];
    a_ptr("_http._tcp.local", "g._http._tcp.local", 120);
    a_send(0);
    CHECK(b.n_services == 2);

    /* A record that runs past the message ends it. */
    a_begin(PEER_IP, 5353, 0x8400);
    a_ptr("_http._tcp.local", "h._http._tcp.local", 120);
    put16(a + rd_at - 2, 200);
    a_send(0);
    CHECK(b.n_services == 2);
}

static void test_full(void) {
    static const char TYPE[] = "_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa._tcp.local";
    static const char TARGET[] = "a-host-name-as-long-as-the-cache-keeps-a-host-name-as-lo.local"; /* 64 bytes in wire format */
    setup();
    /* More instances than the cache holds, with lines longer than the list has room for: it keeps
     * to its size, and the lines it has are whole, as their count says. */
    for (int i = 0; i < 60; i++) {
        char name[200];
        int n = 0;
        for (; n < 60; n++)
            name[n] = (char)('a' + (n + i) % 26);
        name[n++] = (char)('0' + i / 10);
        name[n++] = (char)('0' + i % 10);
        name[n++] = '.';
        for (const char *p = TYPE; *p; p++)
            name[n++] = *p;
        name[n] = 0;
        a_begin(PEER_IP, 5353, 0x8400);
        a_ptr(TYPE, name, 4500);
        a_srv(name, 80, TARGET, 120);
        a_send(0);
    }
    CHECK(b.n_services == BROWSE_MAX_SERVICES);
    const char *l = listed();
    CHECK(list[BROWSE_LIST_SIZE - 1] == 0 && strlen(l) < BROWSE_LIST_SIZE);
    const char *end = strstr(l, "# end 7 ");
    CHECK(end && end[strlen(end) - 1] == '\n');
    uint32_t lines = 0, count = 0;
    for (const char *p = end ? end + 8 : "0"; *p >= '0' && *p <= '9'; p++)
        count = count * 10 + (uint32_t)(*p - '0');
    for (const char *p = l; end && p < end; p++)
        lines += *p == '\n';
    CHECK(lines == count + 1); /* and the first line */
    CHECK(count > 30 && count < 1 + BROWSE_MAX_SERVICES);
    CHECK(strlen(TARGET) + 2 <= BROWSE_TARGET_MAX);
}

/* ── Fuzz ────────────────────────────────────────────────────────── */

static uint32_t seed = 12345;
static uint32_t next(void) {
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
}

static void test_fuzz(void) {
    static uint8_t mangled[MDNS_FRAME_MAX];
    setup();
    add_service(&peer, 8080, "_http", "lab streamd");
    uint32_t len = browse_query(&b, &us, 0, frame);
    uint32_t good = mdns_handle(&peer, frame, len, answer);
    CHECK(good > 0);
    for (uint32_t round = 0; round < 20000; round++) {
        uint32_t n = good;
        for (uint32_t i = 0; i < n; i++)
            mangled[i] = answer[i];
        for (uint32_t k = 1 + next() % 6; k; k--)
            mangled[42 + next() % (n - 42)] = (uint8_t)next();
        if (next() % 4 == 0)
            n = 42 + next() % (n - 42);
        uint64_t now = (uint64_t)round * 37;
        browse_input(&b, &us, mangled, n, now);
        browse_query(&b, &us, now, frame);
        browse_expire(&b, now);
        CHECK(b.n_services <= BROWSE_MAX_SERVICES && b.n_types <= BROWSE_MAX_TYPES && b.n_hosts <= BROWSE_MAX_HOSTS);
        browse_list(&b, &us, round, list);
        CHECK(list[BROWSE_LIST_SIZE - 1] == 0);
    }
}

int main(void) {
    test_first_query();
    test_found();
    test_refresh();
    test_missing();
    test_types();
    test_compressed();
    test_ignored();
    test_full();
    test_fuzz();
    printf("%d of %d browse checks passed\n", checks - failures, checks);
    return failures != 0;
}
