#ifndef RNTP_LINUX_CLIENT_H
#define RNTP_LINUX_CLIENT_H
#include "../protocol.h"
#include <pthread.h>
#include <stdatomic.h>

typedef struct { uint64_t lba, stamp; uint8_t data[512]; } RntpCache;
typedef struct {
    int fd, locked, started, initialized;
    uint64_t sectors, generation, revision, id, clock;
    uint32_t metadata_end, count;
    uint8_t boot[512], frame[RNTP_FRAME], payload[RNTP_BATCH_BYTES];
    struct { uint64_t lba; uint8_t data[512]; } writes[RNTP_MAX_WRITES];
    RntpCache cache[224]; /* 192 metadata sectors, 32 data sectors */
    pthread_mutex_t mutex;
    pthread_t heartbeat;
    atomic_bool stop;
} RntpClient;
int rntp_client_connect(RntpClient *c, const char *host, const char *port);
int rntp_client_start(RntpClient *c);
void rntp_client_close(RntpClient *c);
/* begin owns the local mutex until finish; negative errno on failure. */
int rntp_client_begin(RntpClient *c);
int rntp_client_finish(RntpClient *c, int commit);
int rntp_client_read(void *c, uint64_t lba, uint8_t data[512]);
int rntp_client_write(void *c, uint64_t lba, const uint8_t data[512]);
int rntp_client_free(RntpClient *c, uint64_t *bytes);
#endif
