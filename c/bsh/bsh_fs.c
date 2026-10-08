#include "mem.h"
#include "string.h"
#include "syscall.h"

#include "bsh.h"
#include "bsh_int.h"

/*
 *  Files and directories: ls, cd, mkdir, rmdir, rm, read, mount.
 */

static uint8_t upper(uint8_t c) { return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c; }

/*  <path> as its parent and its last name: "/mnt/fat/A" is "/mnt/fat" and
 *  "A"; "/A" is "/" and "A".  Returns the name, in <path>.  */
static const uint8_t *split_path(const uint8_t *path, uint8_t parent[BSH_PATH]) {
    uint8_t last = 0;
    for (uint8_t k = 0; path[k]; k++)
        if (path[k] == '/')
            last = k;
    bsh_copy(parent, path, (uint8_t)(last + 1));
    if (!last)
        bsh_copy(parent, (const uint8_t *)"/", 2);
    return path + last + 1;
}

/*  Is <name> in <dir>: 1 as a directory, 2 as a file, 0 not there, -1 when
 *  the directory cannot be listed.  */
static int dir_has(const uint8_t *dir, const uint8_t *name) {
    static VfsDirEntry_T entries[64];
    int64_t count = list_dir_path(dir, entries);
    if (count < 0 || count > 64)
        return -1;
    uint32_t nl = strlen(name);
    for (int64_t e = 0; e < count; e++) {
        if (entries[e].name_len != nl)
            continue;
        uint32_t k = 0;
        while (k < nl && upper(entries[e].name[k]) == upper(name[k]))
            k++;
        if (k == nl)
            return entries[e].is_dir ? 1 : 2;
    }
    return 0;
}

/*  A path to change something at: on the floppy or the RAM disk, the FAT12
 *  mounts.  The kernel would take "/" for the floppy's root, and the CD and
 *  the tar are read-only.  Says so when it is not.  */
static int writable(BshSession *s, const char *cmd, const uint8_t *parent, const uint8_t *abs) {
    bsh_load_mounts();
    uint8_t type = bsh_mount_type_at(parent);
    if (type == 2 || type == 5)
        return 1;
    bsh_str(s, cmd);
    bsh_str(s, ": cannot change '");
    bsh_ustr(s, abs);
    bsh_str(s, "': only under /mnt/fat or /mnt/tmp\n");
    return 0;
}

static void fail(BshSession *s, const char *cmd, const char *what, const uint8_t *path) {
    bsh_str(s, cmd);
    bsh_str(s, ": ");
    bsh_str(s, what);
    bsh_ustr(s, path);
    bsh_str(s, "\n");
}

/* ------------------------------------------------------------------------ */

int bsh_cmd_ls(BshSession *s, const uint8_t *arg) {
    uint8_t abs[BSH_PATH];
    bsh_abs_path(s, abs, arg);

    /*  The root and "/mnt": from the mount table, each mount point with its
     *  filesystem.  */
    bsh_load_mounts();
    if (bsh_mount_dir(abs)) {
        const uint8_t *seen[BSH_MAX_MOUNTS];
        uint8_t seen_len[BSH_MAX_MOUNTS];
        int shown = 0;
        uint8_t pl = (uint8_t)strlen(abs);
        for (int i = 0; i < bsh_mnt_count; i++) {
            const uint8_t *name;
            uint8_t len = bsh_mount_child(&bsh_mnt[i], abs, &name);
            if (!len)
                continue;
            int dup = 0;
            for (int k = 0; k < shown && !dup; k++)
                dup = seen_len[k] == len && memcmp(seen[k], name, len) == 0;
            if (dup)
                continue;
            seen[shown] = name;
            seen_len[shown++] = len;
            bsh_str(s, "  ");
            bsh_out(s, name, len);
            bsh_str(s, "/  <DIR>");
            /*  A mount point itself: what is mounted there.  */
            if (bsh_mnt[i].path_len == (pl == 1 ? 1 : pl + 1) + len) {
                bsh_str(s, "  ");
                bsh_str(s, bsh_fs_name(bsh_mnt[i].fs_type));
            }
            bsh_str(s, "\n");
        }
        if (!shown)
            bsh_str(s, "  (empty)\n");
        return 0;
    }

    /* 64 entries: syscall 0x2D takes no capacity argument and writes one per
     * directory member, up to 64.  A 32-entry buffer is overrun silently. */
    static VfsDirEntry_T entries[64];
    uint8_t type = bsh_mount_type_at(abs);
    int64_t count = (type == 0 || type == 1) ? -1 : list_dir_path(abs, entries);
    if (count < 0 || count > 64) {
        fail(s, "ls", "no such directory: ", abs);
        return 0;
    }

    for (int64_t i = 0; i < count; i++) {
        VfsDirEntry_T *e = &entries[i];
        bsh_str(s, "  ");
        bsh_out(s, e->name, e->name_len);
        if (e->is_dir) {
            bsh_str(s, "/  <DIR>\n");
        } else {
            bsh_str(s, "  ");
            bsh_u64(s, e->size, 0);
            bsh_str(s, " bytes\n");
        }
    }
    if (count == 0)
        bsh_str(s, "  (empty)\n");
    return 0;
}

