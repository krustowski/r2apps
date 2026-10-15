/*
 *  host.c
 *
 *  Linux harness for theM.
 *
 *  The emulator itself is written against the r2 syscall wrappers.  This
 *  file implements the same handful of calls on top of libc so the very
 *  same sources can be built and driven on a workstation, where a failing
 *  guest can be traced, screenshotted and re-run in a second rather than
 *  through a boot of the operating system.
 *
 *  Not part of the r2 build: the Makefile there compiles them/*.c only.
 *
 *  Driven by the environment:
 *    THEM_RUN_MS   how long to let the guest run  (default 4000)
 *    THEM_SHOTS    comma separated ms marks to capture the screen at
 *    THEM_KEYS     ms:scancode pairs to feed the keyboard
 *    THEM_OUT      output path prefix              (default ./them)
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* Declared by hand: libcr2 defines its own exit() and memcpy(), so pulling
 * in <stdlib.h> or <string.h> here would collide with them. */
char *getenv(const char *name);
unsigned long strtoul(const char *nptr, char **endptr, int base);
unsigned long strlen(const char *s);

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

/* Mirrors of the r2 ABI structures the emulator passes around. */
typedef struct {
    u8 system_name[32];
    u8 system_user[32];
    u8 system_path[32];
    u8 system_version[8];
    u32 system_path_cluster;
    u32 system_uptime;
    u8 ip_addr[4];
} __attribute__((packed)) SysInfo_T;

typedef struct {
    u8 name[32];
    u8 name_len;
    u8 is_dir;
    u32 size;
} __attribute__((packed)) VfsDirEntry_T;

typedef struct {
    u8 seconds;
    u8 minutes;
    u8 hours;
    u8 day;
    u8 month;
    u16 year;
} __attribute__((packed)) RTC_T;

/* --- clock ----------------------------------------------------------- */

static u64 start_ms = 0;

static u64 now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000 + (u64)(ts.tv_nsec / 1000000);
}

u64 get_ticks(void) {
    if (!start_ms)
        start_ms = now_ms();
    return now_ms() - start_ms;
}

void sleep_ms(u64 ms) { usleep((useconds_t)(ms * 1000)); }

/* --- console --------------------------------------------------------- */

/* The emulator's own diagnostics go to the serial line on r2; here that is
 * simply stderr, alongside anything a guest prints. */
long serial_init(void) { return 1; }

long serial_write(u32 value) {
    fputc((int)(value & 0xFF), stderr);
    fflush(stderr);
    return 1;
}

long serial_read(u32 *value) {
    *value = 0;
    return 0;
}

long print(const u8 *s) {
    fputs((const char *)s, stderr);
    fflush(stderr);
    return 1;
}

long clear_screen(void) {
    fputs("\n---- clear ----\n", stderr);
    return 1;
}

/* --- framebuffer ----------------------------------------------------- */

static u32 frame[1024 * 768];
static u32 frame_w = 0, frame_h = 0;
static unsigned long frame_count = 0;

static char out_prefix[256] = "them";
static u64 shot_times[32];
static int shot_count = 0;
static int shot_done = 0;

