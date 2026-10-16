#include "browse.h"
#include "mdns.h"
#include "net.h"
#include "printf.h"
#include "syscall.h"

/*
 *  mdnsd — Multicast DNS responder for r2
 *
 *  Answers <host>.local with this machine's address, and advertises the r2
 *  servers that are running (DNS-SD): every second it reads the kernel's TCP
 *  port table (syscall 0x46) and publishes the ports it knows below, named
 *  after the process that holds them.  A server that stops is withdrawn.
 *
 *  It also browses for the services of other machines (browse.c) and keeps
 *  the list of them, its own first, in BROWSE_LIST_PATH for Memento.
 *
 *    mdnsd                the host name from sysinfo, as eth sends it to DHCP,
 *                         followed when Settings renames the machine
 *    mdnsd --name lab     lab.local instead
 *    mdnsd debug          reports every answer
 *
 *  Runs beside eth, which owns the address: frames for UDP 5353 come here
 *  (syscall 0x37, arg2 = 2), whether sent to the group or to this host.
 */

/* The servers r2 ships, by the TCP port they listen on.  One string per type,
 * so that services of a type share the pointer. */
typedef struct {
    uint16_t port;
    const char *type;
} Known_T;

static const char HTTP[] = "_http";
static const char TELNET[] = "_telnet";
static const char RNTP[] = "_rntp";

static const Known_T KNOWN[] = {
    {80, HTTP},     /* GARN */
    {8080, HTTP},   /* streamd, chat's web page */
    {23, TELNET},   /* TNT */
    {5555, TELNET}, /* bbs-go */
    {7780, RNTP},   /* an RNTP export */
};

#define N_KNOWN (sizeof(KNOWN) / sizeof(KNOWN[0]))
#define REFRESH_MS 1000u
#define ANNOUNCE_MS 1000u /* between the two announcements (RFC 6762 §8.3) */

static Mdns_T mdns;
static MdnsService_T found[MDNS_MAX_SERVICES];
static uint8_t n_found;
static uint8_t rx[2048];
static uint8_t tx[MDNS_FRAME_MAX];
static uint8_t debug = 0;
static uint8_t fixed_name = 0; /* --name: sysinfo's name is not followed */

static uint8_t announce_left = 0;
static uint64_t announce_at = 0;

static Browse_T browse;
static uint8_t list[BROWSE_LIST_SIZE];   /* the list as it would be written now */
static uint8_t listed[BROWSE_LIST_SIZE]; /* as it was last written */
static uint32_t list_seq;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void bzero_buf(uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        p[i] = 0;
}

static void copy(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        dst[i] = src[i];
}

