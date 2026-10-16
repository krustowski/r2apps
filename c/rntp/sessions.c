#include "sessions.h"

static void copy(uint8_t *to, const uint8_t *from, uint64_t n) {
    for (uint64_t i = 0; i < n; ++i) to[i] = from[i];
}
static void finish(RntpSessions *s, int commit) {
    if (s->modified) {
        if (!commit) {
            for (uint64_t lba = 0; lba < s->sectors; ++lba)
                if (s->dirty[lba / 8] & (1U << (lba % 8)))
                    copy(s->image + lba * RNTP_SECTOR, s->undo + lba * RNTP_SECTOR, RNTP_SECTOR);
        } else {
            if (!++s->revision) s->revision = 1;
            rntp_free_space(s->image, s->sectors, &s->committed_free);
        }
    }
    for (uint64_t i = 0; i < (s->sectors + 7) / 8; ++i) s->dirty[i] = 0;
    s->modified = 0;
    s->locked = -1;
}
int rntp_sessions_init(RntpSessions *s, uint8_t *image, uint8_t *undo,
                       uint64_t sectors, uint64_t generation) {
    if (!s || !image || !undo || !generation || !sectors || sectors > RNTP_DIRTY_BYTES * 8ULL)
        return 0;
    s->image = image; s->undo = undo; s->sectors = sectors; s->generation = generation;
    s->revision = 1; s->legacy = s->locked = -1; s->modified = 0;
    for (unsigned i = 0; i < RNTP_PEERS; ++i) s->peers[i] = 0;
    for (unsigned i = 0; i < RNTP_DIRTY_BYTES; ++i) s->dirty[i] = 0;
    return rntp_free_space(image, sectors, &s->committed_free);
}
void rntp_sessions_release(RntpSessions *s, unsigned peer) {
    if (peer >= RNTP_PEERS) return;
    if (s->locked == (int)peer) finish(s, 0);
    if (s->legacy == (int)peer) s->legacy = -1;
    s->peers[peer] = 0;
}
static void remember(RntpSessions *s, uint64_t lba) {
    uint8_t bit = (uint8_t)(1U << (lba % 8));
    if (!(s->dirty[lba / 8] & bit)) {
        copy(s->undo + lba * RNTP_SECTOR, s->image + lba * RNTP_SECTOR, RNTP_SECTOR);
        s->dirty[lba / 8] |= bit;
    }
    s->modified = 1;
}
uint32_t rntp_sessions_handle(RntpSessions *s, unsigned peer,
                              const uint8_t *request, uint8_t *reply) {
    RntpHeader h;
    if (peer >= RNTP_PEERS || !rntp_decode(request, &h)) return 0;
    RntpHeader out = { (uint8_t)(h.op | RNTP_RESPONSE), RNTP_OK, 0,
                       h.id, h.lba, s->generation };
    if (!h.id || h.status || (h.op & RNTP_RESPONSE)) out.status = RNTP_INVALID;
    else if (h.op == RNTP_HELLO || h.op == RNTP_HELLO_SHARED) {
        if (h.length || h.lba || h.generation || s->peers[peer]) out.status = RNTP_INVALID;
        else {
            int shared = h.op == RNTP_HELLO_SHARED;
            int busy = s->legacy >= 0;
            if (!shared)
                for (unsigned i = 0; i < RNTP_PEERS; ++i) busy |= s->peers[i] != 0;
            if (busy) out.status = RNTP_BUSY;
            else {
                s->peers[peer] = shared ? 2 : 1;
                if (!shared) s->legacy = (int)peer;
                out.lba = s->sectors; out.length = RNTP_SECTOR;
                copy(reply + RNTP_HEADER, s->image, RNTP_SECTOR);
            }
        }
    } else if (!s->peers[peer]) out.status = RNTP_BUSY;
    else if (h.generation != s->generation) out.status = RNTP_STALE;
    else if (h.op == RNTP_CAPS && !h.length && !h.lba) {
        out.length = 8;
        rntp_put_number(reply + RNTP_HEADER, RNTP_CAP_BATCH | RNTP_CAP_SHARED | RNTP_CAP_READ_BATCH, 8);
    } else if (h.op == RNTP_LOCK || h.op == RNTP_UNLOCK || h.op == RNTP_ABORT) {
        if (s->peers[peer] != 2 || h.length || h.lba) out.status = RNTP_INVALID;
        else if (h.op == RNTP_LOCK) {
            if (s->locked >= 0) out.status = RNTP_BUSY;
            else { s->locked = (int)peer; out.length = 8; }
        } else if (s->locked != (int)peer) out.status = RNTP_INVALID;
        else { finish(s, h.op == RNTP_UNLOCK); out.length = 8; }
        if (out.length) rntp_put_number(reply + RNTP_HEADER, s->revision, 8);
    } else if (h.op == RNTP_STAT && !h.length && !h.lba) {
        /* Publish only committed FAT edits, including the owner's heartbeat. */
        out.length = RNTP_STAT_BYTES;
        rntp_put_number(reply + RNTP_HEADER, s->committed_free, RNTP_STAT_BYTES);
    } else {
        int write = h.op == RNTP_WRITE || h.op == RNTP_WRITE_BATCH;
        int block = write || h.op == RNTP_READ || h.op == RNTP_READ_BATCH;
        if (block && s->peers[peer] == 2 && s->locked != (int)peer) out.status = RNTP_BUSY;
        else if (write && s->peers[peer] == 2) {
            /* Check the entire frame before adding any undo records. Sector 0
             * is immutable in shared mode: all mounts rely on its geometry. */
            uint32_t count = h.op == RNTP_WRITE ? 1 :
                (h.length >= 4 ? (uint32_t)rntp_get_number(request + RNTP_HEADER, 4) : 0);
            if ((h.op == RNTP_WRITE && h.length != RNTP_SECTOR)
                || (h.op == RNTP_WRITE_BATCH && (h.lba || !count || count > RNTP_MAX_WRITES
                    || h.length != 4 + count * RNTP_WRITE_RECORD))) out.status = RNTP_INVALID;
            for (uint32_t i = 0; out.status == RNTP_OK && i < count; ++i) {
                uint64_t lba = h.op == RNTP_WRITE ? h.lba :
                    rntp_get_number(request + RNTP_HEADER + 4 + i * RNTP_WRITE_RECORD, 8);
                if (!lba || lba >= s->sectors) out.status = RNTP_INVALID;
            }
            if (out.status == RNTP_OK) {
                for (uint32_t i = 0; i < count; ++i)
                    remember(s, h.op == RNTP_WRITE ? h.lba :
                        rntp_get_number(request + RNTP_HEADER + 4 + i * RNTP_WRITE_RECORD, 8));
                return rntp_handle(s->image, s->sectors, s->generation, 1, request, reply);
            }
        } else {
            uint32_t n = rntp_handle(s->image, s->sectors, s->generation, 1, request, reply);
            RntpHeader result;
            if (n && write && rntp_decode(reply, &result) && !result.status) {
                ++s->revision;
                rntp_free_space(s->image, s->sectors, &s->committed_free);
            }
            return n;
        }
    }
    rntp_encode(reply, &out);
    return RNTP_HEADER + out.length;
}