int bsh_cmd_cd(BshSession *s, const uint8_t *arg) {
    /*  "cd" alone: back where the session started.  */
    const uint8_t *to = arg[0] ? arg : s->start;
    if (!bsh_cd(s, to)) {
        uint8_t abs[BSH_PATH];
        bsh_abs_path(s, abs, to);
        fail(s, "cd", "no such directory: ", abs);
    }
    return 0;
}

/*  One to eight characters of what FAT allows in a name.  No extension: the
 *  kernel makes "A.B" a directory listed as plain "A", which could not then
 *  be found by the name it was given.  */
static int dir_name_ok(const uint8_t *n) {
    uint32_t len = 0;
    for (; *n; n++, len++) {
        uint8_t c = *n;
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        for (const char *k = "!#$%&'()-@^_`{}~"; *k && !ok; k++)
            ok = c == (uint8_t)*k;
        if (!ok)
            return 0;
    }
    return len >= 1 && len <= 8;
}

/*
 *  The kernel (syscall 0x27) makes a FAT name of whatever it is given,
 *  cutting a longer one short, and does not say whether the directory came
 *  to be --- not when the name is taken, not when the disk is full --- so the
 *  name is checked first and the directory looked for afterwards.
 */
int bsh_cmd_mkdir(BshSession *s, const uint8_t *arg) {
    if (!arg[0]) {
        bsh_str(s, "mkdir: usage: mkdir <path>\n");
        return 0;
    }
    uint8_t abs[BSH_PATH], parent[BSH_PATH];
    bsh_abs_path(s, abs, arg);
    const uint8_t *name = split_path(abs, parent);

    if (!dir_name_ok(name)) {
        bsh_str(s, "mkdir: '");
        bsh_ustr(s, name);
        bsh_str(s, "' cannot be a directory name: up to 8 letters, digits or -_, no dot\n");
        return 0;
    }
    if (!writable(s, "mkdir", parent, abs))
        return 0;

    int had = dir_has(parent, name);
    if (had < 0) {
        fail(s, "mkdir", "no such directory: ", parent);
        return 0;
    }
    if (had) {
        fail(s, "mkdir", had == 1 ? "already there: " : "a file is there already: ", abs);
        return 0;
    }
    if (!write_subdir(parent, name))
        fail(s, "mkdir", "cannot create ", abs);
    else if (dir_has(parent, name) != 1)
        fail(s, "mkdir", "not created (is the disk full?): ", abs);
    return 0;
}

/*
 *  Deleting (syscall 0x23, a file with 0 and an empty directory with 1) takes
 *  a bare name in the kernel's working directory, so it is done from the
 *  parent's.  The kernel says when nothing was deleted.
 */
static int remove_entry(BshSession *s, const char *cmd, const uint8_t *arg, int dir) {
    if (!arg[0]) {
        bsh_str(s, cmd);
        bsh_str(s, dir ? ": usage: rmdir <path>\n" : ": usage: rm <path>\n");
        return 0;
    }
    uint8_t abs[BSH_PATH], parent[BSH_PATH];
    bsh_abs_path(s, abs, arg);
    const uint8_t *name = split_path(abs, parent);
    if (!name[0] || !writable(s, cmd, parent, abs))
        return 0;

    int had = dir_has(parent, name);
    if (had <= 0) {
        fail(s, cmd, "no such file or directory: ", abs);
        return 0;
    }
    if (had != (dir ? 1 : 2)) {
        fail(s, cmd, dir ? "not a directory: " : "a directory (rmdir removes one): ", abs);
        return 0;
    }

    bsh_kernel_cwd(parent);
    int64_t r = syscall(ScDeleteFile, (int64_t)name, dir ? 1 : 0, 0);
    bsh_kernel_cwd_for(s);
    if (r)
        fail(s, cmd, dir ? "not removed (is it empty?): " : "not deleted: ", abs);
    return 0;
}

int bsh_cmd_rmdir(BshSession *s, const uint8_t *arg) { return remove_entry(s, "rmdir", arg, 1); }
int bsh_cmd_rm(BshSession *s, const uint8_t *arg) { return remove_entry(s, "rm", arg, 0); }

