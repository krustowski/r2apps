#include "linux/fat.h"
#include "fixture.h"
#include <errno.h>
#include <stdio.h>
static uint8_t *image;
static int read_sector(void *ctx, uint64_t lba, uint8_t out[512]) { (void)ctx; memcpy(out, image + lba * 512, 512); return 0; }
static int write_sector(void *ctx, uint64_t lba, const uint8_t in[512]) { (void)ctx; memcpy(image + lba * 512, in, 512); return 0; }
struct Listing { unsigned count; off_t offset; };
static int list(void *ctx, const char *name, const struct stat *st, off_t offset) {
    (void)name; (void)st; struct Listing *l = ctx;
    assert(offset > l->offset); l->offset = offset; ++l->count; return l->count == 3;
}
static void test(unsigned mib) {
    image = fixture(mib);
    RntpFat f = { .read = read_sector, .write = write_sector };
    assert(!fat_init(&f, image, mib * 2048));
    struct statvfs before, after; assert(!fat_statfs(&f, &before));
    assert(!fat_create(&f, "/DATA", 1));
    assert(!fat_create(&f, "/DATA/FILE.BIN", 0));
    assert(fat_create(&f, "/data/file.bin", 0) == -EEXIST);
    assert(fat_create(&f, "/NAME-TOO-LONG.BIN", 0) < 0);
    uint8_t in[181001], out[181001];
    for (unsigned i = 0; i < sizeof(in); ++i) in[i] = i * 37 + 19;
    assert(fat_write(&f, "/data/file.bin", in, sizeof(in), 0) == sizeof(in));
    assert(fat_read(&f, "/DATA/FILE.BIN", out, sizeof(out), 0) == sizeof(out)); assert(!memcmp(in, out, sizeof(in)));
    assert(fat_read(&f, "/DATA/FILE.BIN", out, 1507, 509) == 1507); assert(!memcmp(in + 509, out, 1507));
    assert(!memcmp(image + f.fat * 512, image + (f.fat + f.fat_sectors) * 512, f.fat_sectors * 512));
    assert(!fat_truncate(&f, "/DATA/FILE.BIN", 509));
    assert(!fat_truncate(&f, "/DATA/FILE.BIN", 2053));
    memset(out, 123, 2053); assert(fat_read(&f, "/DATA/FILE.BIN", out, 2053, 0) == 2053);
    assert(!memcmp(in, out, 509)); for (unsigned i = 509; i < 2053; ++i) assert(!out[i]);
    assert(fat_write(&f, "/DATA/FILE.BIN", in, 300, 8000) == 300);
    assert(fat_read(&f, "/DATA/FILE.BIN", out, 8300, 0) == 8300);
    for (unsigned i = 2053; i < 8000; ++i) assert(!out[i]);
    assert(!memcmp(out + 8000, in, 300));
    assert(!fat_create(&f, "/OTHER", 1));
    assert(fat_rename(&f, "/DATA", "/DATA/SUB", 0) == -EINVAL);
    assert(!fat_rename(&f, "/DATA/FILE.BIN", "/OTHER/NEW.BIN", 0));
    struct stat st; assert(fat_getattr(&f, "/DATA/FILE.BIN", &st) == -ENOENT);
    assert(!fat_getattr(&f, "/OTHER/NEW.BIN", &st) && st.st_size == 8300);
    assert(!fat_create(&f, "/OTHER/DEST.BIN", 0));
    assert(fat_rename(&f, "/OTHER/NEW.BIN", "/OTHER/DEST.BIN", 1) == -EEXIST);
    assert(!fat_rename(&f, "/OTHER/NEW.BIN", "/OTHER/DEST.BIN", 0));
    assert(fat_remove(&f, "/OTHER", 1) == -ENOTEMPTY);
    assert(fat_remove(&f, "/OTHER/DEST.BIN", 1) == -ENOTDIR);
    struct Listing l = {0}; assert(!fat_readdir(&f, "/OTHER", &l, list, 0) && l.count == 3);
    assert(!fat_remove(&f, "/OTHER/DEST.BIN", 0));
    assert(!fat_remove(&f, "/DATA", 1)); assert(!fat_remove(&f, "/OTHER", 1));
    assert(!fat_statfs(&f, &after) && before.f_bfree == after.f_bfree);
    assert(!fat_create(&f, "/EXPAND", 1));
    for (unsigned i = 0; i < 70; ++i) { char name[32]; snprintf(name, sizeof(name), "/EXPAND/F%u", i); assert(!fat_create(&f, name, 0)); }
    l = (struct Listing){0}; unsigned total = 0;
    do { l.count = 0; assert(!fat_readdir(&f, "/EXPAND", &l, list, l.offset)); total += l.count; } while (l.count);
    assert(total == 72);
    assert(!fat_create(&f, "/MOVE", 1)); assert(!fat_rename(&f, "/EXPAND", "/MOVE/CHILD", 0));
    assert(fat_rename(&f, "/MOVE", "/MOVE/CHILD/CYCLE", 0) == -EINVAL);
    free(image);
}
int main(void) { test(2); test(16); test(64); puts("Linux FAT12/16 operations: PASS"); return 0; }
