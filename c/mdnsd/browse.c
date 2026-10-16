#include "browse.h"

/*
 *  browse.c
 *
 *  A cache of services on the network and the questions that keep it
 *  current.  What a browser could do and this one does not: known-answer
 *  suppression (every responder answers every question, at intervals that
 *  double up to an hour), duplicate question suppression, TXT records, IPv6.
 *  The cache is no bigger than BROWSE_MAX_*: what does not fit is not kept.
 */

#define FIRST_INTERVAL 1000u
#define MAX_INTERVAL 3600000u /* an hour (RFC 6762 §5.2) */
#define BATCH_MS 250u         /* a refresh due this soon goes along with the questions due now */
#define MAX_QUESTIONS 32
#define MAX_TTL_S 86400u
#define GOODBYE_MS 1000u /* a TTL of 0 leaves a record one more second (RFC 6762 §10.1) */
#define REFRESHES 4      /* at 80, 85, 90 and 95 % of a record's TTL */
#define ASKS 3           /* questions for a missing SRV or address before waiting for answers */
#define TRAILER_ROOM 40  /* "# end <seq> <lines>\n" and the zero after it */

/* Service types asked for besides those _services names: r2's own, and ssh. */
static const char *const SEEDS[] = {"_http", "_telnet", "_rntp", "_ssh"};
#define N_SEEDS (sizeof(SEEDS) / sizeof(SEEDS[0]))

static const uint8_t META[] = "\x09_services\x07_dns-sd\x04_udp\x05local";

/* ── Bytes ───────────────────────────────────────────────────────── */

static void zero(void *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        ((uint8_t *)p)[i] = 0;
}

