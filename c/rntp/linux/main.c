#define FUSE_USE_VERSION 31
#define _GNU_SOURCE
#include <fuse.h>
#include "client.h"
#include "fat.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { RntpClient client; RntpFat fat; } Mount;
static Mount *mount_state(void) { return fuse_get_context()->private_data; }
static int begin(Mount **m) { *m = mount_state(); return rntp_client_begin(&(*m)->client); }
static int end(Mount *m, int result) {
    int e = rntp_client_finish(&m->client, result >= 0);
    return e < 0 ? e : result;
}
static int getattr(const char *p, struct stat *st, struct fuse_file_info *fi) {
    (void)fi; Mount *m; int e = begin(&m); if (e < 0) return e;
    return end(m, fat_getattr(&m->fat, p, st));
}
struct Fill { void *out; fuse_fill_dir_t fill; };
static int fill(void *ptr, const char *name, const struct stat *st, off_t offset) {
    struct Fill *f = ptr; return f->fill(f->out, name, st, offset, 0);
}
static int readdir(const char *p, void *out, fuse_fill_dir_t callback, off_t offset,
                   struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void)fi; (void)flags; Mount *m; int e = begin(&m); if (e < 0) return e;
    struct Fill f = { out, callback }; return end(m, fat_readdir(&m->fat, p, &f, fill, offset));
}
static int create(const char *p, mode_t mode, struct fuse_file_info *fi) {
    (void)mode; Mount *m; int e = begin(&m); if (e < 0) return e;
    fi->direct_io = 1; return end(m, fat_create(&m->fat, p, 0));
}
static int mkdir_cb(const char *p, mode_t mode) {
    (void)mode; Mount *m; int e = begin(&m); if (e < 0) return e;
    return end(m, fat_create(&m->fat, p, 1));
}
static int unlink_cb(const char *p) {
    Mount *m; int e = begin(&m); if (e < 0) return e;
    return end(m, fat_remove(&m->fat, p, 0));
}
static int rmdir_cb(const char *p) {
    Mount *m; int e = begin(&m); if (e < 0) return e;
    return end(m, fat_remove(&m->fat, p, 1));
}
static int rename_cb(const char *from, const char *to, unsigned flags) {
    Mount *m; int e = begin(&m); if (e < 0) return e;
    return end(m, fat_rename(&m->fat, from, to, flags));
}
static int open_file(const char *p, struct fuse_file_info *fi) {
    Mount *m; int e = begin(&m); if (e < 0) return e;
    struct stat st; e = fat_getattr(&m->fat, p, &st);
    if (!e && S_ISDIR(st.st_mode)) e = -EISDIR;
    if (!e && (fi->flags & O_ACCMODE) != O_RDONLY && !(st.st_mode & 0222)) e = -EACCES;
    fi->direct_io = 1; fi->keep_cache = 0; return end(m, e);
}
static int read_file(const char *p, char *out, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void)fi; Mount *m; int e = begin(&m); if (e < 0) return e;
    return end(m, fat_read(&m->fat, p, out, size, offset));
}
static int write_file(const char *p, const char *in, size_t size, off_t offset, struct fuse_file_info *fi) {
    Mount *m; int e = begin(&m); if (e < 0) return e;
    if (fi && (fi->flags & O_APPEND)) {
        struct stat st; e = fat_getattr(&m->fat, p, &st);
        if (e < 0) return end(m, e);
        offset = st.st_size;
    }
    return end(m, fat_write(&m->fat, p, in, size, offset));
}
static int truncate_file(const char *p, off_t size, struct fuse_file_info *fi) {
    (void)fi; Mount *m; int e = begin(&m); if (e < 0) return e;
    return end(m, fat_truncate(&m->fat, p, size));
}
static int statfs(const char *p, struct statvfs *st) {
    (void)p; Mount *m; int e = begin(&m); if (e < 0) return e;
    uint64_t free_bytes; e = rntp_client_free(&m->client, &free_bytes);
    if (e >= 0) {
        memset(st, 0, sizeof(*st)); st->f_bsize = st->f_frsize = m->fat.spc * 512;
        st->f_blocks = m->fat.clusters; st->f_bfree = st->f_bavail = free_bytes / st->f_frsize;
        st->f_namemax = 12;
    }
    return end(m, e);
}
static int utimens(const char *p, const struct timespec times[2], struct fuse_file_info *fi) {
    (void)fi; Mount *m; int e = begin(&m); if (e < 0) return e;
    return end(m, fat_utimens(&m->fat, p, times));
}
static int fsync_file(const char *p, int data_only, struct fuse_file_info *fi) {
    (void)p; (void)data_only; (void)fi;
    Mount *m; int e = begin(&m); return e < 0 ? e : end(m, 0);
}
static void *init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    Mount *m = mount_state();
    cfg->entry_timeout = cfg->negative_timeout = cfg->attr_timeout = 0;
    cfg->direct_io = 1; cfg->kernel_cache = cfg->auto_cache = 0;
    cfg->hard_remove = 1; /* FAT's 8.3 names cannot hold libfuse's hidden names. */
    conn->want &= ~FUSE_CAP_WRITEBACK_CACHE;
    conn->max_write = 16384;
    if (rntp_client_start(&m->client) < 0) {
        fprintf(stderr, "rntp-fuse: cannot start heartbeat\n");
        fuse_exit(fuse_get_context()->fuse);
    }
    return m;
}
static void destroy(void *ptr) { Mount *m = ptr; rntp_client_close(&m->client); }
static const struct fuse_operations operations = {
    .getattr = getattr, .readdir = readdir, .create = create, .mkdir = mkdir_cb,
    .unlink = unlink_cb, .rmdir = rmdir_cb, .rename = rename_cb, .open = open_file,
    .read = read_file, .write = write_file, .truncate = truncate_file,
    .statfs = statfs, .utimens = utimens, .fsync = fsync_file,
    .init = init, .destroy = destroy,
};
int main(int argc, char **argv) {
    if (argc < 3 || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "Usage: rntp-fuse HOST MOUNTPOINT [-p PORT] [-f] [FUSE options]\n");
        return argc >= 2 && !strcmp(argv[1], "--help") ? 0 : 2;
    }
    const char *port = "7780";
    char **args = calloc(argc + 4, sizeof(*args));
    Mount *m = calloc(1, sizeof(*m));
    if (!args || !m) { free(args); free(m); return 1; }
    int count = 0; args[count++] = argv[0]; args[count++] = argv[2];
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--port")) {
            if (++i >= argc) { fprintf(stderr, "rntp-fuse: missing port\n"); free(args); free(m); return 2; }
            port = argv[i];
        } else args[count++] = argv[i];
    }
    char *tail; unsigned long n = strtoul(port, &tail, 10);
    if (!*port || *tail || !n || n > 65535) { fprintf(stderr, "rntp-fuse: invalid port\n"); free(args); free(m); return 2; }
    args[count++] = "-s"; args[count++] = "-o"; args[count++] = "default_permissions,fsname=rntp,subtype=rntp";
    int e = rntp_client_connect(&m->client, argv[1], port);
    if (e < 0) {
        fprintf(stderr, "rntp-fuse: connection failed: %s (shared RNTP server required)\n", strerror(-e));
        /* connect initialized the mutex even when the handshake failed. */
        rntp_client_close(&m->client); free(args); free(m); return 1;
    }
    m->fat.ctx = &m->client; m->fat.read = rntp_client_read; m->fat.write = rntp_client_write;
    m->fat.uid = getuid(); m->fat.gid = getgid();
    e = fat_init(&m->fat, m->client.boot, m->client.sectors);
    if (e < 0) {
        fprintf(stderr, "rntp-fuse: invalid FAT12/16 volume\n");
        rntp_client_close(&m->client); free(args); free(m); return 1;
    }
    e = fuse_main(count, args, &operations, m);
    /* libfuse calls destroy after a session, but early mount failure skips it. */
    rntp_client_close(&m->client);
    free(args); free(m); return e;
}
