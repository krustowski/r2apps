#ifndef _MDNSD_MDNS_INCLUDED_
#define _MDNSD_MDNS_INCLUDED_

/*
 *  mdns.h
 *
 *  The responder half of Multicast DNS (RFC 6762) and DNS-SD (RFC 6763) for
 *  one IPv4 host: the records it owns, the questions it answers, and the
 *  Ethernet frames that carry the answers.  No syscalls here, so the host
 *  tests run it as it is.
 */

#include "types.h"

#define MDNS_PORT 5353
#define MDNS_LABEL_MAX 63
#define MDNS_NAME_MAX 256 /* a name in wire format: length-prefixed labels and a zero */
#define MDNS_HOST_MAX 32
#define MDNS_MAX_SERVICES 16
#define MDNS_FRAME_MAX 1514

#define MDNS_T_A 1
#define MDNS_T_PTR 12
#define MDNS_T_TXT 16
#define MDNS_T_SRV 33

/* Records about this host (A, SRV) live 120 s in caches, the rest 75 min (RFC 6762 §10). */
#define MDNS_TTL_HOST 120
#define MDNS_TTL_OTHER 4500

/*
 *  type MdnsService_T structure
 *
 *  One advertised server: <instance>.<type>._tcp.local, at <port> on this host.
 *  type is the service without "._tcp", e.g. "_http".
 */
typedef struct {
    uint16_t port;
    const char *type;
    uint8_t instance[MDNS_LABEL_MAX];
    uint8_t instance_len;
} MdnsService_T;

/*
 *  type Mdns_T structure
 *
 *  Everything the responder answers for.  host is a single label: the A
 *  record is <host>.local.  An ip of zero means no address yet, and then
 *  nothing is answered at all.
 */
typedef struct {
    uint8_t host[MDNS_HOST_MAX];
    uint8_t host_len;
    uint8_t ip[4];
    uint8_t mac[6];
    MdnsService_T services[MDNS_MAX_SERVICES];
    uint8_t n_services;
    uint16_t ip_id; /* IPv4 identification, one per frame sent */
} Mdns_T;

/*
 *  uint32_t mdns_handle() prototype
 *
 *  Answers the query in <frame>, a whole Ethernet frame as syscall 0x35 hands
 *  it over, by writing the reply frame into <out> (MDNS_FRAME_MAX bytes).
 *  Returns the reply's length, or 0 when there is nothing to say: not a query,
 *  or no question about a record this host owns.
 *
 *  A query from port 5353 to the group is answered to the group.  One sent to
 *  this host's own address, or from another port (a "legacy" resolver such as
 *  dig, RFC 6762 §6.7), is answered to its sender alone.
 */
uint32_t mdns_handle(Mdns_T *m, const uint8_t *frame, uint32_t len, uint8_t *out);

/*
 *  uint32_t mdns_announce() prototype
 *
 *  An unsolicited response to the group (RFC 6762 §8.3): the host's address
 *  when <svc> is null, or else that service's PTR, SRV and TXT records, the
 *  _services._dns-sd._udp PTR to its type, and the address.  With <goodbye>
 *  the service's records go out with TTL 0 instead, which takes them out of
 *  every cache on the network (§10.1).  Returns the frame's length in <out>,
 *  0 without an address.
 */
uint32_t mdns_announce(Mdns_T *m, const MdnsService_T *svc, uint8_t goodbye, uint8_t *out);

/*
 *  The querier's side: questions out, the records of answers in.
 */

/* A question: a name in wire format and the record type asked for. */
typedef struct {
    const uint8_t *name;
    uint16_t type;
} MdnsQuestion_T;

/*
 *  uint32_t mdns_query() prototype
 *
 *  A query from port 5353 to the group (RFC 6762 §5.2) asking the first
 *  *<n> of <q>, or as many of them as fit in a frame: *<n> is set to that
 *  number.  Returns the frame's length in <out>, 0 without an address.
 */
uint32_t mdns_query(Mdns_T *m, const MdnsQuestion_T *q, uint32_t *n, uint8_t *out);

/*
 *  One record of a response.  The data is read for the types a browser
 *  uses: target is a PTR's name or an SRV's target, in wire format; port is
 *  an SRV's, ip an A record's.  ttl is in seconds, 0 for a goodbye.
 */
typedef struct {
    uint8_t name[MDNS_NAME_MAX];
    uint16_t type;
    uint32_t ttl;
    uint8_t target[MDNS_NAME_MAX];
    uint16_t port;
    uint8_t ip[4];
} MdnsRecord_T;

/* Where mdns_next_record() has got to in a response. */
typedef struct {
    const uint8_t *msg;
    uint32_t len, off, left;
} MdnsRecords_T;

/*
 *  uint8_t mdns_response() prototype
 *
 *  Starts reading the response in <frame>: 1 and <it> set to its first
 *  record, or 0 when it is no response for us.  Only one from port 5353 counts
 *  (RFC 6762 §6), sent to the group or to this host, and not by this host.
 */
uint8_t mdns_response(const Mdns_T *m, const uint8_t *frame, uint32_t len, MdnsRecords_T *it);

/*
 *  uint8_t mdns_next_record() prototype
 *
 *  The next record of the answer, authority and additional sections, in
 *  that order, into <r>.  0 after the last one, or at one that is malformed:
 *  the rest of the message is not read then.  Records of a class other than
 *  IN are passed over.
 */
uint8_t mdns_next_record(MdnsRecords_T *it, MdnsRecord_T *r);

#endif