/* ------------------------------------------------------------------------ */

int64_t bsh_file_size(const uint8_t *path) {
    uint8_t probe;

    /* A directory, or nothing at all, is an error here; an empty file reads 0. */
    if (read_file_at(path, &probe, 0, 1) < 0)
        return -1;

    uint8_t parent[BSH_PATH];
    const uint8_t *name = split_path(path, parent);
    uint32_t name_len = strlen(name);

    static VfsDirEntry_T entries[64];
    int64_t count = list_dir_path(parent, entries);
    if (count < 0 || count > 64)
        return BSH_SIZE_UNKNOWN;

    for (int64_t e = 0; e < count; e++) {
        if (entries[e].is_dir || entries[e].name_len != name_len)
            continue;
        uint32_t k = 0;
        while (k < name_len && upper(entries[e].name[k]) == upper(name[k]))
            k++;
        if (k == name_len)
            return entries[e].size;
    }
    return BSH_SIZE_UNKNOWN;
}

#define READ_PIECE (32u * 1024) /* read_file_at() at a time */

int bsh_load_file(BshSession *s, const char *cmd, const uint8_t *path, uint8_t **out, uint32_t *out_len) {
    int64_t size = bsh_file_size(path);
    if (size == -1) {
        fail(s, cmd, "cannot read ", path);
        return 0;
    }
    if (size > (int64_t)BSH_READ_MAX) {
        bsh_str(s, cmd);
        bsh_str(s, ": ");
        bsh_u64(s, (uint64_t)size, 0);
        bsh_str(s, " bytes is more than it reads\n");
        return 0;
    }

    /*  Its size from the directory when it says, else grown as the file turns
     *  out to go on.  */
    uint32_t cap = size >= 0 ? (uint32_t)size : 64u * 1024;
    uint8_t *buf = malloc(cap ? cap : 1);
    uint32_t len = 0;
    int err = 0;
    while (buf) {
        if (len == cap) {
            if (size >= 0 || cap >= BSH_READ_MAX)
                break;
            uint32_t more = cap * 2 > BSH_READ_MAX ? BSH_READ_MAX : cap * 2;
            uint8_t *grown = realloc(buf, more);
            if (!grown) {
                free(buf);
                buf = 0;
                break;
            }
            buf = grown;
            cap = more;
        }
        uint32_t want = cap - len < READ_PIECE ? cap - len : READ_PIECE;
        int64_t got = read_file_at(path, buf + len, len, want);
        if (got < 0) {
            err = 1;
            break;
        }
        len += (uint32_t)got;
        if ((uint32_t)got < want)
            break; /* the end of it */
    }
    if (!buf) {
        bsh_str(s, cmd);
        bsh_str(s, ": no memory for ");
        bsh_u64(s, size >= 0 ? (uint64_t)size : cap, 0);
        bsh_str(s, " bytes\n");
        return 0;
    }
    if (err) {
        free(buf);
        fail(s, cmd, "failed reading ", path);
        return 0;
    }
    *out = buf;
    *out_len = len;
    return 1;
}

/*
 *  `read`: the whole file into the user heap, then out.  It used to be read
 *  with read_file() into 4 KiB, which does not know the buffer's size, and a
 *  bigger file wrote on past it.  (tnt has its own, which streams the file
 *  over TCP with resends.)
 */
int bsh_cmd_read(BshSession *s, const uint8_t *arg) {
    if (!arg[0]) {
        bsh_str(s, "read: usage: read <path>\n");
        return 0;
    }
    uint8_t abs[BSH_PATH];
    bsh_abs_path(s, abs, arg);
    uint8_t *buf;
    uint32_t len;
    if (!bsh_load_file(s, "read", abs, &buf, &len))
        return 0;
    if (!len)
        bsh_str(s, "(empty file)\n");
    else {
        bsh_out(s, buf, len);
        if (buf[len - 1] != '\n')
            bsh_str(s, "\n");
    }
    free(buf);
    return 0;
}

int bsh_cmd_mount(BshSession *s, const uint8_t *arg) {
    (void)arg;
    bsh_load_mounts();
    if (!bsh_mnt_count) {
        bsh_str(s, "No mounts.\n");
        return 0;
    }
    for (int i = 0; i < bsh_mnt_count; i++) {
        bsh_out(s, bsh_mnt[i].path, bsh_mnt[i].path_len);
        bsh_str(s, " (");
        bsh_str(s, bsh_fs_name(bsh_mnt[i].fs_type));
        bsh_str(s, ")\n");
    }
    return 0;
}
