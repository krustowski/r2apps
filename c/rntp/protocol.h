#ifndef R2_RNTP_PROTOCOL_H
#define R2_RNTP_PROTOCOL_H
#ifdef RNTP_HOST
#include <stdint.h>
#else
#include "types.h"
#endif

#define RNTP_PORT 7780
#define RNTP_HEADER 40
#define RNTP_SECTOR 512
#define RNTP_MAX_WRITES 32
#define RNTP_WRITE_RECORD (8 + RNTP_SECTOR)
#define RNTP_BATCH_BYTES (4 + RNTP_MAX_WRITES * RNTP_WRITE_RECORD)
#define RNTP_FRAME (RNTP_HEADER + RNTP_BATCH_BYTES)
#define RNTP_HELLO 1
#define RNTP_READ 2
#define RNTP_WRITE 3
#define RNTP_PING 4
#define RNTP_STAT 5
#define RNTP_CAPS 6
#define RNTP_WRITE_BATCH 7
#define RNTP_HELLO_SHARED 8
#define RNTP_LOCK 9
#define RNTP_UNLOCK 10
#define RNTP_READ_BATCH 11
#define RNTP_ABORT 12
#define RNTP_CAP_BATCH 1ULL
#define RNTP_CAP_SHARED 2ULL
#define RNTP_CAP_READ_BATCH 4ULL
#define RNTP_STAT_BYTES 8
#define RNTP_RESPONSE 128
#define RNTP_OK 0
#define RNTP_BUSY 1
#define RNTP_INVALID 2
#define RNTP_STALE 3

typedef struct {
    uint8_t op;
    uint16_t status;
    uint32_t length;
    uint64_t id, lba, generation;
} RntpHeader;

void rntp_encode(uint8_t out[RNTP_HEADER], const RntpHeader *header);
int rntp_decode(const uint8_t in[RNTP_HEADER], RntpHeader *header);
void rntp_put_number(uint8_t *out, uint64_t value, uint32_t bytes);
uint64_t rntp_get_number(const uint8_t *in, uint32_t bytes);
uint64_t rntp_decode_free_space(const uint8_t payload[RNTP_STAT_BYTES]);
int rntp_free_space(const uint8_t *image, uint64_t capacity, uint64_t *free_bytes);
/* A complete request in, a complete reply out. No transport or kernel calls. */
uint32_t rntp_handle(uint8_t *image, uint64_t sectors, uint64_t generation,
                     int permitted, const uint8_t *request, uint8_t *reply);
#endif