static void copy(void *dst, const void *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        ((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
}

static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }

/* <a> and <b> alike, ASCII case aside, as DNS compares names. */
static uint8_t same(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen) {
    if (alen != blen)
        return 0;
    for (uint32_t i = 0; i < alen; i++)
        if (lower(a[i]) != lower(b[i]))
            return 0;
    return 1;
}

static uint32_t cstr_len(const char *s) {
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static uint8_t is_zero_ip(const uint8_t ip[4]) { return !(ip[0] | ip[1] | ip[2] | ip[3]); }

/* ── Names ───────────────────────────────────────────────────────── */

/* A name in wire format, as mdns_next_record() leaves it, cut into its labels: at most four. */
typedef struct {
    const uint8_t *s[4];
    uint8_t len[4];
    uint8_t n;
} Labels_T;

static uint8_t split(const uint8_t *wire, Labels_T *l) {
    uint32_t p = 0;
    l->n = 0;
    while (wire[p]) {
        if (l->n == 4 || p + 1u + wire[p] >= MDNS_NAME_MAX)
            return 0;
        l->s[l->n] = wire + p + 1;
        l->len[l->n] = wire[p];
        l->n++;
        p += 1u + wire[p];
    }
    return l->n;
}

static uint8_t is(const Labels_T *l, uint8_t i, const char *s) { return same(l->s[i], l->len[i], (const uint8_t *)s, cstr_len(s)); }

/* Whether labels <i> on are a service type, "_x._tcp.local" or "_x._udp.local"; *udp says which. */
static uint8_t type_at(const Labels_T *l, uint8_t i, uint8_t *udp) {
    if (l->n != i + 3 || l->s[i][0] != '_' || !is(l, i + 2, "local"))
        return 0;
    if (is(l, i + 1, "_tcp"))
        *udp = 0;
    else if (is(l, i + 1, "_udp"))
        *udp = 1;
    else
        return 0;
    return 1;
}

static uint32_t wire_len(const uint8_t *name) {
    uint32_t n = 0;
    while (n < MDNS_NAME_MAX && name[n])
        n += 1u + name[n];
    return n + 1;
}

static uint8_t wire_eq(const uint8_t *a, const uint8_t *b) {
    uint32_t p = 0;
    for (;;) {
        if (a[p] != b[p])
            return 0;
        if (!a[p])
            return 1;
        if (!same(a + p + 1, a[p], b + p + 1, b[p]))
            return 0;
        p += 1u + a[p];
        if (p >= BROWSE_TARGET_MAX)
            return 0;
    }
}

static uint32_t put_label(uint8_t *out, uint32_t at, const uint8_t *s, uint32_t len) {
    out[at++] = (uint8_t)len;
    copy(out + at, s, len);
    return at + len;
}

/* <type>._tcp.local, or with <instance> in front, in wire format. */
static void service_wire(const Browse_T *b, uint8_t t, const BrowseService_T *s, uint8_t *out) {
    const BrowseType_T *ty = &b->types[t];
    uint32_t at = 0;
    if (s)
        at = put_label(out, at, s->instance, s->instance_len);
    at = put_label(out, at, ty->name, ty->len);
    at = put_label(out, at, (const uint8_t *)(ty->udp ? "_udp" : "_tcp"), 4);
    at = put_label(out, at, (const uint8_t *)"local", 5);
    out[at] = 0;
}

/* ── The cache ───────────────────────────────────────────────────── */

static uint32_t ttl_ms(uint32_t ttl) { return (ttl > MAX_TTL_S ? MAX_TTL_S : ttl) * 1000u; }

static uint64_t expiry(uint32_t ttl, uint64_t now) { return now + (ttl ? ttl_ms(ttl) : GOODBYE_MS); }

static int find_type(const Browse_T *b, const uint8_t *name, uint8_t len, uint8_t udp) {
    for (uint8_t t = 0; t < b->n_types; t++)
        if (b->types[t].udp == udp && same(b->types[t].name, b->types[t].len, name, len))
            return t;
    return -1;
}

/* The type's index, a new one asked for at once; -1 when the table is full. */
static int add_type(Browse_T *b, const uint8_t *name, uint8_t len, uint8_t udp, uint64_t now) {
    int t = find_type(b, name, len, udp);
    if (t >= 0 || b->n_types == BROWSE_MAX_TYPES)
        return t;
    BrowseType_T *ty = &b->types[b->n_types];
    copy(ty->name, name, len);
    ty->len = len;
    ty->udp = udp;
    ty->ask_at = now;
    ty->interval = FIRST_INTERVAL;
    return b->n_types++;
}

static BrowseService_T *find_service(Browse_T *b, const uint8_t *instance, uint8_t len, int t) {
    for (uint8_t i = 0; i < b->n_services; i++) {
        BrowseService_T *s = &b->services[i];
        if (s->type == t && same(s->instance, s->instance_len, instance, len))
            return s;
    }
    return 0;
}

static const BrowseHost_T *find_host(const Browse_T *b, const uint8_t *name) {
    for (uint8_t i = 0; i < b->n_hosts; i++)
        if (wire_eq(b->hosts[i].name, name))
            return &b->hosts[i];
    return 0;
}

static uint8_t is_target(const Browse_T *b, const uint8_t *name) {
    for (uint8_t i = 0; i < b->n_services; i++)
        if (b->services[i].srv_expires && b->services[i].target[0] && wire_eq(b->services[i].target, name))
            return 1;
    return 0;
}

/* <type>._tcp.local PTR <instance>.<type>._tcp.local, or _services._dns-sd._udp.local PTR <type>._tcp.local */
static void on_ptr(Browse_T *b, const MdnsRecord_T *r, uint64_t now) {
    Labels_T name, target;
    uint8_t udp, target_udp;
    if (!split(r->name, &name) || !split(r->target, &target))
        return;

    if (name.n == 4 && is(&name, 0, "_services") && is(&name, 1, "_dns-sd") && is(&name, 2, "_udp") && is(&name, 3, "local")) {
        if (r->ttl && type_at(&target, 0, &udp))
            add_type(b, target.s[0], target.len[0], udp, now);
        return;
    }

    if (!type_at(&name, 0, &udp) || !type_at(&target, 1, &target_udp) || udp != target_udp || !same(name.s[0], name.len[0], target.s[1], target.len[1]))
        return;
    int t = add_type(b, name.s[0], name.len[0], udp, now);
    if (t < 0)
        return;
    BrowseService_T *s = find_service(b, target.s[0], target.len[0], t);
    if (!s) {
        if (!r->ttl || b->n_services == BROWSE_MAX_SERVICES)
            return;
        s = &b->services[b->n_services++];
        zero(s, sizeof(*s));
        copy(s->instance, target.s[0], target.len[0]);
        s->instance_len = target.len[0];
        s->type = (uint8_t)t;
        s->ask_at = now;
    }
    s->ptr_expires = expiry(r->ttl, now);
    s->ptr_ttl = ttl_ms(r->ttl);
    s->ptr_refreshes = r->ttl ? 0 : REFRESHES;
}

/* <instance>.<type>._tcp.local SRV <priority> <weight> <port> <target> */
static void on_srv(Browse_T *b, const MdnsRecord_T *r, uint64_t now) {
    Labels_T name;
    uint8_t udp;
    if (!split(r->name, &name) || !type_at(&name, 1, &udp))
        return;
    int t = find_type(b, name.s[1], name.len[1], udp);
    BrowseService_T *s = t < 0 ? 0 : find_service(b, name.s[0], name.len[0], t);
    if (!s)
        return;
    uint32_t len = wire_len(r->target);
    if (len <= BROWSE_TARGET_MAX)
        copy(s->target, r->target, len);
    else
        s->target[0] = 0;
    s->port = r->port;
    s->srv_expires = expiry(r->ttl, now);
    s->srv_ttl = ttl_ms(r->ttl);
    s->srv_refreshes = r->ttl ? 0 : REFRESHES;
    s->asks = 0; /* the address may be missing next */
    s->ask_at = now;
}

/* <target> A <address>, for the targets of SRV records only */
static void on_a(Browse_T *b, const MdnsRecord_T *r, uint64_t now) {
    if (wire_len(r->name) > BROWSE_TARGET_MAX || !is_target(b, r->name))
        return;
    BrowseHost_T *h = (BrowseHost_T *)find_host(b, r->name);
    if (!h) {
        if (!r->ttl)
            return;
        if (b->n_hosts < BROWSE_MAX_HOSTS) {
            h = &b->hosts[b->n_hosts++];
        } else {
            /* Full: a host no SRV points at any more makes room. */
            for (uint8_t i = 0; i < b->n_hosts && !h; i++)
                if (!is_target(b, b->hosts[i].name))
                    h = &b->hosts[i];
            if (!h)
                return;
        }
        copy(h->name, r->name, wire_len(r->name));
    }
    copy(h->ip, r->ip, 4);
    h->expires = expiry(r->ttl, now);
}

void browse_start(Browse_T *b, uint64_t now) {
    zero(b, sizeof(*b));
    b->meta_at = now;
    b->meta_interval = FIRST_INTERVAL;
    for (uint32_t i = 0; i < N_SEEDS; i++)
        add_type(b, (const uint8_t *)SEEDS[i], (uint8_t)cstr_len(SEEDS[i]), 0, now);
}

void browse_input(Browse_T *b, const Mdns_T *m, const uint8_t *frame, uint32_t len, uint64_t now) {
    static MdnsRecord_T r;
    MdnsRecords_T first, it;
    if (!mdns_response(m, frame, len, &first))
        return;
    /* PTRs first, which make the services; then their SRVs; then the targets' addresses.  A
     * responder puts them in that order anyway, but an announcement need not. */
    for (uint8_t pass = 0; pass < 3; pass++) {
        it = first;
        while (mdns_next_record(&it, &r)) {
            if (pass == 0 && r.type == MDNS_T_PTR)
                on_ptr(b, &r, now);
            else if (pass == 1 && r.type == MDNS_T_SRV)
                on_srv(b, &r, now);
            else if (pass == 2 && r.type == MDNS_T_A)
                on_a(b, &r, now);
        }
    }
}

void browse_expire(Browse_T *b, uint64_t now) {
    for (uint8_t i = 0; i < b->n_services;) {
        BrowseService_T *s = &b->services[i];
        if (now >= s->ptr_expires) {
            copy(s, &b->services[--b->n_services], sizeof(*s));
            continue;
        }
        if (s->srv_expires && now >= s->srv_expires) {
            s->srv_expires = 0;
            s->target[0] = 0;
            s->port = 0;
            s->asks = 0;
            s->ask_at = now;
        }
        i++;
    }
    for (uint8_t i = 0; i < b->n_hosts;) {
        if (now >= b->hosts[i].expires) {
            copy(&b->hosts[i], &b->hosts[--b->n_hosts], sizeof(b->hosts[i]));
            continue;
        }
        i++;
    }
}

/* ── Asking ──────────────────────────────────────────────────────── */

/* Whether the record that expires at <expires> after <ttl> ms is due its next refresh question. */
static uint8_t refresh_due(uint64_t expires, uint32_t ttl, uint8_t done, uint64_t now) {
    return expires && done < REFRESHES && now + BATCH_MS >= expires - (uint64_t)ttl * (20u - 5u * done) / 100u;
}

/* The address of <s>'s target, if it is known and alive. */
static const BrowseHost_T *address_of(const Browse_T *b, const BrowseService_T *s) {
    return s->srv_expires && s->target[0] ? find_host(b, s->target) : 0;
}

enum { Q_META, Q_TYPE, Q_SRV, Q_A };

/* What a question in the frame stands for, to be marked asked once it has gone. */
typedef struct {
    uint8_t kind, index;
} Asked_T;

uint32_t browse_query(Browse_T *b, Mdns_T *m, uint64_t now, uint8_t *out) {
    static uint8_t names[MAX_QUESTIONS][MDNS_NAME_MAX];
    MdnsQuestion_T q[MAX_QUESTIONS];
    Asked_T asked[MAX_QUESTIONS];
    uint32_t n = 0;

    if (is_zero_ip(m->ip))
        return 0;

    if (now >= b->meta_at) {
        q[n].name = META;
        q[n].type = MDNS_T_PTR;
        asked[n].kind = Q_META;
        asked[n++].index = 0;
    }

    for (uint8_t t = 0; t < b->n_types && n < MAX_QUESTIONS; t++) {
        uint8_t due = now >= b->types[t].ask_at; /* never early: at least 1 s between the first two (RFC 6762 §5.2) */
        for (uint8_t i = 0; i < b->n_services && !due; i++) {
            const BrowseService_T *s = &b->services[i];
            due = s->type == t && (refresh_due(s->ptr_expires, s->ptr_ttl, s->ptr_refreshes, now) ||
                                   refresh_due(s->srv_expires, s->srv_ttl, s->srv_refreshes, now));
        }
        if (!due)
            continue;
        service_wire(b, t, 0, names[n]);
        q[n].name = names[n];
        q[n].type = MDNS_T_PTR;
        asked[n].kind = Q_TYPE;
        asked[n++].index = t;
    }

    for (uint8_t i = 0; i < b->n_services && n < MAX_QUESTIONS; i++) {
        BrowseService_T *s = &b->services[i];
        if (s->asks >= ASKS || now < s->ask_at || !s->ptr_ttl)
            continue;
        if (!s->srv_expires) {
            service_wire(b, s->type, s, names[n]);
            q[n].name = names[n];
            q[n].type = MDNS_T_SRV;
            asked[n].kind = Q_SRV;
        } else if (s->target[0] && !address_of(b, s)) {
            /* One question per target, whatever number of its services wait for it. */
            uint32_t j = 0;
            while (j < n && !(q[j].type == MDNS_T_A && q[j].name && wire_eq(q[j].name, s->target)))
                j++;
            q[n].name = j < n ? 0 : s->target;
            q[n].type = MDNS_T_A;
            asked[n].kind = Q_A;
        } else {
            continue;
        }
        asked[n++].index = i;
    }

    /* Questions shared by several services are in the frame once. */
    MdnsQuestion_T send[MAX_QUESTIONS];
    uint32_t sent_of[MAX_QUESTIONS], n_send = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (q[i].name)
            send[n_send++] = q[i];
        sent_of[i] = n_send; /* question i went once the first sent_of[i] questions did */
    }
    if (!n_send)
        return 0;
    uint32_t fitted = n_send;
    uint32_t len = mdns_query(m, send, &fitted, out);
    if (!len)
        return 0;

    for (uint32_t i = 0; i < n && sent_of[i] <= fitted; i++) {
        if (asked[i].kind == Q_META) {
            b->meta_at = now + b->meta_interval;
            b->meta_interval = b->meta_interval * 2 > MAX_INTERVAL ? MAX_INTERVAL : b->meta_interval * 2;
        } else if (asked[i].kind == Q_TYPE) {
            uint8_t t = asked[i].index;
            BrowseType_T *ty = &b->types[t];
            if (now >= ty->ask_at) {
                ty->ask_at = now + ty->interval;
                ty->interval = ty->interval * 2 > MAX_INTERVAL ? MAX_INTERVAL : ty->interval * 2;
            }
            for (uint8_t j = 0; j < b->n_services; j++) {
                BrowseService_T *s = &b->services[j];
                if (s->type != t)
                    continue;
                if (refresh_due(s->ptr_expires, s->ptr_ttl, s->ptr_refreshes, now))
                    s->ptr_refreshes++;
                if (refresh_due(s->srv_expires, s->srv_ttl, s->srv_refreshes, now))
                    s->srv_refreshes++;
            }
        } else {
            BrowseService_T *s = &b->services[asked[i].index];
            s->asks++;
            s->ask_at = now + ((uint64_t)FIRST_INTERVAL << s->asks);
        }
    }
    return len;
}

/* ── The list ────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    uint32_t len, cap;
    uint8_t full;
} Text_T;

static void put(Text_T *o, uint8_t c) {
    if (o->len < o->cap)
        o->buf[o->len++] = c;
    else
        o->full = 1;
}

static void put_str(Text_T *o, const char *s) {
    while (*s)
        put(o, (uint8_t)*s++);
}

static void put_num(Text_T *o, uint32_t v) {
    uint8_t digits[10];
    uint32_t n = 0;
    do {
        digits[n++] = (uint8_t)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        put(o, digits[--n]);
}

/* A label as text: printable ASCII as it is, '?' for anything else, once per UTF-8 character. */
static void put_label_text(Text_T *o, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c >= 0x80 && c < 0xc0)
            continue;
        put(o, c >= 0x20 && c < 0x7f ? c : '?');
    }
}

