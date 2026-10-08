/*
 *  Buffered streams over fd.c.
 *
 *  A stream holds one buffer, used for reading or for writing at a time.
 *  stdout is line-buffered and stderr unbuffered, as usual; files are fully
 *  buffered, which matters here: each write is a syscall that walks the FAT
 *  chain from the start of the file.  exit() flushes every stream.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "r2sys.h"

#define F_EOF 1
#define F_ERR 2
#define F_READING 4 /* buf holds bytes read ahead of the fd's offset */
#define F_WRITING 8 /* buf holds bytes not written yet */
#define F_STATIC 16 /* stdin, stdout, stderr: never freed */

struct r2_file {
    int fd;
    int flags;
    int bufmode;
    int ungot; /* -1, or the byte ungetc() pushed back */
    unsigned char *buf;
    size_t pos; /* reading: next byte in buf */
    size_t len; /* reading: bytes in buf; writing: bytes pending */
    struct r2_file *next;
};

static FILE std_in = {0, F_STATIC, _IOFBF, -1, 0, 0, 0, 0};
static FILE std_out = {1, F_STATIC, _IOLBF, -1, 0, 0, 0, &std_in};
static FILE std_err = {2, F_STATIC, _IONBF, -1, 0, 0, 0, &std_out};

FILE *stdin = &std_in;
FILE *stdout = &std_out;
FILE *stderr = &std_err;

/* Every open stream, newest first. */
static FILE *streams = &std_err;

static int get_buf(FILE *f)
{
    if (!f->buf && !(f->buf = malloc(BUFSIZ))) {
        f->flags |= F_ERR;
        return -1;
    }
    return 0;
}

