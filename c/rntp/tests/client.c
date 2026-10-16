#include "linux/client.h"
#include "linux/fat.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct { RntpClient c; RntpFat f; unsigned index; } Client;
static void setup(Client *c, const char *host, const char *port, unsigned index) {
    assert(!rntp_client_connect(&c->c, host, port));
    c->f = (RntpFat){ .ctx = &c->c, .read = rntp_client_read, .write = rntp_client_write };
    assert(!fat_init(&c->f, c->c.boot, c->c.sectors)); c->index = index;
    assert(!rntp_client_start(&c->c));
}
static void start(Client *c) { assert(!rntp_client_begin(&c->c)); }
static int end(Client *c, int result) { assert(!rntp_client_finish(&c->c, result >= 0)); return result; }
static void *worker(void *ptr) {
    Client *c = ptr; char dir[20], path[40]; snprintf(dir, sizeof(dir), "/USER%u", c->index);
    start(c); assert(!end(c, fat_create(&c->f, dir, 1)));
    uint8_t data[8192]; memset(data, 40 + c->index, sizeof(data));
    for (unsigned i = 0; i < 24; ++i) {
        snprintf(path, sizeof(path), "%s/F%u.BIN", dir, i);
        start(c); assert(!end(c, fat_create(&c->f, path, 0)));
        start(c); assert(end(c, fat_write(&c->f, path, data, sizeof(data), 0)) == sizeof(data));
        start(c); uint8_t out[8192]; assert(end(c, fat_read(&c->f, path, out, sizeof(out), 0)) == sizeof(out)); assert(!memcmp(out, data, sizeof(out)));
        start(c); assert(end(c, fat_write(&c->f, "/SHARED.BIN", data, 512, c->index * 512)) == 512);
    }
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 3); Client *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b)); assert(a && b);
    setup(a, argv[1], argv[2], 0); setup(b, argv[1], argv[2], 1);
    start(a); assert(!end(a, fat_create(&a->f, "/SHARED.BIN", 0)));
    pthread_t t1, t2; assert(!pthread_create(&t1, NULL, worker, a)); assert(!pthread_create(&t2, NULL, worker, b));
    pthread_join(t1, NULL); pthread_join(t2, NULL);
    uint8_t out[2048]; start(a); assert(end(a, fat_read(&a->f, "/SHARED.BIN", out, sizeof(out), 0)) == 1024);
    for (unsigned i = 0; i < 1024; ++i) assert(out[i] == (i < 512 ? 40 : 41));
    start(b); assert(!end(b, fat_rename(&b->f, "/SHARED.BIN", "/RENAMED.BIN", 0)));
    start(a); struct stat st; assert(end(a, fat_getattr(&a->f, "/SHARED.BIN", &st)) == -ENOENT);
    start(a); assert(!end(a, fat_getattr(&a->f, "/RENAMED.BIN", &st)) && st.st_size == 1024);
    start(a); struct statvfs before; assert(!end(a, fat_statfs(&a->f, &before)));
    uint8_t *too_large = malloc(a->c.sectors * 512ULL); assert(too_large); memset(too_large, 77, a->c.sectors * 512ULL);
    start(b); int error = fat_write(&b->f, "/RENAMED.BIN", too_large, a->c.sectors * 512ULL, 0);
    assert(error == -ENOSPC); assert(!rntp_client_finish(&b->c, 0)); free(too_large);
    start(a); struct statvfs after; assert(!end(a, fat_statfs(&a->f, &after))); assert(before.f_bfree == after.f_bfree);
    start(a); assert(end(a, fat_read(&a->f, "/RENAMED.BIN", out, sizeof(out), 0)) == 1024);
    for (unsigned i = 0; i < 1024; ++i) assert(out[i] == (i < 512 ? 40 : 41));
    /* Auto-flush more than 32 writes, then drop TCP without committing. */
    start(b); uint8_t damage[512]; memset(damage, 99, sizeof(damage));
    for (unsigned i = 0; i < 40; ++i) assert(!rntp_client_write(&b->c, b->f.data + i, damage));
    shutdown(b->c.fd, SHUT_RDWR); assert(rntp_client_finish(&b->c, 0) < 0);
    start(a); assert(end(a, fat_read(&a->f, "/RENAMED.BIN", out, sizeof(out), 0)) == 1024);
    for (unsigned i = 0; i < 1024; ++i) assert(out[i] == (i < 512 ? 40 : 41));
    rntp_client_close(&a->c); rntp_client_close(&b->c); free(a); free(b);
    puts("two TCP clients: concurrent writes, cache coherence, ENOSPC rollback and disconnect rollback PASS"); return 0;
}
