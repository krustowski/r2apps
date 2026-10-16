#ifndef _MDNSD_BROWSE_INCLUDED_
#define _MDNSD_BROWSE_INCLUDED_

/*
 *  browse.h
 *
 *  The querier half: DNS-SD browsing (RFC 6763 §4, §9) for every service on
 *  the network.  It asks _services._dns-sd._udp.local for the service types
 *  there are, and each type for its instances, and keeps what the answers
 *  say for as long as their TTLs allow (RFC 6762 §5.2, §10).  Answers go to
 *  the group, so those to other machines' questions count as well.  No
 *  syscalls here either, so the host tests run it as it is.
 */

#include "mdns.h"

#define BROWSE_MAX_TYPES 24
#define BROWSE_MAX_SERVICES 48
#define BROWSE_MAX_HOSTS 48
#define BROWSE_TARGET_MAX 64 /* an SRV target longer than this in wire format goes unresolved */

/*
 *  The list for other programs, such as Memento's Network window: a text
 *  file of BROWSE_LIST_SIZE bytes, zero-padded, rewritten whole whenever it
 *  changes.
 *
 *      # mdnsd <seq>
 *      <instance> TAB <type> TAB <host> TAB <IPv4> TAB <port>
 *      ...
 *      # end <seq> <lines>
 *
 *  <type> is "_http._tcp" and the like, <host> the SRV target without its
 *  final dot ("rourex.local"), and <IPv4> empty while the target has no
 *  address.  This host's own services come first.  Bytes other than
 *  printable ASCII are '?'.  A reader that does not find an end line with
 *  the first line's <seq> read the file while it was being rewritten.
 */
#define BROWSE_LIST_PATH "/mnt/tmp/MDNS.TXT"
#define BROWSE_LIST_SIZE 8192

typedef struct {
    uint8_t name[MDNS_LABEL_MAX]; /* "_http" */
    uint8_t len;
    uint8_t udp;       /* _udp rather than _tcp */
    uint64_t ask_at;   /* when to ask for its instances again */
    uint32_t interval; /* ms until the question after that one */
} BrowseType_T;

/*
 *  An instance a PTR told of.  It is listed once an SRV has said where it is,
 *  and dropped when the PTR expires.  Each record is asked for again at 80,
 *  85, 90 and 95 % of its TTL (RFC 6762 §5.2) until an answer renews it.
 */
typedef struct {
    uint8_t instance[MDNS_LABEL_MAX];
    uint8_t instance_len;
    uint8_t type;                      /* in types[] */
    uint8_t target[BROWSE_TARGET_MAX]; /* the SRV's, in wire format; empty when too long */
    uint16_t port;
    uint64_t ptr_expires, srv_expires; /* srv_expires 0 while there is no SRV */
    uint32_t ptr_ttl, srv_ttl;         /* ms; 0 after a goodbye */
    uint8_t ptr_refreshes, srv_refreshes;
    uint8_t asks;    /* questions for its missing SRV or address since the last answer */
    uint64_t ask_at; /* when the next of those may go */
} BrowseService_T;

/* An A record for an SRV target. */
typedef struct {
    uint8_t name[BROWSE_TARGET_MAX];
    uint8_t ip[4];
    uint64_t expires;
} BrowseHost_T;

typedef struct {
    BrowseType_T types[BROWSE_MAX_TYPES];
    BrowseService_T services[BROWSE_MAX_SERVICES];
    BrowseHost_T hosts[BROWSE_MAX_HOSTS];
    uint8_t n_types, n_services, n_hosts;
    uint64_t meta_at; /* when to ask for the types again */
    uint32_t meta_interval;
} Browse_T;

/*
 *  void browse_start() prototype
 *
 *  Forgets everything and starts asking again, as for a new address: the
 *  types, then a few that r2 and Linux machines serve whether or not they
 *  answer for _services, each at once, after 1 s, after 2 s and so on,
 *  doubling up to an hour (RFC 6762 §5.2).
 */
void browse_start(Browse_T *b, uint64_t now);

/*
 *  uint32_t browse_query() prototype
 *
 *  The questions due by <now> in one query frame into <out>: the types, the
 *  instances of each type, the SRV of an instance that came without one, the
 *  address of a target that came without one.  Returns its length, 0 when
 *  nothing is due or there is no address to send from.
 */
uint32_t browse_query(Browse_T *b, Mdns_T *m, uint64_t now, uint8_t *out);

/* Takes in the records of the response in <frame>, if it is one. */
void browse_input(Browse_T *b, const Mdns_T *m, const uint8_t *frame, uint32_t len, uint64_t now);

/* Drops what has expired by <now>. */
void browse_expire(Browse_T *b, uint64_t now);

/*
 *  uint32_t browse_list() prototype
 *
 *  The list as above, numbered <seq>, into <out> (BROWSE_LIST_SIZE bytes,
 *  zero-padded): <m>'s own services, then those found that have an SRV.
 *  Lines that do not fit are left out.  Returns the length of the text.
 */
uint32_t browse_list(const Browse_T *b, const Mdns_T *m, uint32_t seq, uint8_t *out);

#endif