static void write_ppm(const char *path) {
    FILE *f;

    if (!frame_w || !frame_h)
        return;
    f = fopen(path, "wb");

    if (!f)
        return;

    fprintf(f, "P6\n%u %u\n255\n", frame_w, frame_h);
    for (u32 i = 0; i < frame_w * frame_h; i++) {
        u8 rgb[3] = {(u8)(frame[i] >> 16), (u8)(frame[i] >> 8), (u8)frame[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/*
 *  Presenting a frame costs nothing on a workstation and tens of milliseconds
 *  on r2, where the call writes the whole VESA framebuffer over a bus that is
 *  not the memory bus.  THEM_BLIT_MS buys that cost back, so a display eating
 *  the run -- which looks exactly like a hung emulator -- can be reproduced
 *  and its pacing checked without booting the operating system.
 */
static void blit_delay(void) {
    const char *v = getenv("THEM_BLIT_MS");
    struct timespec ts;
    long ms;

    if (!v)
        return;

    ms = (long)strtoul(v, 0, 10);
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

long blit_buffer_scaled(const u32 *pixels, u32 src_w, u32 src_h) {
    blit_delay();
    if (src_w * src_h > sizeof(frame) / sizeof(frame[0]))
        return 0;

    frame_w = src_w;
    frame_h = src_h;
    frame_count++;
    for (u32 i = 0; i < src_w * src_h; i++)
        frame[i] = pixels[i];

    return 1;
}

long blit_buffer(const u32 *pixels) { return blit_buffer_scaled(pixels, 640, 400); }

typedef struct {
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
} __attribute__((packed)) FBInfo_T;

/* The harness always offers a linear framebuffer, so the emulator takes the
 * blit path and every frame lands in one buffer this file can dump. */
long get_fb_info(FBInfo_T *info) {
    info->width = 1024;
    info->height = 768;
    info->pitch = 1024 * 4;
    info->bpp = 32;
    return 0;
}

/* Mirrors the kernel's mode 13h renderer so host runs can be screenshotted. */
/* A plain 8x8 block font is enough for the harness: the messages only need
 * to be distinguishable, and the r2 build uses the kernel's real glyphs. */
long get_kernel_font(u8 *buf, u64 size) {
    for (u64 i = 0; i < size; i++)
        buf[i] = (u8)((i % 8 == 0 || i % 8 == 7) ? 0xFF : 0x81);

    return 8;
}

long write_vga(const u8 *buf, const u8 *pal) {
    blit_delay();
    frame_w = 320;
    frame_h = 200;
    frame_count++;

    for (u32 i = 0; i < 320u * 200u; i++) {
        u32 idx = buf[i];
        frame[i] = ((u32)pal[idx * 3] << 16) | ((u32)pal[idx * 3 + 1] << 8) | (u32)pal[idx * 3 + 2];
    }

    return 1;
}
long write_pixel(u32 pos, u16 color) { return 1; }
u64 map_vram(void) { return 0; }
long set_video_mode(u8 mode) { return 0; }

/* --- keyboard -------------------------------------------------------- */

typedef struct {
    u64 at;
    u8 code;
} KeyEvent_T;

static KeyEvent_T key_script[256];
static int key_count = 0;
static int key_next = 0;
static u64 run_ms = 4000;
static int quit_sent = 0;

long pipe_subscribe(const u8 *buf) { return 1; }
long pipe_unsubscribe(const u8 *buf) { return 1; }

long pipe_read(u8 *buf) {
    u64 t = get_ticks();
    int n = 0;

    while (key_next < key_count && key_script[key_next].at <= t && n < 15)
        buf[n++] = key_script[key_next++].code;

    if (!quit_sent && t >= run_ms) {
        quit_sent = 1;
        buf[n++] = 0x58; /* F12: the emulator's own quit key */
    }

    buf[n] = 0;

    /* Capture the screen at the requested marks. */
    while (shot_done < shot_count && t >= shot_times[shot_done]) {
        char path[320];

        snprintf(path, sizeof(path), "%s-%04lu.ppm", out_prefix, (unsigned long)shot_times[shot_done]);
        write_ppm(path);
        fprintf(stderr, "[host] captured %s (%ux%u, frame %lu)\n", path, frame_w, frame_h, frame_count);
        shot_done++;
    }

    return 1;
}

/* --- ports ----------------------------------------------------------- */

long write_port(u16 port, u32 value) { return 1; }

long read_port(u16 port, u32 *value) {
    *value = 0;
    return 1;
}

long play_freq(u16 freq, u16 duration) { return 1; }
long stop_speaker(void) { return 1; }

/* --- filesystem ------------------------------------------------------ */

/*
 *  Resolve a path the way FAT12 does, not the way Linux does.
 *
 *  On the floppy a name is 8.3 and case does not matter, so a guest asking
 *  for "gfx/19.img" finds GFX/19.IMG.  Here the same name misses, and a
 *  harness that fails where the real kernel succeeds is worse than no
 *  harness at all -- every run would need the files renamed first.  Each
 *  component is matched against the directory, case-insensitively, and only
 *  falls back to the literal name when nothing matches.
 */
static void resolve_ci(const char *name, char *out, unsigned long cap) {
    unsigned long n = 0;
    const char *p = name;

    out[0] = '\0';

    if (*p == '/') { /* keep an absolute path absolute */
        if (cap < 2)
            return;
        out[n++] = '/';
        out[n] = '\0'; /* opendir() reads this before the first component */
        p++;
    }

    while (*p) {
        char want[256];
        unsigned long len = 0;
        const char *slash;
        DIR *d;
        struct dirent *e;
        int matched = 0;

        while (p[len] && p[len] != '/')
            len++;
        if (len >= sizeof(want)) /* nothing sane to do with it */
            len = sizeof(want) - 1;
        for (unsigned long i = 0; i < len; i++)
            want[i] = p[i];
        want[len] = '\0';

        slash = p[len] ? "/" : "";
        p += len + (p[len] ? 1 : 0);

        /* Look the component up in whatever directory we have so far. */
        d = opendir(n ? out : ".");
        if (d) {
            while ((e = readdir(d))) {
                unsigned long i = 0;

                while (e->d_name[i] && want[i]) {
                    char a = e->d_name[i], b = want[i];
                    if (a >= 'A' && a <= 'Z')
                        a = (char)(a + 32);
                    if (b >= 'A' && b <= 'Z')
                        b = (char)(b + 32);
                    if (a != b)
                        break;
                    i++;
                }
                if (!e->d_name[i] && !want[i]) {
                    snprintf(want, sizeof(want), "%s", e->d_name);
                    matched = 1;
                    break;
                }
            }
            closedir(d);
        }
        (void)matched; /* no match: keep the literal name and let open fail */

        if (n + strlen(want) + 1 >= cap)
            return;
        n += (unsigned long)snprintf(out + n, cap - n, "%s%s", want, slash);
    }
}

long read_file(const u8 *name, u8 *buffer) {
    char path[1024];
    FILE *f;
    size_t n;

    resolve_ci((const char *)name, path, sizeof(path));
    f = fopen(path, "rb");

    if (!f)
        return 0;

    n = fread(buffer, 1, 1 << 20, f);
    fclose(f);

    return n ? 1 : 0;
}

long read_file_at(const u8 *name, u8 *buffer, u64 offset, u64 length) {
    char path[1024];
    FILE *f;
    size_t n;

    if (!name || !name[0] || !buffer)
        return -1;
    if (length == 0)
        return 0;

    resolve_ci((const char *)name, path, sizeof(path));
    f = fopen(path, "rb");

    if (!f)
        return -1;

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    n = fread(buffer, 1, (size_t)length, f);
    fclose(f);

    return (long)n;
}

long write_file(const u8 *name, const u8 *buffer) {
    char path[1024];
    FILE *f;

    resolve_ci((const char *)name, path, sizeof(path));
    f = fopen(path, "wb");

    if (!f)
        return 0;

    fwrite(buffer, 1, 512, f);
    fclose(f);

    return 1;
}

long write_file_at(const u8 *name, const u8 *buffer, u64 offset, u64 length) {
    char path[1024];
    FILE *f;
    size_t n;

    resolve_ci((const char *)name, path, sizeof(path));
    f = fopen(path, "r+b");

    if (!f)
        f = fopen(path, "w+b");

    if (!f)
        return 0;

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    n = fwrite(buffer, 1, (size_t)length, f);
    fclose(f);

    return (long)n;
}

long delete_file(const u8 *name) {
    char path[1024];

    resolve_ci((const char *)name, path, sizeof(path));

    return unlink(path) == 0;
}

long read_sysinfo(SysInfo_T *si) {
    char cwd[256];

    for (u32 i = 0; i < sizeof(*si); i++)
        ((u8 *)si)[i] = 0;

    /* The kernel reports an absolute working directory here, and the
     * emulator builds the guest's own path out of it, so the harness has to
     * report one too -- but only when it fits, since the field is short. */
    if (getcwd(cwd, sizeof(cwd)) && strlen(cwd) < sizeof(si->system_path)) {
        for (u32 i = 0; cwd[i]; i++)
            si->system_path[i] = (u8)cwd[i];
    } else {
        si->system_path[0] = '.';
    }

    return 1;
}

long read_rtc(RTC_T *rtc) {
    time_t t = time(0);
    struct tm tmv;

    localtime_r(&t, &tmv);
    rtc->seconds = (u8)tmv.tm_sec;
    rtc->minutes = (u8)tmv.tm_min;
    rtc->hours = (u8)tmv.tm_hour;
    rtc->day = (u8)tmv.tm_mday;
    rtc->month = (u8)(tmv.tm_mon + 1);
    rtc->year = (u16)(tmv.tm_year + 1900);

    return 1;
}

long list_dir_path(const u8 *path, VfsDirEntry_T *entries) {
    char dirpath[1024];
    DIR *d;
    struct dirent *e;
    long n = 0;

    resolve_ci((const char *)path, dirpath, sizeof(dirpath));
    d = opendir(dirpath);

    if (!d)
        return 0;

    while ((e = readdir(d)) && n < 64) {
        struct stat st;
        char full[1024];
        unsigned long len = strlen(e->d_name);

        if (len > 31)
            continue;

        snprintf(full, sizeof(full), "%s/%s", dirpath, e->d_name);
        if (stat(full, &st) != 0)
            continue;

        for (u32 i = 0; i < sizeof(entries[n]); i++)
            ((u8 *)&entries[n])[i] = 0;
        for (u32 i = 0; i < len; i++)
            entries[n].name[i] = (u8)e->d_name[i];
        entries[n].name_len = (u8)len;
        entries[n].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        entries[n].size = (u32)st.st_size;
        n++;
    }

    closedir(d);
    return n;
}

/* Dump guest memory for offline disassembly. */
void host_dump(const void *data, unsigned long len) {
    FILE *f = fopen("guest.bin", "wb");

    if (!f)
        return;

    fwrite(data, 1, len, f);
    fclose(f);
    fprintf(stderr, "[host] wrote guest.bin (%lu bytes)\n", len);
}

/* --- harness set-up --------------------------------------------------- */

__attribute__((constructor)) static void host_setup(void) {
    const char *v;

    start_ms = now_ms();

    v = getenv("THEM_OUT");
    if (v)
        snprintf(out_prefix, sizeof(out_prefix), "%s", v);

    v = getenv("THEM_RUN_MS");
    if (v)
        run_ms = (u64)strtoul(v, 0, 10);

    v = getenv("THEM_SHOTS");
    if (v) {
        const char *p = v;
        while (*p && shot_count < 32) {
            shot_times[shot_count++] = (u64)strtoul(p, (char **)&p, 10);
            if (*p == ',')
                p++;
        }
    }

    v = getenv("THEM_KEYS");
    if (v) {
        const char *p = v;
        while (*p && key_count < 256) {
            u64 at = (u64)strtoul(p, (char **)&p, 10);
            if (*p == ':')
                p++;
            key_script[key_count].at = at;
            key_script[key_count].code = (u8)strtoul(p, (char **)&p, 0);
            key_count++;
            if (*p == ',')
                p++;
        }
    }
}
