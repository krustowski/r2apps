#ifndef R2_RNTP_READ_AHEAD_H
#define R2_RNTP_READ_AHEAD_H
#include "protocol.h"

/* Private to one client connection and filesystem operation. Metadata remains
 * in the kernel's separate cache; this buffer never evicts its FAT sectors. */
typedef struct {
    uint64_t lba;
    uint32_t count;
    uint8_t data[RNTP_MAX_WRITES * RNTP_SECTOR];
} RntpReadAhead;
void rntp_read_ahead_clear(RntpReadAhead *cache);
uint32_t rntp_read_ahead_count(uint64_t lba, uint64_t sectors, uint64_t data_start);
int rntp_read_ahead_get(const RntpReadAhead *cache, uint64_t lba, uint8_t out[512]);
int rntp_read_ahead_store(RntpReadAhead *cache, uint64_t lba, uint64_t sectors,
                         uint32_t count, const uint8_t *data, uint32_t length);
#endif
