#ifndef R2_RNTP_SESSIONS_H
#define R2_RNTP_SESSIONS_H
#include "protocol.h"

/* The listener uses one of libcr2's eight sockets, leaving seven clients. */
#define RNTP_PEERS 8
#define RNTP_DIRTY_BYTES ((64U << 20) / RNTP_SECTOR / 8)
typedef struct {
    uint8_t *image, *undo;
    uint64_t sectors, generation, revision, committed_free;
    int legacy, locked;
    uint8_t peers[RNTP_PEERS], dirty[RNTP_DIRTY_BYTES];
    int modified;
} RntpSessions;

int rntp_sessions_init(RntpSessions *s, uint8_t *image, uint8_t *undo,
                       uint64_t sectors, uint64_t generation);
/* Disconnect, RST, timeout and socket reuse all release the session. */
void rntp_sessions_release(RntpSessions *s, unsigned peer);
uint32_t rntp_sessions_handle(RntpSessions *s, unsigned peer,
                              const uint8_t *request, uint8_t *reply);
#endif