static int write_all(FILE *f, const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t r = write(f->fd, p, n);
        if (r <= 0) {
            f->flags |= F_ERR;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Settle the buffer with the fd: write what is pending, or give back read-ahead. */
static int settle(FILE *f)
{
    int r = 0;

    if (f->flags & F_WRITING) {
        r = write_all(f, f->buf, f->len);
        f->flags &= ~F_WRITING;
    } else if (f->flags & F_READING) {
        size_t ahead = f->len - f->pos + (f->ungot >= 0);
        if (ahead)
            lseek(f->fd, -(off_t)ahead, SEEK_CUR);
        f->flags &= ~F_READING;
    }
    f->pos = f->len = 0;
    f->ungot = -1;
    return r;
}

int fflush(FILE *f)
{
    int r = 0;

    if (!f) {
        for (f = streams; f; f = f->next)
            if (f->flags & F_WRITING)
                r |= settle(f);
        return r ? EOF : 0;
    }
    return settle(f) ? EOF : 0;
}

void __stdio_flush_all(void)
{
    fflush(0);
}

static int parse_mode(const char *mode)
{
    int flags;

    switch (*mode) {
    case 'r':
        flags = O_RDONLY;
        break;
    case 'w':
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    case 'a':
        flags = O_WRONLY | O_CREAT | O_APPEND;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (strchr(mode, '+'))
        flags = (flags & ~O_ACCMODE) | O_RDWR;
    return flags;
}

FILE *fdopen(int fd, const char *mode)
{
    FILE *f;

    if (parse_mode(mode) < 0)
        return 0;
    if (!(f = calloc(1, sizeof(*f))))
        return 0;
    f->fd = fd;
    f->bufmode = isatty(fd) ? _IOLBF : _IOFBF;
    f->ungot = -1;
    f->next = streams;
    streams = f;
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    FILE *f;
    int flags = parse_mode(mode), fd;

    if (flags < 0 || (fd = open(path, flags, 0666)) < 0)
        return 0;
    if (!(f = fdopen(fd, mode)))
        close(fd);
    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    int flags = parse_mode(mode), fd;

    settle(f);
    if (f->fd > 2)
        close(f->fd);
    if (flags < 0 || (fd = open(path, flags, 0666)) < 0) {
        f->flags |= F_ERR;
        return 0;
    }
    f->fd = fd;
    f->flags &= F_STATIC;
    return f;
}

int fclose(FILE *f)
{
    FILE **pp;
    int r = settle(f);

    if (f->fd > 2 && close(f->fd) < 0)
        r = -1;
    if (f->flags & F_STATIC)
        return r ? EOF : 0;
    for (pp = &streams; *pp; pp = &(*pp)->next) {
        if (*pp == f) {
            *pp = f->next;
            break;
        }
    }
    free(f->buf);
    free(f);
    return r ? EOF : 0;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    (void)buf;
    (void)size;
    f->bufmode = mode;
    return 0;
}

void setbuf(FILE *f, char *buf)
{
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f)
{
    const unsigned char *p = ptr;
    size_t total = size * n, room;

    if (!total)
        return 0;
    if (f->flags & F_READING)
        settle(f);
    if (f->bufmode == _IONBF || total >= BUFSIZ) {
        if ((f->flags & F_WRITING) && settle(f))
            return 0;
        return write_all(f, p, total) ? 0 : n;
    }
    if (get_buf(f))
        return 0;
    room = BUFSIZ - f->len;
    if (total > room) {
        memcpy(f->buf + f->len, p, room);
        f->len = BUFSIZ;
        f->flags |= F_WRITING;
        if (settle(f))
            return 0;
        p += room;
        total -= room;
    }
    memcpy(f->buf + f->len, p, total);
    f->len += total;
    f->flags |= F_WRITING;
    if (f->bufmode == _IOLBF && memchr(p, '\n', total) && settle(f))
        return 0;
    return n;
}

static int refill(FILE *f)
{
    ssize_t r;

    if (f->flags & F_WRITING)
        settle(f);
    if (f == stdin)
        fflush(stdout);
    if (get_buf(f))
        return -1;
    r = read(f->fd, f->buf, BUFSIZ);
    if (r <= 0) {
        f->flags |= r < 0 ? F_ERR : F_EOF;
        f->flags &= ~F_READING;
        f->pos = f->len = 0;
        return -1;
    }
    f->pos = 0;
    f->len = (size_t)r;
    f->flags |= F_READING;
    return 0;
}

int fgetc(FILE *f)
{
    int c;

    if (f->ungot >= 0) {
        c = f->ungot;
        f->ungot = -1;
        return c;
    }
    if (!(f->flags & F_READING) || f->pos == f->len)
        if (refill(f))
            return EOF;
    return f->buf[f->pos++];
}

size_t fread(void *ptr, size_t size, size_t n, FILE *f)
{
    unsigned char *p = ptr;
    size_t total = size * n, got = 0;

    if (!total)
        return 0;
    if (f->ungot >= 0) {
        p[got++] = (unsigned char)f->ungot;
        f->ungot = -1;
    }
    while (got < total) {
        size_t k;
        if ((f->flags & F_READING) && f->pos < f->len) {
            k = f->len - f->pos;
            if (k > total - got)
                k = total - got;
            memcpy(p + got, f->buf + f->pos, k);
            f->pos += k;
            got += k;
        } else if (total - got >= BUFSIZ) {
            /* big reads go straight to the caller's memory */
            ssize_t r;
            if (f->flags & F_WRITING)
                settle(f);
            f->flags &= ~F_READING;
            r = read(f->fd, p + got, total - got);
            if (r <= 0) {
                f->flags |= r < 0 ? F_ERR : F_EOF;
                break;
            }
            got += (size_t)r;
        } else if (refill(f)) {
            break;
        }
    }
    return got / size;
}

int getc(FILE *f)
{
    return fgetc(f);
}

int getchar(void)
{
    return fgetc(stdin);
}

int ungetc(int c, FILE *f)
{
    if (c == EOF || f->ungot >= 0)
        return EOF;
    f->ungot = (unsigned char)c;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

char *fgets(char *s, int size, FILE *f)
{
    int i = 0, c;

    if (size <= 0)
        return 0;
    while (i < size - 1 && (c = fgetc(f)) != EOF) {
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return 0;
    s[i] = 0;
    return s;
}

int fputc(int c, FILE *f)
{
    unsigned char b = (unsigned char)c;
    return fwrite(&b, 1, 1, f) == 1 ? b : EOF;
}

int putc(int c, FILE *f)
{
    return fputc(c, f);
}

int putchar(int c)
{
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s)
{
    return fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF ? EOF : 0;
}

int fseek(FILE *f, long off, int whence)
{
    if (settle(f) || lseek(f->fd, off, whence) < 0)
        return -1;
    f->flags &= ~F_EOF;
    return 0;
}

long ftell(FILE *f)
{
    long pos = lseek(f->fd, 0, SEEK_CUR);

    if (pos < 0)
        return -1;
    if (f->flags & F_READING)
        pos -= (long)(f->len - f->pos);
    else if (f->flags & F_WRITING)
        pos += (long)f->len;
    if (f->ungot >= 0)
        pos--;
    return pos;
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    f->flags &= ~F_ERR;
}

int fgetpos(FILE *f, fpos_t *pos)
{
    return (*pos = ftell(f)) < 0 ? -1 : 0;
}

int fsetpos(FILE *f, const fpos_t *pos)
{
    return fseek(f, *pos, SEEK_SET);
}

int feof(FILE *f)
{
    return (f->flags & F_EOF) != 0;
}

int ferror(FILE *f)
{
    return (f->flags & F_ERR) != 0;
}

void clearerr(FILE *f)
{
    f->flags &= ~(F_EOF | F_ERR);
}

int fileno(FILE *f)
{
    return f->fd;
}

void perror(const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}