static uint8_t bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void print_ip(const uint8_t ip[4]) { printf((const uint8_t *)"%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]); }

static void print_label(const uint8_t *s, uint8_t n) {
    for (uint8_t i = 0; i < n; i++)
        printf((const uint8_t *)"%c", s[i]);
}

/* Keeps of <s> what a host name may hold (letters, digits, '-'), up to MDNS_HOST_MAX; "r2" if nothing is left. */
static void set_host(const uint8_t *s, uint32_t max) {
    mdns.host_len = 0;
    for (uint32_t i = 0; i < max && s[i] && mdns.host_len < MDNS_HOST_MAX; i++) {
        uint8_t c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')
            mdns.host[mdns.host_len++] = c;
    }
    if (!mdns.host_len) {
        mdns.host[0] = 'r';
        mdns.host[1] = '2';
        mdns.host_len = 2;
    }
}

static void append(MdnsService_T *s, uint8_t c) {
    if (s->instance_len < MDNS_LABEL_MAX)
        s->instance[s->instance_len++] = c;
}

static void append_num(MdnsService_T *s, uint32_t v) {
    uint8_t digits[10];
    uint32_t n = 0;
    do {
        digits[n++] = (uint8_t)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        append(s, digits[--n]);
}

static uint8_t same_service(const MdnsService_T *a, const MdnsService_T *b) {
    return a->port == b->port && a->type == b->type && a->instance_len == b->instance_len && bytes_eq(a->instance, b->instance, a->instance_len);
}

static void send(uint32_t len) {
    if (len)
        send_eth_frame(tx, len);
}

/* ── Keeping up with the machine ─────────────────────────────────── */

/* "<host> <process>": "rourex garn", the name a service browser lists.  <name> is the
 * binding's, 16 bytes and not always terminated; its extension is left out. */
static void name_service(MdnsService_T *s, const uint8_t *name) {
    s->instance_len = 0;
    for (uint8_t i = 0; i < mdns.host_len; i++)
        append(s, mdns.host[i]);
    append(s, ' ');
    uint8_t had_name = 0;
    for (uint32_t i = 0; i < 16 && name[i] && name[i] != '.' && name[i] != ' '; i++) {
        uint8_t c = name[i];
        append(s, c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c);
        had_name = 1;
    }
    if (!had_name)
        append_num(s, s->port);
}

/* The known servers in the port table now, into found[].  0 when the table could not be read. */
static uint8_t scan_services(void) {
    static NetPortTable_T table;
    if (get_net_ports(&table) != 0)
        return 0;

    n_found = 0;
    for (uint8_t b = 0; b < table.n_ports && b < NET_MAX_PORT_BINDINGS; b++) {
        const NetPortBinding_T *nb = &table.bindings[b];
        for (uint32_t k = 0; k < N_KNOWN && n_found < MDNS_MAX_SERVICES; k++) {
            if (KNOWN[k].port != nb->port)
                continue;
            MdnsService_T *s = &found[n_found];
            s->port = nb->port;
            s->type = KNOWN[k].type;
            name_service(s, nb->name);
            /* One process with two ports of a type (chat): tell them apart by the port. */
            for (uint8_t j = 0; j < n_found; j++) {
                if (found[j].type == s->type && found[j].instance_len == s->instance_len && bytes_eq(found[j].instance, s->instance, s->instance_len)) {
                    append(s, ' ');
                    append_num(s, s->port);
                    break;
                }
            }
            n_found++;
        }
    }
    return 1;
}

static void schedule_announcements(uint64_t now) {
    announce_left = 2;
    announce_at = now;
}

/* Settings can rename the machine (syscall 0x01, 0x04).  The services named after the old name get
 * their goodbyes under it, which their records carry; the next scan names them anew, and the new
 * name is announced. */
static void follow_rename(uint64_t now) {
    SysInfo_T si;
    bzero_buf((uint8_t *)&si, sizeof(si));
    if (fixed_name || !read_sysinfo(&si))
        return;
    uint8_t old[MDNS_HOST_MAX], fresh[MDNS_HOST_MAX];
    uint8_t old_len = mdns.host_len;
    copy(old, mdns.host, old_len);
    set_host(si.system_name, sizeof(si.system_name));
    if (mdns.host_len == old_len && bytes_eq(mdns.host, old, old_len))
        return;

    uint8_t fresh_len = mdns.host_len;
    copy(fresh, mdns.host, fresh_len);
    copy(mdns.host, old, old_len);
    mdns.host_len = old_len;
    for (uint8_t i = 0; i < mdns.n_services; i++)
        send(mdns_announce(&mdns, &mdns.services[i], 1, tx));
    copy(mdns.host, fresh, fresh_len);
    mdns.host_len = fresh_len;
    mdns.n_services = 0;

    print((const uint8_t *)"-> mdnsd: renamed, answering for ");
    print_label(mdns.host, mdns.host_len);
    print((const uint8_t *)".local\n");
    if (mdns.ip[0] | mdns.ip[1] | mdns.ip[2] | mdns.ip[3])
        schedule_announcements(now);
}

/* Writes the list of services when it would say something new.  A write that fails (no /mnt/tmp)
 * is tried again a second later. */
static void publish(void) {
    browse_list(&browse, &mdns, list_seq, list);
    if (bytes_eq(list, listed, BROWSE_LIST_SIZE))
        return;
    browse_list(&browse, &mdns, ++list_seq, list);
    if (write_file_at((const uint8_t *)BROWSE_LIST_PATH, list, 0, BROWSE_LIST_SIZE) != BROWSE_LIST_SIZE)
        return;
    copy(listed, list, BROWSE_LIST_SIZE);
    if (debug)
        printf((const uint8_t *)"-> mdnsd: list %d: %d services found\n", (int32_t)list_seq, (int32_t)browse.n_services);
}

/* Reads the host name, the address and the port table; anything new is announced, and a server
 * gone is withdrawn. */
static void refresh(uint64_t now) {
    follow_rename(now);

    /* The address as eth published it (syscall 0x3d).  Sysinfo holds one from boot on, before
     * there is a card to send anything through; the source stays NONE until eth has set it. */
    NetConfig_T cfg;
    bzero_buf((uint8_t *)&cfg, sizeof(cfg));
    if (get_net_config(&cfg) == 0) {
        if (cfg.source == NET_SOURCE_NONE)
            bzero_buf(cfg.ip, 4);
    } else {
        copy(cfg.ip, mdns.ip, 4);
        copy(cfg.mac, mdns.mac, 6);
    }
    if (!bytes_eq(cfg.ip, mdns.ip, 4) || !bytes_eq(cfg.mac, mdns.mac, 6)) {
        copy(mdns.ip, cfg.ip, 4);
        copy(mdns.mac, cfg.mac, 6);
        browse_start(&browse, now); /* what was found was found from the old address */
        if (mdns.ip[0] | mdns.ip[1] | mdns.ip[2] | mdns.ip[3]) {
            print((const uint8_t *)"-> mdnsd: ");
            print_label(mdns.host, mdns.host_len);
            print((const uint8_t *)".local is ");
            print_ip(mdns.ip);
            print((const uint8_t *)"\n");
            schedule_announcements(now);
        }
    }

    if (!scan_services())
        return;

    uint8_t changed = n_found != mdns.n_services;
    for (uint8_t i = 0; i < mdns.n_services; i++) {
        uint8_t kept = 0;
        for (uint8_t j = 0; j < n_found; j++)
            kept |= same_service(&mdns.services[i], &found[j]);
        if (!kept) {
            send(mdns_announce(&mdns, &mdns.services[i], 1, tx));
            changed = 1;
        }
    }
    if (!changed)
        return;

    copy((uint8_t *)mdns.services, (const uint8_t *)found, n_found * (uint32_t)sizeof(found[0]));
    mdns.n_services = n_found;
    schedule_announcements(now);

    if (debug) {
        for (uint8_t i = 0; i < mdns.n_services; i++) {
            const MdnsService_T *s = &mdns.services[i];
            print((const uint8_t *)"-> mdnsd: ");
            print_label(s->instance, s->instance_len);
            printf((const uint8_t *)".%s._tcp.local port %d\n", s->type, (int32_t)s->port);
        }
    }
}

static void announce(void) {
    send(mdns_announce(&mdns, 0, 0, tx));
    for (uint8_t i = 0; i < mdns.n_services; i++)
        send(mdns_announce(&mdns, &mdns.services[i], 0, tx));
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    bzero_buf((uint8_t *)&mdns, sizeof(mdns)); /* _crt0 leaves BSS as the loader did */
    bzero_buf(listed, sizeof(listed));
    n_found = 0;
    announce_left = 0;
    list_seq = 0;
    browse_start(&browse, get_ticks());

    const uint8_t *name = 0;
    for (int i = 1; i < argc; i++) {
        if (memcmp((uint8_t *)argv[i], (uint8_t *)"debug", 6) == 0) {
            debug = 1;
        } else if (memcmp((uint8_t *)argv[i], (uint8_t *)"--name", 7) == 0 && i + 1 < argc) {
            name = (const uint8_t *)argv[++i];
        } else {
            print((const uint8_t *)"usage: mdnsd [--name <host>] [debug]\n");
            return 1;
        }
    }

    if (name) {
        set_host(name, 0xffffffffu);
        fixed_name = 1;
    } else {
        SysInfo_T si;
        bzero_buf((uint8_t *)&si, sizeof(si));
        read_sysinfo(&si);
        set_host(si.system_name, sizeof(si.system_name));
    }

    int64_t rc = syscall(ScNetRegister, MDNS_PORT, 2, 0);
    if (rc != 0) {
        printf((const uint8_t *)"-> mdnsd: cannot bind UDP %d (0x%x): taken, or a kernel without UDP ports\n", MDNS_PORT, (uint32_t)rc);
        return 1;
    }
    print((const uint8_t *)"-> mdnsd: answering for ");
    print_label(mdns.host, mdns.host_len);
    print((const uint8_t *)".local on UDP 5353\n");

    uint64_t next_refresh = 0;
    for (;;) {
        int64_t n = receive_data_nb(RECV_ETH, rx);
        if (n > 0) {
            uint32_t len = mdns_handle(&mdns, rx, (uint32_t)n, tx);
            if (len && debug) {
                print((const uint8_t *)"-> mdnsd: answered ");
                print_ip(rx + 14 + 12);
                print((const uint8_t *)"\n");
            }
            send(len);
            browse_input(&browse, &mdns, rx, (uint32_t)n, get_ticks());
        } else {
            sleep_ms(20);
        }

        uint64_t now = get_ticks();
        if (now >= next_refresh) {
            refresh(now);
            browse_expire(&browse, now);
            publish();
            next_refresh = now + REFRESH_MS;
        }
        if (announce_left && now >= announce_at) {
            announce();
            announce_left--;
            announce_at = now + ANNOUNCE_MS;
        }
        send(browse_query(&browse, &mdns, now, tx));
    }

    return 0;
}
