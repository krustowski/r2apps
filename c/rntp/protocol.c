#include "protocol.h"

uint64_t rntp_get_number(const uint8_t *p, uint32_t n) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < n; ++i) value = (value << 8) | p[i];
    return value;
}
void rntp_put_number(uint8_t *p, uint64_t value, uint32_t n) {
    while (n) { p[--n] = (uint8_t)value; value >>= 8; }
}
static void copy(uint8_t *out, const uint8_t *in, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) out[i] = in[i];
}

uint64_t rntp_decode_free_space(const uint8_t payload[RNTP_STAT_BYTES]) {
    return rntp_get_number(payload, RNTP_STAT_BYTES);
}

static uint64_t le(const uint8_t *p, uint32_t n) {
    uint64_t value = 0;
    while (n) value = (value << 8) | p[--n];
    return value;
}

/* The primary FAT is already in server RAM. Count it locally once per STAT,
 * instead of making mount listings fetch every table sector over TCP. */
int rntp_free_space(const uint8_t *image, uint64_t capacity, uint64_t *free_bytes) {
    if (capacity < 1 || image[510] != 0x55 || image[511] != 0xaa || le(image + 11, 2) != 512) return 0;
    uint64_t total = le(image + 19, 2);
    if (!total) total = le(image + 32, 4);
    uint64_t spc = image[13], reserved = le(image + 14, 2);
    uint64_t fats = image[16], root = le(image + 17, 2), fat_sectors = le(image + 22, 2);
    if (!spc || (spc & (spc - 1)) || spc > 64 || !reserved
        || !fats || fats > 2 || !root || !fat_sectors || total > capacity) return 0;
    uint64_t start = reserved + fats * fat_sectors + (root * 32 + 511) / 512;
    if (start >= total) return 0;
    uint64_t clusters = (total - start) / spc;
    if (!clusters || clusters >= 65525) return 0;
    uint64_t bits = clusters < 4085 ? 12 : 16;
    if ((clusters + 2) * bits > fat_sectors * 512 * 8) return 0;
    const uint8_t *fat = image + reserved * 512;
    uint64_t count = 0;
    for (uint64_t cluster = 2; cluster < clusters + 2; ++cluster) {
        uint64_t offset = bits == 12 ? cluster * 3 / 2 : cluster * 2;
        uint64_t value = le(fat + offset, 2);
        if (bits == 12) value = (cluster & 1) ? value >> 4 : value & 0xfff;
        count += value == 0;
    }
    *free_bytes = count * spc * 512;
    return 1;
}

void rntp_encode(uint8_t out[RNTP_HEADER], const RntpHeader *h) {
    out[0] = 'R'; out[1] = 'N'; out[2] = 'T'; out[3] = 'P';
    out[4] = 1; out[5] = h->op;
    rntp_put_number(out + 6, h->status, 2); rntp_put_number(out + 8, h->length, 4);
    rntp_put_number(out + 12, 0, 4); rntp_put_number(out + 16, h->id, 8);
    rntp_put_number(out + 24, h->lba, 8); rntp_put_number(out + 32, h->generation, 8);
}

int rntp_decode(const uint8_t in[RNTP_HEADER], RntpHeader *h) {
    if (in[0] != 'R' || in[1] != 'N' || in[2] != 'T' || in[3] != 'P'
        || in[4] != 1 || rntp_get_number(in + 12, 4) != 0) return 0;
    h->op = in[5]; h->status = (uint16_t)rntp_get_number(in + 6, 2);
    h->length = (uint32_t)rntp_get_number(in + 8, 4);
    h->id = rntp_get_number(in + 16, 8); h->lba = rntp_get_number(in + 24, 8);
    h->generation = rntp_get_number(in + 32, 8);
    return h->length <= RNTP_BATCH_BYTES;
}

uint32_t rntp_handle(uint8_t *image, uint64_t sectors, uint64_t generation,
                     int permitted, const uint8_t *request, uint8_t *reply) {
    RntpHeader h;
    if (!rntp_decode(request, &h)) return 0;
    RntpHeader out = { (uint8_t)(h.op | RNTP_RESPONSE), RNTP_OK, 0,
                       h.id, h.lba, generation };
    if (h.status || (h.op & RNTP_RESPONSE) || !h.id) out.status = RNTP_INVALID;
    else if (h.op == RNTP_HELLO) {
        if (h.length || h.lba || h.generation) out.status = RNTP_INVALID;
        else if (!permitted) out.status = RNTP_BUSY;
        else { out.lba = sectors; out.length = RNTP_SECTOR; copy(reply + RNTP_HEADER, image, RNTP_SECTOR); }
    } else if (!permitted) out.status = RNTP_BUSY;
    else if (h.generation != generation) out.status = RNTP_STALE;
    else if (h.op == RNTP_PING) {
        if (h.length || h.lba) out.status = RNTP_INVALID;
    } else if (h.op == RNTP_STAT) {
        uint64_t free_bytes;
        if (h.length || h.lba || !rntp_free_space(image, sectors, &free_bytes)) out.status = RNTP_INVALID;
        else { out.length = RNTP_STAT_BYTES; rntp_put_number(reply + RNTP_HEADER, free_bytes, RNTP_STAT_BYTES); }
    } else if (h.op == RNTP_CAPS) {
        if (h.length || h.lba) out.status = RNTP_INVALID;
        else { out.length = 8; rntp_put_number(reply + RNTP_HEADER, RNTP_CAP_BATCH, 8); }
    } else if (h.op == RNTP_READ_BATCH) {
        uint64_t count = h.length == 4 ? rntp_get_number(request + RNTP_HEADER, 4) : 0;
        if (!count || count > RNTP_MAX_WRITES || h.lba >= sectors || count > sectors - h.lba)
            out.status = RNTP_INVALID;
        else {
            out.length = (uint32_t)count * RNTP_SECTOR;
            copy(reply + RNTP_HEADER, image + h.lba * RNTP_SECTOR, out.length);
        }
    } else if (h.op == RNTP_WRITE_BATCH) {
        uint32_t count = h.length >= 4 ? (uint32_t)rntp_get_number(request + RNTP_HEADER, 4) : 0;
        if (h.lba || !count || count > RNTP_MAX_WRITES || h.length != 4 + count * RNTP_WRITE_RECORD)
            out.status = RNTP_INVALID;
        // Validate EVERY record before modifying any of the exported image.
        for (uint32_t i = 0; out.status == RNTP_OK && i < count; ++i)
            if (rntp_get_number(request + RNTP_HEADER + 4 + i * RNTP_WRITE_RECORD, 8) >= sectors)
                out.status = RNTP_INVALID;
        if (out.status == RNTP_OK) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t *record = request + RNTP_HEADER + 4 + i * RNTP_WRITE_RECORD;
                copy(image + rntp_get_number(record, 8) * RNTP_SECTOR, record + 8, RNTP_SECTOR);
            }
        }
    } else if (h.lba >= sectors) out.status = RNTP_INVALID;
    else if (h.op == RNTP_READ && h.length == 0) {
        out.length = RNTP_SECTOR;
        copy(reply + RNTP_HEADER, image + h.lba * RNTP_SECTOR, RNTP_SECTOR);
    } else if (h.op == RNTP_WRITE && h.length == RNTP_SECTOR) {
        copy(image + h.lba * RNTP_SECTOR, request + RNTP_HEADER, RNTP_SECTOR);
    } else out.status = RNTP_INVALID;
    rntp_encode(reply, &out);
    return RNTP_HEADER + out.length;
}
