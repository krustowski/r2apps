#ifndef _R2_BYTES_INCLUDED_
#define _R2_BYTES_INCLUDED_

/*
 *  bytes.h
 *
 *  Byte processing-related definitions, declarations and constants for the r2 kernel project.
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

static inline uint16_t swap16(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t swap32(uint32_t x) { return __builtin_bswap32(x); }
static inline uint64_t swap64(uint64_t x) { return __builtin_bswap64(x); }

static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint32_t htonl(uint32_t x) { return ((uint32_t)htons(x & 0xFFFF) << 16) | (uint32_t)htons(x >> 16); }

static inline uint16_t inet_cksum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;

    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)(((uint32_t)data[i] << 8) | data[i + 1]);

    if (len & 1)
        sum += (uint32_t)((uint32_t)data[len - 1] << 8);

    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);

    return (uint16_t)~sum;
}

#ifdef __cplusplus
}
#endif

#endif
