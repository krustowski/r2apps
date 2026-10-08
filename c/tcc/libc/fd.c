/*
 *  File descriptors over r2's named-file syscalls.
 *
 *  The kernel keeps no open files: every read and write names the file and
 *  says where in it to start (syscalls 0x39 and 0x3a).  A descriptor is that
 *  name and an offset.  Names are resolved by the kernel at each call, against
 *  the working directory at the time.
 *
 *  Writing works on FAT only (the floppy, the RAM disk at /mnt/tmp); the ISO
 *  and the boot archive are read-only.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "r2sys.h"

#define FD_MAX 16
/* The kernel reads at most 64 bytes of a name, terminator included. */
#define NAME_LEN 64

static struct fd {
    int used;
    int flags;
    unsigned long off;
    long size; /* -1 until something needs it */
    char name[NAME_LEN];
} fds[FD_MAX] = {
    {1, O_RDONLY, 0, -1, ""},
    {1, O_WRONLY, 0, -1, ""},
    {1, O_WRONLY, 0, -1, ""},
};

static struct fd *lookup(int fd)
{
    if (fd < 0 || fd >= FD_MAX || !fds[fd].used) {
        errno = EBADF;
        return 0;
    }
    return &fds[fd];
}

static int exists_at(const char *name, unsigned long off)
{
    uint8_t c;
    return read_file_at((const uint8_t *)name, &c, off, 1) == 1;
}

/*
 *  The kernel has no stat: a file's size is found by probing for its last
 *  byte, doubling and then halving --- about 2 log2(size) one-byte reads.
 */
static long file_size(struct fd *f)
{
    unsigned long lo, hi;

    if (f->size >= 0)
        return f->size;
    if (!exists_at(f->name, 0))
        return f->size = 0;
    for (lo = 0, hi = 1; exists_at(f->name, hi); hi *= 2)
        lo = hi;
    /* lo holds a byte, hi does not */
    while (hi - lo > 1) {
        unsigned long mid = lo + (hi - lo) / 2;
        if (exists_at(f->name, mid))
            lo = mid;
        else
            hi = mid;
    }
    return f->size = (long)hi;
}

int open(const char *path, int flags, ...)
{
    struct fd *f;
    uint8_t c;
    int fd, acc = flags & O_ACCMODE;
    long r;

    if (strlen(path) >= NAME_LEN) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (fd = 3; fd < FD_MAX && fds[fd].used; fd++)
        ;
    if (fd == FD_MAX) {
        errno = EMFILE;
        return -1;
    }
    f = &fds[fd];
    f->off = 0;
    f->size = -1;
    strcpy(f->name, path);

    /* -1 for a missing file, 0 for an empty one */
    r = read_file_at((const uint8_t *)path, &c, 0, 1);
    if (r < 0) {
        if (!(flags & O_CREAT)) {
            errno = ENOENT;
            return -1;
        }
        f->size = 0;
    } else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        errno = EEXIST;
        return -1;
    } else if ((flags & O_TRUNC) && acc != O_RDONLY) {
        /* write_file_at only ever grows a file */
        if (!delete_file((const uint8_t *)path)) {
            errno = EACCES;
            return -1;
        }
        f->size = 0;
    }
    /* A new file appears on the first write: there is no call to create one. */
    f->flags = flags;
    f->used = 1;
    return fd;
}

int close(int fd)
{
    struct fd *f = lookup(fd);

    if (!f)
        return -1;
    if (fd > 2)
        f->used = 0;
    return 0;
}

/* The console takes NUL-terminated text, so it gets the bytes a piece at a time. */
static ssize_t console_write(const char *buf, size_t n)
{
    char piece[257];
    size_t done = 0;

    while (done < n) {
        size_t k = 0;
        while (k < sizeof(piece) - 1 && done < n) {
            if (buf[done])
                piece[k++] = buf[done];
            done++;
        }
        piece[k] = 0;
        if (k)
            print((const uint8_t *)piece);
    }
    return (ssize_t)n;
}

ssize_t read(int fd, void *buf, size_t n)
{
    struct fd *f = lookup(fd);
    long r;

    if (!f)
        return -1;
    if (fd <= 2)
        return 0; /* no keyboard stream yet: stdin is empty */
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        errno = EBADF;
        return -1;
    }
    if (n == 0)
        return 0;
    r = read_file_at((const uint8_t *)f->name, buf, f->off, n);
    if (r < 0) {
        errno = EIO;
        return -1;
    }
    f->off += (unsigned long)r;
    return r;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    struct fd *f = lookup(fd);
    long r;

    if (!f)
        return -1;
    if (fd == 1 || fd == 2)
        return console_write(buf, n);
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        errno = EBADF;
        return -1;
    }
    if (n == 0)
        return 0;
    if (f->flags & O_APPEND)
        f->off = (unsigned long)file_size(f);
    r = write_file_at((const uint8_t *)f->name, buf, f->off, n);
    if (r < 0) {
        errno = EIO;
        return -1;
    }
    f->off += (unsigned long)r;
    if (f->size >= 0 && (long)f->off > f->size)
        f->size = (long)f->off;
    if ((size_t)r < n)
        errno = ENOSPC;
    return r;
}

off_t lseek(int fd, off_t off, int whence)
{
    struct fd *f = lookup(fd);
    long base;

    if (!f)
        return -1;
    if (fd <= 2) {
        errno = ESPIPE;
        return -1;
    }
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (long)f->off;
        break;
    case SEEK_END:
        base = file_size(f);
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (base + off < 0) {
        errno = EINVAL;
        return -1;
    }
    f->off = (unsigned long)(base + off);
    return (off_t)f->off;
}

int isatty(int fd)
{
    return fd >= 0 && fd <= 2;
}

int unlink(const char *path)
{
    if (!delete_file((const uint8_t *)path)) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int remove(const char *path)
{
    return unlink(path);
}

int rename(const char *oldpath, const char *newpath)
{
    if (!rename_file((const uint8_t *)oldpath, (const uint8_t *)newpath)) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}
