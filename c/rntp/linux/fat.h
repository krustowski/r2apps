#ifndef RNTP_LINUX_FAT_H
#define RNTP_LINUX_FAT_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

typedef struct {
    void *ctx;
    int (*read)(void *, uint64_t, uint8_t[512]);
    int (*write)(void *, uint64_t, const uint8_t[512]);
    uint32_t sectors, fat, root, data, clusters, fat_sectors, root_entries;
    uint8_t spc, fats, bits;
    uid_t uid;
    gid_t gid;
} RntpFat;
/* Every operation must run inside the transport's LOCK/UNLOCK transaction.
 * No directory entry or allocation decision survives between operations. */
int fat_init(RntpFat *f, const uint8_t boot[512], uint64_t capacity);
int fat_getattr(RntpFat *f, const char *path, struct stat *st);
typedef int (*FatFill)(void *, const char *, const struct stat *, off_t);
int fat_readdir(RntpFat *f, const char *path, void *ctx, FatFill fill, off_t offset);
int fat_create(RntpFat *f, const char *path, int directory);
int fat_remove(RntpFat *f, const char *path, int directory);
int fat_rename(RntpFat *f, const char *from, const char *to, unsigned flags);
int fat_read(RntpFat *f, const char *path, void *out, size_t size, off_t offset);
int fat_write(RntpFat *f, const char *path, const void *in, size_t size, off_t offset);
int fat_truncate(RntpFat *f, const char *path, off_t size);
int fat_statfs(RntpFat *f, struct statvfs *st);
int fat_utimens(RntpFat *f, const char *path, const struct timespec times[2]);
#endif
