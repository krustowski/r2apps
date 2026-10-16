#include "read_ahead.h"
static void copy(uint8_t *out, const uint8_t *in, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) out[i] = in[i];
}
void rntp_read_ahead_clear(RntpReadAhead *cache) { cache->count = 0; }
uint32_t rntp_read_ahead_count(uint64_t lba, uint64_t sectors, uint64_t data_start) {
    if (lba >= sectors) return 0;
    uint64_t count = sectors - lba;
    if (count > RNTP_MAX_WRITES) count = RNTP_MAX_WRITES;
    // Metadata-only queries should not fetch unrelated file data.
    if (lba < data_start && count > data_start - lba) count = data_start - lba;
    return (uint32_t)count;
}
int rntp_read_ahead_get(const RntpReadAhead *cache, uint64_t lba, uint8_t out[512]) {
    if (lba < cache->lba || lba - cache->lba >= cache->count) return 0;
    copy(out, cache->data + (lba - cache->lba) * RNTP_SECTOR, RNTP_SECTOR);
    return 1;
}
int rntp_read_ahead_store(RntpReadAhead *cache, uint64_t lba, uint64_t sectors,
                         uint32_t count, const uint8_t *data, uint32_t length) {
    rntp_read_ahead_clear(cache);
    if (!count || count > RNTP_MAX_WRITES || lba >= sectors || count > sectors - lba
        || length != count * RNTP_SECTOR) return 0;
    copy(cache->data, data, length);
    cache->lba = lba; cache->count = count;
    return 1;
}
