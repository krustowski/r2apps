#define _GNU_SOURCE
#include "fat.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define EOC 0xffffU
#define DIR 0x10
#define READONLY 1
#define TRY(expr) do { int error_ = (expr); if (error_ < 0) return error_; } while (0)
typedef struct { uint64_t sector; unsigned offset; uint8_t raw[32]; int root; } Entry;
static uint32_t le(const uint8_t *p, unsigned n) {
    uint32_t v = 0; while (n) v = (v << 8) | p[--n]; return v;
}
static void put(uint8_t *p, uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) { p[i] = v; v >>= 8; }
}
static uint32_t first(const Entry *e) { return le(e->raw + 26, 2); }
static uint32_t length(const Entry *e) { return le(e->raw + 28, 4); }
static uint32_t cluster_bytes(const RntpFat *f) { return f->spc * 512U; }
static int valid(const RntpFat *f, uint32_t c) {
    return c >= 2 && c < f->clusters + 2 && c < (f->bits == 12 ? 0xff0U : 0xfff0U);
}
static uint64_t sector_of(const RntpFat *f, uint32_t c) { return f->data + (uint64_t)(c - 2) * f->spc; }
static int rd(RntpFat *f, uint64_t sector, uint8_t out[512]) {
    return sector < f->sectors ? f->read(f->ctx, sector, out) : -EIO;
}
static int wr(RntpFat *f, uint64_t sector, const uint8_t in[512]) {
    return sector && sector < f->sectors ? f->write(f->ctx, sector, in) : -EIO;
}
int fat_init(RntpFat *f, const uint8_t b[512], uint64_t capacity) {
    uint32_t total = le(b + 19, 2); if (!total) total = le(b + 32, 4);
    f->spc = b[13]; f->fat = le(b + 14, 2); f->fats = b[16];
    f->fat_sectors = le(b + 22, 2); f->root_entries = le(b + 17, 2);
    if (b[510] != 0x55 || b[511] != 0xaa || le(b + 11, 2) != 512
        || !f->spc || (f->spc & (f->spc - 1)) || f->spc > 64 || !f->fat
        || !f->fats || f->fats > 2 || !f->fat_sectors || !f->root_entries
        || !total || total > capacity || capacity > (64U << 20) / 512) return -EINVAL;
    f->sectors = total; f->root = f->fat + f->fats * f->fat_sectors;
    f->data = f->root + (f->root_entries * 32 + 511) / 512;
    if (f->data >= total) return -EINVAL;
    f->clusters = (total - f->data) / f->spc;
    if (!f->clusters || f->clusters >= 65525) return -EINVAL;
    f->bits = f->clusters < 4085 ? 12 : 16;
    if ((uint64_t)(f->clusters + 2) * f->bits > (uint64_t)f->fat_sectors * 4096) return -EINVAL;
    return 0;
}
static int fat_get(RntpFat *f, uint32_t c, uint32_t *v) {
    if (!valid(f, c)) return -EIO;
    uint32_t at = f->bits == 12 ? c + c / 2 : c * 2;
    uint8_t a[512], b[512]; TRY(rd(f, f->fat + at / 512, a));
    uint32_t value = a[at % 512];
    if (at % 512 == 511) { TRY(rd(f, f->fat + at / 512 + 1, b)); value |= b[0] << 8; }
    else value |= a[at % 512 + 1] << 8;
    if (f->bits == 12) value = c & 1 ? value >> 4 : value & 0xfff;
    *v = value >= (f->bits == 12 ? 0xff8U : 0xfff8U) ? EOC : value;
    return 0;
}
static int fat_set(RntpFat *f, uint32_t c, uint32_t v) {
    if (!valid(f, c)) return -EIO;
    uint32_t at = f->bits == 12 ? c + c / 2 : c * 2, off = at % 512;
    uint8_t a[512], b[512];
    TRY(rd(f, f->fat + at / 512, a));
    if (off == 511) TRY(rd(f, f->fat + at / 512 + 1, b));
    uint32_t old = a[off] | ((off == 511 ? b[0] : a[off + 1]) << 8);
    if (f->bits == 12) v = c & 1 ? (old & 15) | ((v & 0xfff) << 4) : (old & 0xf000) | (v & 0xfff);
    a[off] = v; if (off == 511) b[0] = v >> 8; else a[off + 1] = v >> 8;
    for (unsigned i = 0; i < f->fats; ++i) {
        TRY(wr(f, f->fat + i * f->fat_sectors + at / 512, a));
        if (off == 511) TRY(wr(f, f->fat + i * f->fat_sectors + at / 512 + 1, b));
    }
    return 0;
}
static int allocate(RntpFat *f, uint32_t hint, uint32_t *out) {
    if (!valid(f, hint)) hint = 2;
    for (uint32_t i = 0; i < f->clusters; ++i) {
        uint32_t c = 2 + (hint - 2 + i) % f->clusters, v;
        if (!valid(f, c)) continue;
        TRY(fat_get(f, c, &v));
        if (v) continue;
        TRY(fat_set(f, c, EOC));
        uint8_t zero[512] = {0};
        for (unsigned j = 0; j < f->spc; ++j) TRY(wr(f, sector_of(f, c) + j, zero));
        *out = c; return 0;
    }
    return -ENOSPC;
}
static int free_chain(RntpFat *f, uint32_t c) {
    if (!c) return 0;
    for (uint32_t i = 0; i < f->clusters; ++i) {
        uint32_t next; TRY(fat_get(f, c, &next)); TRY(fat_set(f, c, 0));
        if (next == EOC) return 0;
        if (!valid(f, next)) return -EIO;
        c = next;
    }
    return -EIO;
}
static void timestamp(uint8_t *e, time_t value) {
    struct tm tm; gmtime_r(&value, &tm);
    if (tm.tm_year < 80) { tm.tm_year = 80; tm.tm_mon = 0; tm.tm_mday = 1; }
    if (tm.tm_year > 207) tm.tm_year = 207;
    put(e + 22, (tm.tm_hour << 11) | (tm.tm_min << 5) | tm.tm_sec / 2, 2);
    put(e + 24, ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday, 2);
}
static time_t entry_time(const uint8_t *e) {
    uint32_t d = le(e + 24, 2), t = le(e + 22, 2);
    if (!d) return 0;
    struct tm tm = { .tm_year = 80 + (int)(d >> 9), .tm_mon = (int)((d >> 5) & 15) - 1,
        .tm_mday = d & 31, .tm_hour = t >> 11, .tm_min = (t >> 5) & 63, .tm_sec = (t & 31) * 2 };
    return timegm(&tm);
}
static void stat_entry(RntpFat *f, const Entry *e, struct stat *st) {
    memset(st, 0, sizeof(*st));
    int dir = e->root || (e->raw[11] & DIR);
    st->st_mode = (dir ? S_IFDIR | 0755 : S_IFREG | 0644);
    if (e->raw[11] & READONLY) st->st_mode &= ~0222;
    st->st_nlink = dir ? 2 : 1; st->st_uid = f->uid; st->st_gid = f->gid;
    st->st_size = dir ? cluster_bytes(f) : length(e);
    st->st_blksize = cluster_bytes(f); st->st_blocks = (st->st_size + 511) / 512;
    st->st_mtime = st->st_ctime = st->st_atime = entry_time(e->raw);
}
static int name83(const char *s, size_t n, uint8_t out[11]) {
    if (!n || n > 12 || s[0] == '.' || s[n - 1] == '.') return -EINVAL;
    memset(out, ' ', 11); unsigned pos = 0, ext = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = s[i];
        if (c == '.') { if (ext || !pos) return -EINVAL; ext = 1; pos = 8; continue; }
        if (c <= 32 || c >= 127 || strchr("\"*+,/:;<=>?[\\]|", c)) return -EINVAL;
        if ((!ext && pos >= 8) || (ext && pos >= 11)) return -ENAMETOOLONG;
        out[pos++] = (uint8_t)toupper(c);
    }
    return 0;
}
static void display_name(const uint8_t raw[32], char out[13]) {
    unsigned pos = 0, n = 8;
    while (n && raw[n - 1] == ' ') --n;
    for (unsigned i = 0; i < n; ++i) out[pos++] = raw[i];
    n = 3; while (n && raw[8 + n - 1] == ' ') --n;
    if (n) { out[pos++] = '.'; for (unsigned i = 0; i < n; ++i) out[pos++] = raw[8 + i]; }
    out[pos] = 0;
}
/* Directory sectors are addressed by ordinal, bounded by the volume. */
static int directory_sector(RntpFat *f, uint32_t dir, uint32_t ordinal, uint64_t *lba) {
    if (!dir) {
        if (ordinal >= (f->root_entries * 32 + 511) / 512) return -ENOENT;
        *lba = f->root + ordinal; return 0;
    }
    if (ordinal / f->spc >= f->clusters) return -EIO;
    for (uint32_t i = 0; i < ordinal / f->spc; ++i) {
        uint32_t next; TRY(fat_get(f, dir, &next));
        if (next == EOC) return -ENOENT;
        if (!valid(f, next)) return -EIO;
        dir = next;
    }
    if (!valid(f, dir)) return -EIO;
    *lba = sector_of(f, dir) + ordinal % f->spc; return 0;
}
static int find(RntpFat *f, uint32_t dir, const uint8_t name[11], Entry *entry) {
    for (uint32_t i = 0; i < f->clusters * f->spc; ++i) {
        uint64_t lba; int result = directory_sector(f, dir, i, &lba);
        if (result < 0) return result;
        uint8_t data[512]; TRY(rd(f, lba, data));
        for (unsigned j = 0; j < 16; ++j) {
            if (!dir && i * 16 + j >= f->root_entries) return -ENOENT;
            uint8_t *e = data + j * 32;
            if (!e[0]) return -ENOENT;
            if (e[0] == 0xe5 || (e[11] & 0x0f) == 0x0f || (e[11] & 8)) continue;
            if (!memcmp(e, name, 11)) {
                entry->sector = lba; entry->offset = j * 32; entry->root = 0;
                memcpy(entry->raw, e, 32); return 0;
            }
        }
    }
    return -EIO;
}
static int lookup(RntpFat *f, const char *path, Entry *out) {
    if (!path || path[0] != '/') return -EINVAL;
    memset(out, 0, sizeof(*out)); out->root = 1; out->raw[11] = DIR;
    const char *p = path; uint32_t dir = 0;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        const char *start = p; while (*p && *p != '/') ++p;
        uint8_t name[11]; TRY(name83(start, p - start, name));
        TRY(find(f, dir, name, out));
        const char *next = p; while (*next == '/') ++next;
        if (*next) {
            if (!(out->raw[11] & DIR)) return -ENOTDIR;
            dir = first(out); if (!valid(f, dir)) return -EIO;
        }
    }
    return 0;
}
static int parent(RntpFat *f, const char *path, uint32_t *dir, uint8_t name[11]) {
    size_t n = strlen(path); if (!n || path[0] != '/' || n > 4095) return -EINVAL;
    while (n > 1 && path[n - 1] == '/') --n;
    size_t start = n; while (start && path[start - 1] != '/') --start;
    TRY(name83(path + start, n - start, name));
    char prefix[4096]; memcpy(prefix, path, start); prefix[start] = 0;
    Entry e; TRY(lookup(f, prefix, &e));
    if (!(e.raw[11] & DIR)) return -ENOTDIR;
    *dir = e.root ? 0 : first(&e); return 0;
}
static int save(RntpFat *f, const Entry *e) {
    uint8_t data[512]; TRY(rd(f, e->sector, data)); memcpy(data + e->offset, e->raw, 32);
    return wr(f, e->sector, data);
}
static int empty_slot(RntpFat *f, uint32_t dir, Entry *entry) {
    uint32_t i;
    for (i = 0; i < f->clusters * f->spc; ++i) {
        uint64_t lba; int result = directory_sector(f, dir, i, &lba);
        if (result == -ENOENT) break;
        if (result < 0) return result;
        uint8_t data[512]; TRY(rd(f, lba, data));
        for (unsigned j = 0; j < 16; ++j) {
            if (!dir && i * 16 + j >= f->root_entries) return -ENOSPC;
            if (!data[j * 32] || data[j * 32] == 0xe5) {
                memset(entry, 0, sizeof(*entry)); entry->sector = lba; entry->offset = j * 32; return 0;
            }
        }
    }
    if (!dir) return -ENOSPC;
    if (i >= f->clusters * f->spc) return -EIO;
    uint32_t c = dir;
    for (uint32_t k = 0; k < f->clusters; ++k) {
        uint32_t next; TRY(fat_get(f, c, &next));
        if (next == EOC) {
            TRY(allocate(f, c + 1, &next)); TRY(fat_set(f, c, next));
            memset(entry, 0, sizeof(*entry)); entry->sector = sector_of(f, next); return 0;
        }
        if (!valid(f, next)) return -EIO;
        c = next;
    }
    return -EIO;
}
int fat_getattr(RntpFat *f, const char *path, struct stat *st) {
    Entry e; TRY(lookup(f, path, &e)); stat_entry(f, &e, st); return 0;
}
int fat_readdir(RntpFat *f, const char *path, void *ctx, FatFill fill, off_t offset) {
    if (offset < 0) return -EINVAL;
    Entry e; TRY(lookup(f, path, &e)); if (!(e.raw[11] & DIR)) return -ENOTDIR;
    struct stat st; stat_entry(f, &e, &st);
    if (offset < 1 && fill(ctx, ".", &st, 1)) return 0;
    if (offset < 2 && fill(ctx, "..", NULL, 2)) return 0;
    uint32_t dir = e.root ? 0 : first(&e);
    for (uint32_t i = 0; i < f->clusters * f->spc; ++i) {
        uint64_t lba; int result = directory_sector(f, dir, i, &lba);
        if (result == -ENOENT) return 0;
        if (result < 0) return result;
        uint8_t data[512]; TRY(rd(f, lba, data));
        for (unsigned j = 0; j < 16; ++j) {
            if (!dir && i * 16 + j >= f->root_entries) return 0;
            const uint8_t *raw = data + j * 32;
            if (!raw[0]) return 0;
            if (raw[0] == 0xe5 || raw[0] == '.' || (raw[11] & 8)) continue;
            off_t next = (off_t)i * 16 + j + 3;
            if (next <= offset) continue;
            memcpy(e.raw, raw, 32); e.root = 0; stat_entry(f, &e, &st);
            char name[13]; display_name(raw, name);
            if (fill(ctx, name, &st, next)) return 0;
        }
    }
    return -EIO;
}
int fat_create(RntpFat *f, const char *path, int directory) {
    uint32_t dir; uint8_t name[11]; TRY(parent(f, path, &dir, name));
    Entry e; int result = find(f, dir, name, &e);
    if (!result) return -EEXIST;
    if (result != -ENOENT) return result;
    TRY(empty_slot(f, dir, &e)); memcpy(e.raw, name, 11); e.raw[11] = directory ? DIR : 0x20;
    timestamp(e.raw, time(NULL));
    if (directory) {
        uint32_t cluster; TRY(allocate(f, 2, &cluster)); put(e.raw + 26, cluster, 2);
        uint8_t data[512] = {0};
        memset(data, ' ', 11); data[0] = '.'; data[11] = DIR; put(data + 26, cluster, 2);
        memset(data + 32, ' ', 11); data[32] = data[33] = '.'; data[43] = DIR; put(data + 58, dir, 2);
        TRY(wr(f, sector_of(f, cluster), data));
    }
    return save(f, &e);
}
static int is_empty(RntpFat *f, uint32_t dir) {
    for (uint32_t i = 0; i < f->clusters * f->spc; ++i) {
        uint64_t lba; int r = directory_sector(f, dir, i, &lba);
        if (r == -ENOENT) return 0;
        if (r < 0) return r;
        uint8_t data[512]; TRY(rd(f, lba, data));
        for (unsigned j = 0; j < 16; ++j) {
            uint8_t *e = data + j * 32; if (!e[0]) return 0;
            if (e[0] != 0xe5 && e[0] != '.' && !(e[11] & 8)) return -ENOTEMPTY;
        }
    }
    return -EIO;
}
int fat_remove(RntpFat *f, const char *path, int directory) {
    Entry e; TRY(lookup(f, path, &e)); if (e.root) return -EBUSY;
    if (directory != !!(e.raw[11] & DIR)) return directory ? -ENOTDIR : -EISDIR;
    if (e.raw[11] & READONLY) return -EACCES;
    if (directory) TRY(is_empty(f, first(&e)));
    TRY(free_chain(f, first(&e))); e.raw[0] = 0xe5; return save(f, &e);
}
static int chain_at(RntpFat *f, Entry *e, uint32_t index, int grow, uint32_t *out) {
    if (index >= f->clusters) return grow ? -ENOSPC : -EIO;
    uint32_t c = first(e);
    if (!c) {
        if (!grow) return -EIO;
        TRY(allocate(f, 2, &c)); put(e->raw + 26, c, 2);
    }
    for (uint32_t i = 0; i < index; ++i) {
        uint32_t next; TRY(fat_get(f, c, &next));
        if (next == EOC && grow) { TRY(allocate(f, c + 1, &next)); TRY(fat_set(f, c, next)); }
        if (!valid(f, next)) return -EIO;
        c = next;
    }
    if (!valid(f, c)) return -EIO;
    *out = c; return 0;
}
static int file_entry(RntpFat *f, const char *path, Entry *e, int write) {
    TRY(lookup(f, path, e)); if (e->raw[11] & DIR) return -EISDIR;
    if (write && (e->raw[11] & READONLY)) return -EACCES;
    return 0;
}
int fat_read(RntpFat *f, const char *path, void *out, size_t size, off_t offset) {
    if (offset < 0 || size > INT_MAX) return -EINVAL;
    Entry e; TRY(file_entry(f, path, &e, 0));
    if ((uint64_t)offset >= length(&e)) return 0;
    if (size > length(&e) - (uint64_t)offset) size = length(&e) - (uint64_t)offset;
    if (!size) return 0;
    uint32_t c; TRY(chain_at(f, &e, offset / cluster_bytes(f), 0, &c));
    size_t done = 0; uint32_t at = offset % cluster_bytes(f);
    while (done < size) {
        uint8_t data[512]; TRY(rd(f, sector_of(f, c) + at / 512, data));
        size_t n = 512 - at % 512; if (n > size - done) n = size - done;
        memcpy((uint8_t *)out + done, data + at % 512, n); done += n; at += n;
        if (at == cluster_bytes(f) && done < size) {
            uint32_t next; TRY(fat_get(f, c, &next)); if (!valid(f, next)) return -EIO;
            c = next; at = 0;
        }
    }
    return (int)done;
}
/* Fill a range in one chain walk, including bytes between EOF and a write. */
static int write_range(RntpFat *f, Entry *e, uint64_t offset, const uint8_t *in, uint64_t size) {
    if (!size) return 0;
    uint32_t c; TRY(chain_at(f, e, offset / cluster_bytes(f), 1, &c));
    uint32_t at = offset % cluster_bytes(f); uint64_t done = 0;
    while (done < size) {
        uint8_t data[512]; size_t n = 512 - at % 512; if (n > size - done) n = size - done;
        uint64_t lba = sector_of(f, c) + at / 512;
        if (n != 512) TRY(rd(f, lba, data));
        if (in) memcpy(data + at % 512, in + done, n); else memset(data + at % 512, 0, n);
        TRY(wr(f, lba, data)); done += n; at += n;
        if (at == cluster_bytes(f) && done < size) {
            uint32_t next; TRY(fat_get(f, c, &next));
            if (next == EOC) { TRY(allocate(f, c + 1, &next)); TRY(fat_set(f, c, next)); }
            if (!valid(f, next)) return -EIO;
            c = next; at = 0;
        }
    }
    return 0;
}
int fat_write(RntpFat *f, const char *path, const void *in, size_t size, off_t offset) {
    if (offset < 0 || size > INT_MAX || (uint64_t)offset + size > UINT32_MAX) return -EFBIG;
    Entry e; TRY(file_entry(f, path, &e, 1)); if (!size) return 0;
    uint32_t old = length(&e);
    if ((uint64_t)offset > old) TRY(write_range(f, &e, old, NULL, (uint64_t)offset - old));
    TRY(write_range(f, &e, offset, in, size));
    if ((uint64_t)offset + size > old) put(e.raw + 28, (uint64_t)offset + size, 4);
    timestamp(e.raw, time(NULL)); TRY(save(f, &e)); return (int)size;
}
int fat_truncate(RntpFat *f, const char *path, off_t size) {
    if (size < 0 || (uint64_t)size > UINT32_MAX) return -EFBIG;
    Entry e; TRY(file_entry(f, path, &e, 1)); uint32_t old = length(&e);
    if ((uint64_t)size > old) TRY(write_range(f, &e, old, NULL, (uint64_t)size - old));
    else if ((uint64_t)size < old) {
        if (!size) { TRY(free_chain(f, first(&e))); put(e.raw + 26, 0, 2); }
        else {
            uint32_t c, next; TRY(chain_at(f, &e, (size - 1) / cluster_bytes(f), 0, &c));
            TRY(fat_get(f, c, &next)); TRY(fat_set(f, c, EOC));
            if (next != EOC) TRY(free_chain(f, next));
        }
    }
    put(e.raw + 28, size, 4); timestamp(e.raw, time(NULL)); return save(f, &e);
}
int fat_rename(RntpFat *f, const char *from, const char *to, unsigned flags) {
    if (flags & ~1U) return -EINVAL; /* RENAME_NOREPLACE only. */
    Entry src, dst; TRY(lookup(f, from, &src)); if (src.root) return -EBUSY;
    if (src.raw[11] & READONLY) return -EACCES;
    uint32_t dir; uint8_t name[11]; TRY(parent(f, to, &dir, name));
    if (src.raw[11] & DIR) {
        uint32_t c = dir;
        for (uint32_t i = 0; c; ++i) {
            if (c == first(&src)) return -EINVAL;
            if (i >= f->clusters || !valid(f, c)) return -EIO;
            uint8_t dotdot[11]; memset(dotdot, ' ', 11); dotdot[0] = dotdot[1] = '.';
            Entry parent_entry; TRY(find(f, c, dotdot, &parent_entry)); c = first(&parent_entry);
        }
    }
    int found = find(f, dir, name, &dst);
    if (!found) {
        if (dst.sector == src.sector && dst.offset == src.offset) return 0;
        if (flags & 1U) return -EEXIST;
        if (!!(src.raw[11] & DIR) != !!(dst.raw[11] & DIR)) return src.raw[11] & DIR ? -ENOTDIR : -EISDIR;
        if (dst.raw[11] & READONLY) return -EACCES;
        if (dst.raw[11] & DIR) TRY(is_empty(f, first(&dst)));
        TRY(free_chain(f, first(&dst)));
    } else {
        if (found != -ENOENT) return found;
        TRY(empty_slot(f, dir, &dst));
    }
    memcpy(dst.raw, src.raw, 32); memcpy(dst.raw, name, 11); TRY(save(f, &dst));
    if (src.raw[11] & DIR) {
        uint8_t data[512]; TRY(rd(f, sector_of(f, first(&src)), data)); put(data + 58, dir, 2);
        TRY(wr(f, sector_of(f, first(&src)), data));
    }
    src.raw[0] = 0xe5; return save(f, &src);
}
int fat_statfs(RntpFat *f, struct statvfs *st) {
    memset(st, 0, sizeof(*st)); st->f_bsize = st->f_frsize = cluster_bytes(f);
    st->f_blocks = f->clusters; st->f_namemax = 12;
    for (uint32_t c = 2; c < f->clusters + 2; ++c) {
        if (!valid(f, c)) continue;
        uint32_t v; TRY(fat_get(f, c, &v)); if (!v) ++st->f_bfree;
    }
    st->f_bavail = st->f_bfree; return 0;
}
int fat_utimens(RntpFat *f, const char *path, const struct timespec times[2]) {
    Entry e; TRY(lookup(f, path, &e)); if (e.root) return 0;
    if (e.raw[11] & READONLY) return -EACCES;
    if (times && times[1].tv_nsec == UTIME_OMIT) return 0;
    timestamp(e.raw, !times || times[1].tv_nsec == UTIME_NOW ? time(NULL) : times[1].tv_sec);
    return save(f, &e);
}