static void put_ip(Text_T *o, const uint8_t ip[4]) {
    if (is_zero_ip(ip))
        return;
    for (uint32_t i = 0; i < 4; i++) {
        if (i)
            put(o, '.');
        put_num(o, ip[i]);
    }
}

/* One line, or none when it does not fit. */
static uint8_t put_line(Text_T *o, const uint8_t *instance, uint32_t instance_len, const uint8_t *type, uint32_t type_len, uint8_t udp,
                        const uint8_t *target, const uint8_t ip[4], uint16_t port) {
    uint32_t start = o->len;
    put_label_text(o, instance, instance_len);
    put(o, '\t');
    put_label_text(o, type, type_len);
    put_str(o, udp ? "._udp\t" : "._tcp\t");
    for (uint32_t p = 0; target[p] && p < BROWSE_TARGET_MAX; p += 1u + target[p]) {
        if (p)
            put(o, '.');
        put_label_text(o, target + p + 1, target[p]);
    }
    put(o, '\t');
    put_ip(o, ip);
    put(o, '\t');
    put_num(o, port);
    put(o, '\n');
    if (o->full) {
        zero(o->buf + start, o->len - start); /* the zero padding after the end line */
        o->len = start;
        return 0;
    }
    return 1;
}

uint32_t browse_list(const Browse_T *b, const Mdns_T *m, uint32_t seq, uint8_t *out) {
    static const uint8_t NO_IP[4] = {0, 0, 0, 0};
    zero(out, BROWSE_LIST_SIZE);
    Text_T o = {out, 0, BROWSE_LIST_SIZE - TRAILER_ROOM, 0};
    put_str(&o, "# mdnsd ");
    put_num(&o, seq);
    put(&o, '\n');

    uint32_t lines = 0;
    uint8_t own[MDNS_HOST_MAX + 8];
    uint32_t at = put_label(own, 0, m->host, m->host_len);
    at = put_label(own, at, (const uint8_t *)"local", 5);
    own[at] = 0;
    for (uint8_t i = 0; i < m->n_services && !o.full; i++) {
        const MdnsService_T *s = &m->services[i];
        lines += put_line(&o, s->instance, s->instance_len, (const uint8_t *)s->type, cstr_len(s->type), 0, own, m->ip, s->port);
    }
    for (uint8_t i = 0; i < b->n_services && !o.full; i++) {
        const BrowseService_T *s = &b->services[i];
        if (!s->srv_expires || !s->srv_ttl || !s->ptr_ttl)
            continue; /* unresolved, or said goodbye */
        const BrowseType_T *ty = &b->types[s->type];
        const BrowseHost_T *h = address_of(b, s);
        lines += put_line(&o, s->instance, s->instance_len, ty->name, ty->len, ty->udp, s->target, h ? h->ip : NO_IP, s->port);
    }

    o.cap = BROWSE_LIST_SIZE - 1;
    o.full = 0;
    put_str(&o, "# end ");
    put_num(&o, seq);
    put(&o, ' ');
    put_num(&o, lines);
    put(&o, '\n');
    return o.len;
}
