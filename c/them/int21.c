#include "cpu.h"
#include "int.h"
#include "io.h"
#include "log.h"
#include "mem.h"
#include "mmu.h"
#include "printf.h"
#include "syscall.h"
#include "winhost.h"

/*
 *  int21.c
 *
 *  The DOS services a .COM program expects to find behind INT 21h.
 *
 *  https://www.stanislavs.org/helppc/int_21.html
 *
 *  krusty@vxn.dev / Nov 3, 2025
 */

#define DOS_HANDLES 20
#define DOS_NO_FILE 0xFF
#define DOS_NESTING 4
#define FS_ATTEMPTS 4
#define DOS_FILE_CAP 65024

/*
 *  Room for the program image being loaded.
 *
 *  This is the ceiling on the size of a program the emulator can start: the
 *  kernel's read takes no length and only accepts a buffer inside the
 *  program image, so the whole file has to fit here, and what fits here is
 *  what is left of the two megabytes a process gets.
 */
#define DOS_IMAGE_CAP (128 * 1024)

typedef struct {
    uint8_t refs; /* references from the current process and its EXEC parents */
    uint8_t name[64];
    uint8_t *data;
    uint32_t size;
    uint32_t pos;
    uint8_t modified;
    uint8_t buffered; /* 1 when <data> holds the file, which writing needs */
} DosFile_T;

static DosFile_T handles[DOS_HANDLES];
/* DOS handle numbers belong to each process. Inherited handles refer to the
 * same open file (and position), but a child closing one leaves the parent's
 * reference alive. */
static uint8_t file_handles[DOS_HANDLES];

typedef struct {
    CPU_T cpu;
    CpuIrqState_T irqs;
    uint16_t psp;
    uint16_t dta_seg;
    uint16_t dta_off;
    uint8_t files[DOS_HANDLES];
} DosParent_T;

static DosParent_T parents[DOS_NESTING];
static uint8_t parent_depth = 0;
static uint16_t child_code = 0;

static void handle_flush(DosFile_T *f);
static void handle_close(uint16_t h);

/*
 *  One staging area for every file the kernel reads on our behalf.
 *
 *  The read syscall takes no length and only accepts a buffer inside the
 *  program image, so a file cannot be read straight into the emulator's RAM
 *  or into a heap block. It lands here first and is copied on.
 */
static uint8_t staging[DOS_IMAGE_CAP];

/* Which part of the file the staging area currently holds. A file too big
 * to hold at once is worked through as a series of these windows. */
static uint32_t staging_base = 0;
static uint32_t staging_len = 0;
static uint16_t psp_segment = 0x1000;
static uint16_t dta_seg = 0x1000;
static uint16_t dta_off = 0x0080;
static uint8_t exit_code = 0;

/* Track live paragraph ranges so resize/free really change available RAM.
 * The program's PSP block participates just like an AH=48h allocation. */
#define DOS_BLOCKS 128
#define DOS_MEMORY_END 0xA000
static struct {
    uint16_t seg, size, owner;
} blocks[DOS_BLOCKS];
static uint16_t arena_start;

static int16_t block_find(uint16_t seg) {
    for (uint16_t i = 0; i < DOS_BLOCKS; i++)
        if (blocks[i].owner && blocks[i].seg == seg)
            return (int16_t)i;
    return -1;
}

static int16_t block_add(uint16_t seg, uint16_t size, uint16_t owner) {
    for (uint16_t i = 0; i < DOS_BLOCKS; i++) {
        if (!blocks[i].owner) {
            blocks[i].seg = seg;
            blocks[i].size = size;
            blocks[i].owner = owner;
            return (int16_t)i;
        }
    }
    return -1;
}

static void block_cursor(Memory_T *mem) {
    uint16_t top = arena_start;
    for (uint16_t i = 0; i < DOS_BLOCKS; i++)
        if (blocks[i].owner && blocks[i].seg + blocks[i].size > top)
            top = blocks[i].seg + blocks[i].size;
    mem->next_free_seg = top;
}

/* Size of the hole at base, or zero if base lies in a live block. */
static uint16_t block_room(uint16_t base, int16_t ignore) {
    uint16_t end = DOS_MEMORY_END;
    for (uint16_t i = 0; i < DOS_BLOCKS; i++) {
        if (!blocks[i].owner || (int16_t)i == ignore)
            continue;
        if (base >= blocks[i].seg && base < blocks[i].seg + blocks[i].size)
            return 0;
        if (blocks[i].seg >= base && blocks[i].seg < end)
            end = blocks[i].seg;
    }
    return end > base ? end - base : 0;
}

void dos_init(uint16_t psp) {
    arena_start = psp;
    for (uint16_t i = 0; i < DOS_BLOCKS; i++)
        blocks[i].owner = 0;
    psp_segment = psp;
    dta_seg = psp;
    dta_off = 0x0080;
    exit_code = 0;
    parent_depth = 0;
    child_code = 0;

    for (uint8_t i = 0; i < DOS_HANDLES; i++) {
        handles[i].refs = 0;
        file_handles[i] = DOS_NO_FILE;
    }

    /* 0-4 are the standard devices DOS opens for every program. */
    for (uint8_t i = 0; i < 5; i++)
        handles[i].refs = 1;
}

/*
 *  Hand back everything the DOS layer is still holding.
 *
 *  Called once on the way out. A guest that ends without closing its files
 *  leaves their buffers here, and the kernel does not take them back when
 *  the process goes.
 */
void dos_shutdown(void) {
    for (uint8_t i = 0; i < DOS_HANDLES; i++) {
        handle_flush(&handles[i]);
        if (handles[i].data) {
            free(handles[i].data);
            handles[i].data = (uint8_t *)0;
        }

        handles[i].refs = 0;
        handles[i].buffered = 0;
        file_handles[i] = DOS_NO_FILE;
    }
    parent_depth = 0;
}

uint8_t dos_exit_code(void) { return exit_code; }

static void dos_ok(CPU_T *cpu) { set_flag(cpu, CF, 0); }

static void dos_fail(CPU_T *cpu, uint16_t err) {
    cpu->AX = err;
    set_flag(cpu, CF, 1);
}

/*
 *  A program that started another program.
 *
 *  DOS keeps the parent in memory while the child runs and hands control
 *  back when the child ends. There is only one CPU here, so the parent's
 *  registers are put aside and picked up again on the way out -- which is
 *  the whole of EXEC, once the child has been loaded somewhere that does
 *  not land on top of the parent.
 */
static void dos_terminate(Memory_T *mem, CPU_T *cpu, uint8_t code) {
    if (parent_depth) {
        DosParent_T *p = &parents[--parent_depth];

        /* Close the child's table before restoring the parent's. This drops
         * inherited references too, without destroying the parent's files. */
        for (uint8_t i = 5; i < DOS_HANDLES; i++)
            handle_close(i);
        for (uint8_t i = 0; i < DOS_HANDLES; i++)
            file_handles[i] = p->files[i];

        for (uint16_t i = 0; i < DOS_BLOCKS; i++)
            if (blocks[i].owner == psp_segment)
                blocks[i].owner = 0;
        block_cursor(mem);
        child_code = code;

        *cpu = p->cpu;
        cpu_irq_restore(&p->irqs);
        psp_segment = p->psp;
        dta_seg = p->dta_seg;
        dta_off = p->dta_off;

        /* The parent resumes just past its INT 21h, told the child ran. */
        set_flag(cpu, CF, 0);
        cpu->AX = 0;

        (void)mem;
        return;
    }

    exit_code = code;
    cpu_halt();
}

/*
 *  The working directory, as the kernel spells it.
 *
 *  The kernel answers this out of its configuration, which it will not wait
 *  for: when another task holds it at that instant -- the network driver,
 *  the web server, the shell drawing its prompt -- the call writes nothing
 *  and still reports success. A caller that took one answer on trust would
 *  take whatever was in the buffer for the current directory, and a guest
 *  started under that goes looking for its files in the root.  So the buffer
 *  is cleared first, and an empty answer is asked again: the lock is never
 *  held for more than a moment, and a syscall runs with interrupts on, so
 *  whoever holds it gets to finish between one attempt and the next.  There
 *  is no sleeping in between. A sleep is a wait on the scheduler, the one
 *  kind of wait the emulator has never made, and a foreground task that
 *  makes it is not woken again.
 */
#define CWD_ATTEMPTS 32

/*
 *  Asked for once.
 *
 *  The directory cannot change under a guest -- nothing it does moves the
 *  emulator -- and every name a guest opens is measured against it, so
 *  asking the kernel each time is a syscall per name for an answer that was
 *  settled before the guest started.
 */
static uint8_t cwd_cache[40];
static uint8_t cwd_known = 0;

/*
 *  Where the guest starts, when the emulator is told rather than asking the
 *  kernel: in a window it runs a game from its own directory, which is where
 *  a DOS game expects to be started and where it looks for its files.
 */
void dos_set_cwd(const uint8_t *dir) {
    uint16_t n = 0;

    while (dir[n] && n < sizeof(cwd_cache) - 1) {
        cwd_cache[n] = dir[n];
        n++;
    }
    cwd_cache[n] = 0;
    cwd_known = n != 0;
}

uint8_t dos_cwd(uint8_t *out, uint16_t cap) {
    if (cwd_known) {
        uint16_t n = 0;

        while (n < cap - 1 && cwd_cache[n]) {
            out[n] = cwd_cache[n];
            n++;
        }
        out[n] = 0;

        return 1;
    }

    for (uint8_t attempt = 0; attempt < CWD_ATTEMPTS; attempt++) {
        SysInfo_T si;
        uint16_t n = 0;

        for (uint16_t i = 0; i < sizeof(si); i++)
            ((uint8_t *)&si)[i] = 0;

        /* The kernel's answer starts with a slash. "." is the workstation
         * harness saying the real directory does not fit the field, and
         * is as good a name for it as any. */
        if (read_sysinfo(&si) && (si.system_path[0] == '/' || si.system_path[0] == '.')) {
            while (n < cap - 1 && n < sizeof(si.system_path) && si.system_path[n] && si.system_path[n] != '\n' && si.system_path[n] != '\r') {
                out[n] = si.system_path[n];
                n++;
            }
            out[n] = 0;

            for (uint16_t i = 0; i < sizeof(cwd_cache) - 1 && i < n; i++)
                cwd_cache[i] = out[i];
            cwd_cache[(n < sizeof(cwd_cache) - 1) ? n : sizeof(cwd_cache) - 1] = 0;
            cwd_known = 1;

            return 1;
        }
    }

    out[0] = '/';
    out[1] = 0;

    return 0;
}

/*
 *  Make <name> absolute, in place, by putting the working directory in
 *  front of a relative one.
 *
 *  The kernel would do this itself, but it resolves a relative name against
 *  a directory it reads without waiting for, and against the root when it
 *  cannot get at it -- so a relative name finds its file most of the time.
 *  An absolute one finds it every time.
 */
void dos_absolute(uint8_t *name, uint16_t cap) {
    uint8_t cwd[40];
    uint16_t clen = 0;
    uint16_t nlen = 0;

    if (name[0] == '/')
        return;

    if (!dos_cwd(cwd, sizeof(cwd)))
        return; /* nothing to put in front of it; the kernel gets it as is */

    while (cwd[clen])
        clen++;
    while (name[nlen])
        nlen++;

    if (clen && cwd[clen - 1] == '/')
        clen--; /* the root already ends in its slash */

    if (clen + 1 + nlen + 1 > cap)
        return; /* does not fit; left relative, which may still work */

    for (int32_t i = (int32_t)nlen; i >= 0; i--)
        name[clen + 1 + i] = name[i];
    for (uint16_t i = 0; i < clen; i++)
        name[i] = cwd[i];
    name[clen] = '/';
}

/*
 *  Copy an ASCIIZ path out of guest memory, in the kernel's spelling.
 *
 *  DOS writes a path the DOS way -- a drive letter and backslashes -- and
 *  the kernel splits on forward slashes and knows nothing about drives.
 *  A name is converted here, on its way in, so every service below sees one
 *  form: "C:\MNT\FAT\GFX\19.IMG" is /MNT/FAT/GFX/19.IMG, and the root of
 *  the one drive is r2's root, as the program's own path in the environment
 *  says (AH=47h answers likewise). That root holds nothing but MNT, so a
 *  guest's files are named relative to its directory or in full. It comes
 *  out absolute, too.
 *
 *  That last part is not tidiness. The kernel resolves a relative name
 *  against the floppy, whatever the working directory happens to be, so a
 *  guest running off the CD that asked for "gfx/19.img" would be answered
 *  from the floppy -- or not at all. Saying which directory is meant is
 *  the only way to be read from the medium the guest was started from.
 *  The directory itself is remembered rather than asked for each time.
 */
static void guest_string(Memory_T *mem, uint16_t seg, uint16_t off, uint8_t *out, uint16_t cap) {
    uint16_t i = 0;
    uint16_t n = 0;

    /* "C:" names the drive the emulator is already running from. */
    if (mem_r8(mem, seg, (uint16_t)(off + 1)) == ':')
        i = 2;

    while (n < cap - 1) {
        uint8_t c = mem_r8(mem, seg, (uint16_t)(off + i++));
        if (!c)
            break;

        /*
         *  Drop the padding out of an 8.3 name.
         *
         *  A name cannot hold a space in 8.3, so one only ever got there by
         *  being padded out to the eight and three a directory entry stores.
         *  DOS parses "HIGHSC  .CDM" and "HIGHSC.CDM" into the same eleven
         *  bytes and opens the same file for both; a program that builds its
         *  filenames out of a padded table -- and several do -- is relying on
         *  exactly that. Handed on with the spaces still in, the name matches
         *  nothing and the program is left opening files that are right there.
         */
        if (c == ' ')
            continue;

        out[n++] = (uint8_t)(c == '\\' ? '/' : c);
    }

    out[n] = 0;

    dos_absolute(out, cap);
}

static uint8_t path_matches(const uint8_t *entry, uint8_t entry_len, const uint8_t *want) {
    uint8_t i = 0;

    /* Compare the last path component, case-insensitively. */
    while (want[i] && i < entry_len) {
        uint8_t a = entry[i];
        uint8_t b = want[i];

        if (a >= 'a' && a <= 'z')
            a = (uint8_t)(a - 32);
        if (b >= 'a' && b <= 'z')
            b = (uint8_t)(b - 32);
        if (a != b)
            return 0;
        i++;
    }

    return (uint8_t)(i == entry_len && !want[i]);
}

/*
 *  Split <name> into the directory it lives in and its last component.
 *
 *  Writes the directory into <dir> and returns the component. A name with
 *  no directory part belongs to the working directory, which is the one the
 *  emulator itself was started in.
 */
static const uint8_t *split_dir(const uint8_t *name, uint8_t *dir, uint16_t cap) {
    const uint8_t *base = name;
    uint16_t dir_len = 0;

    for (uint16_t i = 0; name[i]; i++)
        if (name[i] == '/' || name[i] == '\\') {
            base = &name[i + 1];
            dir_len = i ? i : 1; /* "/file" lives in the root */
        }

    if (dir_len && dir_len < cap - 1) {
        for (uint16_t i = 0; i < dir_len; i++)
            dir[i] = name[i];
        dir[dir_len] = 0;
    } else {
        dos_cwd(dir, cap);
    }

    return base;
}

/* Ask the kernel for a listing, retrying: the floppy controller drops a
 * transfer now and then, and an empty listing is worth asking about twice. */
static int64_t list_dir_retry(const uint8_t *dir, VfsDirEntry_T *entries) {
    int64_t count = 0;

    for (uint8_t attempt = 0; attempt < FS_ATTEMPTS && count <= 0; attempt++)
        count = list_dir_path(dir, entries);

    /* Anything outside [0, 64] is the syscall reporting an error. */
    return (count < 0 || count > 64) ? 0 : count;
}

/*
 *  Size of <name>, or 0 when it is not there.
 *
 *  The kernel's file read takes no length, so the size has to come from the
 *  directory listing before anything is read into a fixed buffer. A name
 *  may carry a directory part; what is left of it is looked up in the
 *  working directory.
 */
uint32_t dos_file_size(const uint8_t *name) {
    /* The listing syscall takes no capacity and writes one entry per
     * directory member up to 64, so anything shorter is silently overrun. */
    static VfsDirEntry_T entries[64];
    uint8_t dir[64];
    const uint8_t *base = split_dir(name, dir, sizeof(dir));
    int64_t count = list_dir_retry(dir, entries);

    for (int64_t i = 0; i < count && i < 64; i++)
        if (!entries[i].is_dir && path_matches(entries[i].name, entries[i].name_len, base))
            return entries[i].size;

    return 0;
}

/*
 *  Directory searches -- INT 21h AH=4Eh and AH=4Fh.
 *
 *  DOS keeps its place in the caller's disk transfer area, which is what
 *  lets a program run two searches at once. Here there is one search at a
 *  time: the whole directory is listed up front and walked through, which is
 *  what a program listing one panel of files wants and all it wants.
 */
static struct {
    VfsDirEntry_T entries[64];
    int64_t count;
    int64_t next;
    uint8_t pattern[11]; /* the mask, in the padded 8.3 shape */
    uint8_t attr;
} search;

/*
 *  Lay a name out the way a directory entry holds it: eight characters of
 *  name, three of extension, space padded and upper case.  In a mask, a star
 *  means the rest of its field is anything at all, which is exactly a run of
 *  question marks.
 */
static void pattern83(const uint8_t *name, uint8_t *out) {
    uint8_t field = 0; /* 0 = name, 1 = extension */
    uint8_t i = 0;
    uint8_t n = 0;

    for (uint8_t k = 0; k < 11; k++)
        out[k] = ' ';

    while (name[i]) {
        uint8_t c = name[i++];

        if (c == '.') {
            field = 1;
            n = 0;
            continue;
        }

        if (c >= 'a' && c <= 'z')
            c = (uint8_t)(c - 32);

        if (c == '*') {
            while (n < (field ? 3 : 8))
                out[(field ? 8 : 0) + n++] = '?';
            continue;
        }

        if (n < (field ? 3 : 8))
            out[(field ? 8 : 0) + n++] = c;
    }
}

/* 1 when a directory entry's name answers to the mask. */
static uint8_t pattern_matches(const uint8_t *entry, uint8_t len, const uint8_t *pattern) {
    uint8_t text[33];
    uint8_t name[11];

    if (len > sizeof(text) - 1)
        len = sizeof(text) - 1;
    for (uint8_t i = 0; i < len; i++)
        text[i] = entry[i];
    text[len] = 0;

    /* A plain name carries no stars, so the same laying out serves. */
    pattern83(text, name);

    for (uint8_t i = 0; i < 11; i++)
        if (pattern[i] != '?' && pattern[i] != name[i])
            return 0;

    return 1;
}

/*
 *  Pull a file into the staging area.
 *
 *  The floppy driver reports no error and retries nothing, so a dropped
 *  transfer looks like a file full of zeros. Read until something arrives:
 *  the controller is re-initialised on each attempt, and a failed read is
 *  the common reason a program refuses to start.
 *
 *  Returns how many bytes of the staging area are meaningful, which is the
 *  file's own size when the directory could tell us and <fallback> when it
 *  could not.
 */
static uint32_t staging_read(const uint8_t *name, uint32_t fallback) {
    uint32_t size = dos_file_size(name);

    if (!size)
        size = fallback;
    if (!size)
        return 0;

    /*
     *  Refuse what will not fit rather than truncating it.
     *
     *  The read syscall takes no length: it writes the whole file, however
     *  big, into whatever buffer it is handed. Clamping the size we report
     *  does nothing about that -- the bytes past the end of the staging area
     *  are still written -- so a file bigger than the area cannot be read at
     *  all until the kernel grows a read that takes a length. A guest told
     *  it cannot open the file is a great deal better off than one running
     *  in memory the read has just walked through.
     *
     *  This only catches the sizes we know. A caller that could not get one
     *  from the directory and passes a <fallback> is still taking the file
     *  on trust.
     */
    if (size > sizeof(staging))
        return 0;

    staging_base = 0;
    staging_len = size;

    for (uint8_t attempt = 0; attempt < FS_ATTEMPTS; attempt++) {
        uint8_t empty = 1;

        for (uint32_t i = 0; i < sizeof(staging); i++)
            staging[i] = 0;

        if (read_file(name, staging)) {
            for (uint32_t i = 0; i < size && empty; i++)
                if (staging[i])
                    empty = 0;

            if (!empty)
                return size;
        }
    }

    return 0;
}

/*
 *  Read <name> into <dest>, at most <cap> bytes. Returns the number of
 *  bytes read, or 0 when the file could not be read.
 *
 *  The size comes from the directory listing, because the kernel's read
 *  reports no length. That lookup is the fragile part -- one listing
 *  returns a bounded number of entries, and the name may live somewhere it
 *  cannot enumerate -- so a caller that can cope with not knowing the exact
 *  length passes a <fallback> to use instead. The staging area is zeroed
 *  before every read, so the bytes past the end of a short file are zeros
 *  rather than the remains of the previous one.
 */
uint32_t dos_read_file(const uint8_t *name, uint8_t *dest, uint32_t cap, uint32_t fallback) {
    uint32_t size = staging_read(name, fallback);

    if (!size)
        return 0;

    if (size > cap)
        size = cap;

    for (uint32_t i = 0; i < size; i++)
        dest[i] = staging[i];

    return size;
}

/*
 *  Program loading.
 *
 *  DOS decides how to load a file by what is in it, not by its name: an
 *  image starting with the "MZ" signature is a relocatable executable, and
 *  anything else is a .COM -- a flat image that runs at a caller-supplied
 *  offset (normally 0x100) of a single segment it owns entirely.
 */

#define EXE_SIGNATURE_MZ 0x5A4D
#define EXE_SIGNATURE_ZM 0x4D5A

/*
 *  Pull the <len> bytes at <off> of <name> into the staging area.
 *
 *  This is the read that is told how much room it has, so it is the one a
 *  file bigger than the staging area has to be read through: the caller asks
 *  for a window at a time and works its way along.  Returns how many bytes
 *  arrived, which is short at the end of the file.
 */
static uint32_t staging_window(const uint8_t *name, uint32_t off, uint32_t len) {
    staging_base = off;
    staging_len = 0;

    if (len > sizeof(staging))
        len = sizeof(staging);
    if (!len)
        return 0;

    for (uint8_t attempt = 0; attempt < FS_ATTEMPTS; attempt++) {
        int64_t got;

        for (uint32_t i = 0; i < len; i++)
            staging[i] = 0;

        got = read_file_at(name, staging, off, len);

        /* Nothing there is an answer; a failure is worth asking again, the
         * floppy being what it is. */
        if (got == 0)
            return 0;
        if (got > 0) {
            staging_len = (uint32_t)got;
            return staging_len;
        }
    }

    return 0;
}

/* A word at a file offset, out of whatever window is loaded. */
static uint16_t staging_word(uint32_t offset) {
    uint32_t i;

    if (offset < staging_base)
        return 0;

    i = offset - staging_base;

    if (i + 1 >= staging_len || i + 1 >= sizeof(staging))
        return 0;

    return (uint16_t)(staging[i] | ((uint16_t)staging[i + 1] << 8));
}

/*
 *  Lay an .EXE out in the guest's memory.
 *
 *  The image follows the header, loads one paragraph past the PSP, and
 *  carries a table of the places inside it that name a segment. Those
 *  words hold segment numbers relative to the start of the image, so each
 *  one has the real load address added to it -- that is the whole of DOS
 *  relocation, and without it every far call in the program goes nowhere.
 */
static uint8_t load_exe(Memory_T *mem, CPU_T *cpu, uint16_t psp_seg, const uint8_t *name, uint32_t file_size) {
    uint16_t pages = staging_word(0x04);
    uint16_t last_page = staging_word(0x02);
    uint16_t relocations = staging_word(0x06);
    uint16_t header_paragraphs = staging_word(0x08);
    uint16_t min_alloc = staging_word(0x0A);
    uint16_t exe_ss = staging_word(0x0E);
    uint16_t exe_sp = staging_word(0x10);
    uint16_t exe_ip = staging_word(0x14);
    uint16_t exe_cs = staging_word(0x16);
    uint16_t reloc_table = staging_word(0x18);

    uint16_t load_seg = (uint16_t)(psp_seg + 0x10);
    uint32_t header_size = (uint32_t)header_paragraphs * 16;
    uint32_t total;
    uint32_t image_size;

    if (!header_paragraphs || header_size >= file_size)
        return 0;

    /* The last page is only partly used unless the count says otherwise. */
    total = (uint32_t)pages * 512;
    if (pages && last_page)
        total = total - 512 + last_page;
    if (!total || (file_size && total > file_size))
        total = file_size;
    if (total <= header_size)
        return 0;

    image_size = total - header_size;
    if (image_size > (uint32_t)(0xA000 - load_seg) * 16)
        return 0;

    /*
     *  Bring the image in a window at a time.
     *
     *  Only the image is loaded, not the file: an .EXE that carries its
     *  overlays behind the load module is mostly not the program, and a
     *  loader that insisted on holding the whole file could not start one.
     *
     *  It is written straight into the guest's RAM rather than through the
     *  segment helpers, because it can be longer than the 64 KiB an offset
     *  reaches.
     */
    {
        uint32_t base = (uint32_t)load_seg << 4;
        uint32_t done = 0;

        while (done < image_size) {
            uint32_t want = image_size - done;
            uint32_t got;

            if (want > sizeof(staging))
                want = sizeof(staging);

            got = staging_window(name, header_size + done, want);
            if (!got)
                return 0;

            for (uint32_t i = 0; i < got && base + done + i < sizeof(mem->bytes); i++)
                mem->bytes[base + done + i] = staging[i];

            done += got;
        }
    }

    /* Patch every segment word the linker listed.  The table can be longer
     * than the staging area too, so it is walked window by window. */
    for (uint16_t i = 0; i < relocations; i++) {
        uint32_t entry = (uint32_t)reloc_table + (uint32_t)i * 4;
        uint16_t offset;
        uint16_t where;

        if (file_size && entry + 3 >= file_size)
            break; /* a table that runs past the file is not worth trusting */

        if (entry < staging_base || entry + 3 >= staging_base + staging_len) {
            if (!staging_window(name, entry, sizeof(staging)))
                break;
        }

        offset = staging_word(entry);
        where = (uint16_t)(load_seg + staging_word(entry + 2));

        mem_w16(mem, where, offset, (uint16_t)(mem_r16(mem, where, offset) + load_seg));
    }

    cpu->CS = (uint16_t)(load_seg + exe_cs);
    cpu->IP = exe_ip;
    cpu->SS = (uint16_t)(load_seg + exe_ss);
    cpu->SP = exe_sp;
    cpu->DS = psp_seg;
    cpu->ES = psp_seg;
    cpu->AX = 0x0000; /* both command tail drive letters were valid */
    cpu->BX = 0x0000;
    cpu->CX = 0x0000;
    cpu->DX = 0x0000;
    cpu->SI = 0x0000;
    cpu->DI = 0x0000;
    cpu->BP = 0x0000;
    cpu->FLAGS = 0x0202; /* IF set */

    /* Whatever the program did not claim is what DOS can hand out later. */
    {
        uint32_t top = (uint32_t)load_seg + ((image_size + 15) / 16) + min_alloc;

        if (top < 0x2000)
            top = 0x2000;
        if (top > 0x9000)
            top = 0x9000;

        mem->next_free_seg = (uint16_t)top;
    }

    return block_add(psp_seg, mem->next_free_seg - psp_seg, psp_seg) >= 0;
}

/*
 *  Lay a .COM out in the guest's memory: the image starts at com_org and
 *  owns the whole segment, with the stack at the top of it and a
 *  zero word pushed for the INT 20h that a program may return into.
 */
static uint8_t load_com(Memory_T *mem, CPU_T *cpu, uint16_t psp_seg, uint16_t com_org, const uint8_t *name, uint32_t file_size) {
    /* Keep the image within its segment and below the initial stack word. */
    if (com_org >= 0xFFFE || file_size > 0xFFFEu - com_org)
        return 0;

    {
        uint32_t base = ((uint32_t)psp_seg << 4) + com_org;
        uint32_t done = 0;

        while (done < file_size) {
            uint32_t want = file_size - done;
            uint32_t got;

            if (want > sizeof(staging))
                want = sizeof(staging);

            got = staging_window(name, done, want);
            if (!got)
                break;

            for (uint32_t i = 0; i < got && base + done + i < sizeof(mem->bytes); i++)
                mem->bytes[base + done + i] = staging[i];

            done += got;
        }

        if (!done)
            return 0;
    }

    cpu->CS = psp_seg;
    cpu->DS = psp_seg;
    cpu->ES = psp_seg;
    cpu->SS = psp_seg;
    cpu->SP = 0xFFFE;
    cpu->IP = com_org;
    cpu->AX = 0x0000;
    cpu->BX = 0x0000;
    cpu->CX = 0x00FF;
    cpu->DX = psp_seg;
    cpu->SI = com_org;
    cpu->DI = 0xFFFE;
    cpu->BP = 0x0000;
    cpu->FLAGS = 0x0202; /* IF set */

    mem_w16(mem, psp_seg, 0xFFFE, 0x0000);
    mem->next_free_seg = (uint16_t)(psp_seg + 0x1000);
    return block_add(psp_seg, 0x1000, psp_seg) >= 0;
}

/*
 *  Load <name> and leave the CPU ready to run it.
 *
 *  A name without an extension is resolved the way DOS resolves one: .COM
 *  first, then .EXE, then the name as given.
 */
uint8_t dos_load_program(Memory_T *mem, CPU_T *cpu, uint16_t psp_seg, uint16_t com_org, uint8_t *name) {
    static const uint8_t *suffix[2] = {(const uint8_t *)".COM", (const uint8_t *)".EXE"};
    uint32_t size = 0;
    uint8_t len = 0;
    uint8_t dotted = 0;

    /* Absolute before anything is read through it: the kernel resolves a
     * relative name against a working directory it does not wait for, and
     * against the root when it cannot get at it.  The caller's buffer holds
     * 64 bytes, and this is also the name the environment block ends up
     * carrying, so the program learns where it really lives. */
    dos_absolute(name, 64);

    /* Only the last component says whether there is an extension to add:
     * a directory on the way, or the harness's "./", has dots of its own. */
    while (name[len]) {
        if (name[len] == '/')
            dotted = 0;
        else if (name[len] == '.')
            dotted = 1;
        len++;
    }

    /*
     *  Only the front of the file is read here -- enough to tell an .EXE
     *  from a .COM and to hold an .EXE header -- and the loaders pull in the
     *  rest themselves.  Nothing reads the whole file, so how big it is no
     *  longer decides whether the program can start.
     */
    if (!dotted && len < 58) {
        for (uint8_t s = 0; s < 2 && !size; s++) {
            for (uint8_t i = 0; i < 4; i++)
                name[len + i] = suffix[s][i];
            name[len + 4] = 0;

            size = dos_file_size(name);
            if (size && !staging_window(name, 0, sizeof(staging)))
                size = 0;
        }

        if (!size)
            name[len] = 0; /* nothing matched; try the bare name */
    }

    if (!size) {
        size = dos_file_size(name);
        if (!staging_window(name, 0, sizeof(staging)))
            return 0;
    }

    if (!staging_len)
        return 0;

    if (staging_word(0) == EXE_SIGNATURE_MZ || staging_word(0) == EXE_SIGNATURE_ZM)
        return load_exe(mem, cpu, psp_seg, name, size);

    return load_com(mem, cpu, psp_seg, com_org, name, size ? size : staging_len);
}

/* The program segment prefix DOS builds in front of a child. */
static void dos_child_psp(Memory_T *mem, uint16_t seg, uint16_t env_seg, uint16_t parent_psp, const uint8_t *tail, uint8_t tail_len) {
    mem_w8(mem, seg, 0x0000, 0xCD); /* INT 20h */
    mem_w8(mem, seg, 0x0001, 0x20);
    mem_w16(mem, seg, 0x0002, 0xA000);
    mem_w8(mem, seg, 0x0005, 0xCD);
    mem_w8(mem, seg, 0x0006, 0x21);
    mem_w8(mem, seg, 0x0007, 0xCB);
    mem_w16(mem, seg, 0x0016, parent_psp);
    mem_w16(mem, seg, 0x002C, env_seg);
    mem_w8(mem, seg, 0x0050, 0xCD);
    mem_w8(mem, seg, 0x0051, 0x21);
    mem_w8(mem, seg, 0x0052, 0xCB);

    for (uint8_t i = 0; i < tail_len; i++)
        mem_w8(mem, seg, (uint16_t)(0x0081 + i), tail[i]);

    mem_w8(mem, seg, 0x0080, tail_len);
    mem_w8(mem, seg, (uint16_t)(0x0081 + tail_len), 0x0D);
}

/* 1 when <path> names the command interpreter, whatever directory it is in. */
static uint8_t is_command_com(const uint8_t *path) {
    const uint8_t *base = path;
    const uint8_t *want = (const uint8_t *)"COMMAND.COM";
    uint8_t i = 0;

    for (uint16_t k = 0; path[k]; k++)
        if (path[k] == '/' || path[k] == '\\')
            base = &path[k + 1];

    while (want[i] && base[i]) {
        uint8_t a = base[i];

        if (a >= 'a' && a <= 'z')
            a = (uint8_t)(a - 32);
        if (a != want[i])
            return 0;
        i++;
    }

    return (uint8_t)(!want[i] && !base[i]);
}

/*
 *  INT 21h AH=4Bh AL=00h -- load and run a program.
 *
 *  The parent stays where it is, the child is loaded above it, and the
 *  parent picks up again from dos_terminate() when the child ends.
 *
 *  A program that wants a command line rather than a program usually asks
 *  for the interpreter named in COMSPEC and passes "/C <command>". There
 *  is no interpreter here, so that one case is unwrapped: what the parent
 *  wanted run is in the tail, and running it directly is what the
 *  interpreter would have done.
 */
static uint8_t dos_exec(Memory_T *mem, CPU_T *cpu) {
    uint8_t name[64];
    uint8_t tail[128];
    uint16_t blk = cpu->BX;
    uint16_t blk_seg = cpu->ES;
    uint16_t env_seg = mem_r16(mem, blk_seg, blk);
    uint16_t toff = mem_r16(mem, blk_seg, (uint16_t)(blk + 2));
    uint16_t tseg = mem_r16(mem, blk_seg, (uint16_t)(blk + 4));
    uint8_t tail_len = mem_r8(mem, tseg, toff);
    uint16_t child;
    DosParent_T *slot;

    if (parent_depth >= DOS_NESTING)
        return 0;

    guest_string(mem, cpu->DS, cpu->DX, name, sizeof(name));

    if (tail_len > sizeof(tail) - 1)
        tail_len = sizeof(tail) - 1;
    for (uint8_t i = 0; i < tail_len; i++)
        tail[i] = mem_r8(mem, tseg, (uint16_t)(toff + 1 + i));
    tail[tail_len] = 0;

    if (is_command_com(name)) {
        uint8_t i = 0;
        uint8_t n = 0;

        while (tail[i] == ' ' || tail[i] == '\t')
            i++;

        /* Only "/C <command>" means anything without an interpreter. */
        if (tail[i] != '/' || (tail[i + 1] != 'C' && tail[i + 1] != 'c'))
            return 0;

        i += 2;
        while (tail[i] == ' ' || tail[i] == '\t')
            i++;

        /* The command's own name, then whatever is left is its tail. */
        while (tail[i] && tail[i] != ' ' && tail[i] != '\t' && n < sizeof(name) - 1) {
            uint8_t c = tail[i++];

            if (c == ':' && n == 1) { /* a drive letter names this drive */
                n = 0;
                continue;
            }
            name[n++] = (uint8_t)(c == '\\' ? '/' : c);
        }
        name[n] = 0;

        if (!name[0])
            return 0;

        n = 0;
        while (tail[i] && n < sizeof(tail) - 1)
            tail[n++] = tail[i++];
        tail[n] = 0;
        tail_len = n;
    }

    /*
     *  The child gets an environment of its own: the parent's strings, and
     *  behind them the child's own path. That path is how a program finds
     *  the directory it was started from, and a parameter block naming
     *  segment zero is asking to inherit the parent's rather than saying
     *  there is none.
     */
    {
        uint16_t src = env_seg ? env_seg : mem_r16(mem, psp_segment, 0x002C);
        uint16_t base = mem->next_free_seg;
        uint16_t off = 0;

        if (src) {
            uint8_t prev = 1;

            while (off < 0x1E00) {
                uint8_t c = mem_r8(mem, src, off);

                if (!c && !prev)
                    break; /* the second zero is the end of the block */

                mem_w8(mem, base, off++, c);
                prev = c;
            }
        }

        mem_w8(mem, base, off++, 0);     /* no more strings */
        mem_w16(mem, base, off, 0x0001); /* one of what follows */
        off += 2;

        mem_w8(mem, base, off++, 'C');
        mem_w8(mem, base, off++, ':');
        if (name[0] != '/')
            mem_w8(mem, base, off++, '\\');
        for (uint8_t i = 0; name[i]; i++)
            mem_w8(mem, base, off++, (uint8_t)(name[i] == '/' ? '\\' : name[i]));
        mem_w8(mem, base, off++, 0);

        env_seg = base;
        child = (uint16_t)(base + ((off + 15) / 16));
    }

    if (child < 0x1100 || child > 0x8000)
        return 0;

    slot = &parents[parent_depth];
    slot->cpu = *cpu;
    cpu_irq_save(&slot->irqs);
    slot->psp = psp_segment;
    slot->dta_seg = dta_seg;
    slot->dta_off = dta_off;
    for (uint8_t i = 0; i < DOS_HANDLES; i++)
        slot->files[i] = file_handles[i];

    dos_child_psp(mem, child, env_seg, psp_segment, tail, tail_len);

    if (!dos_load_program(mem, cpu, child, 0x0100, name)) {
        *cpu = slot->cpu; /* nothing ran; the parent never moved */
        return 0;
    }

    block_add(env_seg, child - env_seg, child);
    /* Loading may fail, so acquire inherited references only after success. */
    for (uint8_t i = 5; i < DOS_HANDLES; i++)
        if (file_handles[i] != DOS_NO_FILE)
            handles[file_handles[i]].refs++;
    parent_depth++;
    psp_segment = child;
    dta_seg = child;
    dta_off = 0x0080;

    return 1;
}

/*
 *  Hand the next matching entry to the guest through the disk transfer area.
 *
 *  The layout is the one DOS 2.0 settled on: its own search state first,
 *  which nothing here needs, then the attribute, the time and date, the size
 *  and the name. The listing carries no timestamps, so every file is dated
 *  the same; a program that sorts by date sees one heap, which is a good deal
 *  better than seeing no files at all.
 */
static uint8_t search_step(Memory_T *mem) {
    while (search.next < search.count && search.next < 64) {
        VfsDirEntry_T *e = &search.entries[search.next++];
        uint8_t attr = (uint8_t)(e->is_dir ? 0x10 : 0x20);
        uint16_t off = dta_off;
        uint8_t n = 0;

        if (!pattern_matches(e->name, e->name_len, search.pattern))
            continue;

        /* A directory only comes back when the caller asked for one. */
        if (e->is_dir && !(search.attr & 0x10))
            continue;

        mem_w8(mem, dta_seg, (uint16_t)(off + 0x15), attr);
        mem_w16(mem, dta_seg, (uint16_t)(off + 0x16), 0x6000); /* 12:00:00 */
        mem_w16(mem, dta_seg, (uint16_t)(off + 0x18), 0x2821); /* 1 Jan 2000 */
        mem_w16(mem, dta_seg, (uint16_t)(off + 0x1A), (uint16_t)(e->size & 0xFFFF));
        mem_w16(mem, dta_seg, (uint16_t)(off + 0x1C), (uint16_t)(e->size >> 16));

        while (n < e->name_len && n < 12)
            mem_w8(mem, dta_seg, (uint16_t)(off + 0x1E + n), e->name[n]), n++;
        mem_w8(mem, dta_seg, (uint16_t)(off + 0x1E + n), 0);

        return 1;
    }

    return 0;
}

static int16_t handle_alloc(void) {
    for (uint8_t i = 5; i < DOS_HANDLES; i++)
        if (file_handles[i] == DOS_NO_FILE)
            return (int16_t)i;

    return -1;
}

static int16_t file_alloc(void) {
    for (uint8_t i = 5; i < DOS_HANDLES; i++)
        if (!handles[i].refs)
            return (int16_t)i;
    return -1;
}

static uint8_t handle_valid(uint16_t h) { return (uint8_t)(h >= 5 && h < DOS_HANDLES && file_handles[h] != DOS_NO_FILE); }

static DosFile_T *handle_file(uint16_t h) { return &handles[file_handles[h]]; }

static void handle_flush(DosFile_T *f) {
    /* The kernel writes fixed 512-byte blocks, so a saved file is padded
     * out to that size. */
    uint8_t block[512];

    if (!f->modified || !f->data)
        return;

    for (uint16_t i = 0; i < 512; i++)
        block[i] = (i < f->size) ? f->data[i] : 0;

    write_file(f->name, block);
    f->modified = 0;
}

static void handle_close(uint16_t h) {
    if (!handle_valid(h))
        return;

    DosFile_T *f = handle_file(h);
    handle_flush(f);
    file_handles[h] = DOS_NO_FILE;
    if (--f->refs)
        return;

    if (f->data) {
        free(f->data);
        f->data = (uint8_t *)0;
    }
    f->buffered = 0;
}

void handle_20h(CPU_T *cpu, Memory_T *memory) { dos_terminate(memory, cpu, 0); }

/*
 *  Interrupt 21h DOS service dispatcher.
 */
void handle_21h(CPU_T *cpu, Memory_T *memory) {
    SRV_21H service = cpu->AX >> 8;
    uint8_t al = (uint8_t)(cpu->AX & 0xFF);

    if (debug)
        log_line((const uint8_t *)"INT 21 svc %x AL=%x", (unsigned int)service, (unsigned int)al);

    switch (service) {
    case PROGRAM_TERMINATE:
        dos_terminate(memory, cpu, 0);
        break;

    case CHAR_OUTPUT: {
        uint8_t pair[2] = {(uint8_t)(cpu->DX & 0xFF), 0};
        con_print(memory, (const uint8_t *)pair);
        break;
    }

    case DIRECT_CONS_IO: {
        uint8_t dl = (uint8_t)(cpu->DX & 0xFF);

        if (dl != 0xFF) {
            uint8_t pair[2] = {dl, 0};
            con_print(memory, (const uint8_t *)pair);
            break;
        }

        /* Non-blocking read: ZF set means no character was waiting. */
        {
            uint16_t entry;

            if (!bios_key_read(memory, &entry)) {
                cpu->AX = (uint16_t)(cpu->AX & 0xFF00);
                set_flag(cpu, ZF, 1);
                break;
            }

            cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | (entry & 0xFF));
            set_flag(cpu, ZF, 0);
        }
        break;
    }

    case CHAR_INPUT_W_ECHO:
    case UNFILTERED_CHAR_INPUT:
    case CHAR_INPUT: {
        uint16_t entry;

        /* Nothing typed yet: re-run the INT so the guest keeps polling
         * instead of the emulator blocking inside the service. */
        if (!bios_key_read(memory, &entry)) {
            bios_retry_instruction(cpu, memory);
            break;
        }

        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | (entry & 0xFF));

        if (service != UNFILTERED_CHAR_INPUT) {
            uint8_t pair[2] = {(uint8_t)(entry & 0xFF), 0};
            con_print(memory, (const uint8_t *)pair);
        }
        break;
    }

    case CHAR_STRING_OUTPUT: {
        uint8_t buf[256];
        uint16_t n = 0;

        while (n < sizeof(buf) - 1) {
            uint8_t c = mem_r8(memory, cpu->DS, (uint16_t)(cpu->DX + n));
            if (c == '$')
                break;
            buf[n++] = c;
        }

        buf[n] = 0;
        con_print(memory, (const uint8_t *)buf);
        break;
    }

    case GET_INPUT_STATUS:
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | (bios_key_ready(memory) ? 0xFF : 0x00));
        break;

    case RESET_INPUT_BUFFER_AND_INPUT:
        cpu->AX = (uint16_t)(cpu->AX & 0xFF00);
        break;

    case DISK_RESET:
        break;

    case SET_DEF_DISK_DRIVE:
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | 1);
        break;

    case GET_DEF_DISK_DRIVE:
        cpu->AX = (uint16_t)((cpu->AX & 0xFF00) | 2); /* C: */
        break;

    case SET_DISK_TRANSFER_AREA_ADDR:
        dta_seg = cpu->DS;
        dta_off = cpu->DX;
        break;

    case GET_CURRENT_DRIVE_ALLOC_DATA:
    case GET_ALLOC_DATA_FOR_SPEC_DRIVE:
        /* The guest sees one fixed DOS drive, also reported by AH=19h.
         * DS:BX must name a media descriptor, not the caller's old data. */
        mem_w8(memory, 0xF000, 0x2000, 0xF8);
        cpu->DS = 0xF000;
        cpu->BX = 0x2000;
        cpu->AX = (cpu->AX & 0xFF00) | 4;
        cpu->CX = 512;
        cpu->DX = 0x0800;
        break;

    case GET_DTA_ADDRESS:
        cpu->ES = dta_seg;
        cpu->BX = dta_off;
        break;

    case SET_INTERRUPT_VECTOR:
        mem_w16(memory, 0, (uint16_t)(al * 4), cpu->DX);
        mem_w16(memory, 0, (uint16_t)(al * 4 + 2), cpu->DS);
        break;

    case GET_INTERRUPT_VECTOR:
        cpu->BX = mem_r16(memory, 0, (uint16_t)(al * 4));
        cpu->ES = mem_r16(memory, 0, (uint16_t)(al * 4 + 2));
        break;

    case GET_SYSTEM_DATE: {
        RTC_T rtc = {0};

        read_rtc(&rtc);
        cpu->CX = rtc.year;
        cpu->DX = (uint16_t)((rtc.month << 8) | rtc.day);
        cpu->AX = (uint16_t)(cpu->AX & 0xFF00);
        break;
    }

    case GET_SYSTEM_TIME: {
        RTC_T rtc = {0};
        uint32_t ms = (uint32_t)(hw_millis() % 1000);

        read_rtc(&rtc);
        cpu->CX = (uint16_t)((rtc.hours << 8) | rtc.minutes);
        cpu->DX = (uint16_t)((rtc.seconds << 8) | (ms / 10));
        break;
    }

    case SET_SYSTEM_DATE:
    case SET_SYSTEM_TIME:
        cpu->AX = (uint16_t)(cpu->AX & 0xFF00);
        break;

    case SET_VERIFY_FLAG:
    case GET_VERIFY_FLAG:
        break;

    case GET_DOS_VERSION:
        cpu->AX = 0x0005; /* DOS 5.0 */
        cpu->BX = 0;
        cpu->CX = 0;
        break;

    case GET_SET_CTRL_BREAK_FLAG:
        cpu->DX = (uint16_t)((cpu->DX & 0xFF00) | 0);
        break;

    case GET_DISK_INFO:
    case GET_FREE_DISK_SPACE:
        cpu->AX = 4;      /* sectors per cluster */
        cpu->BX = 0x0400; /* available clusters */
        cpu->CX = 512;    /* bytes per sector */
        cpu->DX = 0x0800; /* clusters on the drive */
        break;

    case GET_ADDR_OF_PROG_SEGMENT_PREFIX:
    case GET_DISK_INFO_50H:
        cpu->BX = psp_segment;
        break;

    case CREATE_OR_TRUNCATE_FILE_3CH:
    case CREATE_NEW_FILE: {
        int16_t h = handle_alloc();
        int16_t object = file_alloc();

        if (h < 0 || object < 0) {
            dos_fail(cpu, 4);
            break;
        }

        DosFile_T *f = &handles[object];
        guest_string(memory, cpu->DS, cpu->DX, f->name, sizeof(f->name));

        /* Made on the disk now, as DOS makes it, so a file that can never
         * be saved is refused here -- on the CD or the archive, in a
         * directory that is not there, or in "/", which only leads to the
         * mounts. Its bytes go at close, which a guest seldom checks, and
         * would be lost there without a word. */
        static const uint8_t empty[512];
        if (!write_file(f->name, empty)) {
            log_line((const uint8_t *)"create %s FAILED (cannot be written)", f->name);
            dos_fail(cpu, 5); /* access denied */
            break;
        }

        if (!f->data)
            f->data = (uint8_t *)malloc(DOS_FILE_CAP);
        if (!f->data) {
            log_line((const uint8_t *)"create %s: malloc %u FAILED", f->name, (unsigned int)DOS_FILE_CAP);
            dos_fail(cpu, 4);
            break;
        }

        log_line((const uint8_t *)"create %s -> h=%u", f->name, (unsigned int)h);

        f->refs = 1;
        f->size = 0;
        f->pos = 0;
        f->modified = 1;
        f->buffered = 1; /* a new file is empty and already in hand */
        file_handles[h] = (uint8_t)object;

        cpu->AX = (uint16_t)h;
        dos_ok(cpu);
        break;
    }

    case OPEN_FILE_3DH: {
        int16_t h = handle_alloc();
        int16_t object = file_alloc();
        uint32_t size;

        if (h < 0 || object < 0) {
            log_line((const uint8_t *)"open FAILED: all %u handles in use", (unsigned int)DOS_HANDLES);
            dos_fail(cpu, 4);
            break;
        }

        DosFile_T *f = &handles[object];
        guest_string(memory, cpu->DS, cpu->DX, f->name, sizeof(f->name));

        size = dos_file_size(f->name);
        if (!size) {
            log_line((const uint8_t *)"open %s FAILED (not found)", f->name);
            dos_fail(cpu, 2); /* file not found */
            break;
        }

        log_line((const uint8_t *)"open %s size=%u -> h=%u", f->name, (unsigned int)size, (unsigned int)h);

        /*
         *  Nothing is read here.  A read is served out of the file when the
         *  guest asks for it, so a file far larger than anything the
         *  emulator could hold -- a program reading its own overlays out of
         *  the end of itself, say -- can still be opened and worked through.
         */
        f->refs = 1;
        f->size = size;
        f->pos = 0;
        f->modified = 0;
        f->buffered = 0;
        file_handles[h] = (uint8_t)object;

        cpu->AX = (uint16_t)h;
        dos_ok(cpu);
        break;
    }

    case CLOSE_FILE_3EH: {
        if (!handle_valid(cpu->BX)) {
            dos_fail(cpu, 6);
            break;
        }

        log_line((const uint8_t *)"close h=%u %s", (unsigned int)cpu->BX, handle_file(cpu->BX)->name);
        handle_close(cpu->BX);

        dos_ok(cpu);
        break;
    }

    case READ_FILE_OR_DEVICE: {
        DosFile_T *f;
        uint16_t want = cpu->CX;
        uint16_t got = 0;

        if (cpu->BX == 0) { /* stdin */
            cpu->AX = 0;
            dos_ok(cpu);
            break;
        }
        if (!handle_valid(cpu->BX)) {
            dos_fail(cpu, 6);
            break;
        }

        f = handle_file(cpu->BX);

        if (f->buffered) {
            while (got < want && f->pos < f->size)
                mem_w8(memory, cpu->DS, (uint16_t)(cpu->DX + got++), f->data[f->pos++]);
        } else {
            /* Straight out of the file, a window at a time. */
            while (got < want && f->pos < f->size) {
                uint32_t chunk = (uint32_t)(want - got);
                uint32_t n;

                if (chunk > sizeof(staging))
                    chunk = sizeof(staging);

                n = staging_window(f->name, f->pos, chunk);
                if (!n) {
                    log_line((const uint8_t *)"read %s at %u want %u -> NOTHING", f->name, (unsigned int)f->pos, (unsigned int)chunk);
                    break;
                }
                /* Short at the end of the file is just the end of the file;
                 * short before it is the filesystem letting the guest down,
                 * which is what a game that starts looking wrong looks like
                 * from here. */
                if (n < chunk && f->pos + n < f->size)
                    log_line((const uint8_t *)"read %s at %u want %u -> short %u", f->name, (unsigned int)f->pos, (unsigned int)chunk, (unsigned int)n);

                for (uint32_t i = 0; i < n && got < want; i++)
                    mem_w8(memory, cpu->DS, (uint16_t)(cpu->DX + got++), staging[i]);

                f->pos += n;
            }
        }

        cpu->AX = got;
        dos_ok(cpu);
        break;
    }

    case WRITE_FILE_OR_DEVICE: {
        DosFile_T *f;
        uint16_t want = cpu->CX;
        uint16_t done = 0;

        if (cpu->BX <= 2) { /* stdout / stderr */
            uint8_t pair[2] = {0, 0};

            for (uint16_t i = 0; i < want; i++) {
                pair[0] = mem_r8(memory, cpu->DS, (uint16_t)(cpu->DX + i));
                con_print(memory, (const uint8_t *)pair);
            }

            cpu->AX = want;
            dos_ok(cpu);
            break;
        }
        if (!handle_valid(cpu->BX)) {
            dos_fail(cpu, 6);
            break;
        }

        f = handle_file(cpu->BX);

        /* Writing needs the file in hand, so it is read in the first time
         * the guest writes to one it opened. */
        if (!f->buffered) {
            if (!f->data)
                f->data = (uint8_t *)malloc(DOS_FILE_CAP);
            if (!f->data || f->size > DOS_FILE_CAP) {
                log_line((const uint8_t *)"write %s: buffer %u FAILED (size %u)", f->name, (unsigned int)DOS_FILE_CAP, (unsigned int)f->size);
                dos_fail(cpu, 5); /* access denied: too big to hold */
                break;
            }

            dos_read_file(f->name, f->data, DOS_FILE_CAP, 0);
            f->buffered = 1;
        }

        while (done < want && f->pos < DOS_FILE_CAP) {
            f->data[f->pos++] = mem_r8(memory, cpu->DS, (uint16_t)(cpu->DX + done++));
            if (f->pos > f->size)
                f->size = f->pos;
        }

        f->modified = 1;
        cpu->AX = done;
        dos_ok(cpu);
        break;
    }

    case MOVE_FILE_POINTER: {
        DosFile_T *f;
        int32_t offset = (int32_t)(((uint32_t)cpu->CX << 16) | cpu->DX);
        int32_t base;

        if (!handle_valid(cpu->BX)) {
            dos_fail(cpu, 6);
            break;
        }

        f = handle_file(cpu->BX);
        base = (al == 1) ? (int32_t)f->pos : ((al == 2) ? (int32_t)f->size : 0);

        if (base + offset < 0)
            f->pos = 0;
        else
            f->pos = (uint32_t)(base + offset);

        cpu->AX = (uint16_t)(f->pos & 0xFFFF);
        cpu->DX = (uint16_t)(f->pos >> 16);
        dos_ok(cpu);
        break;
    }

    case DELETE_FILE_41H: {
        uint8_t name[64];

        guest_string(memory, cpu->DS, cpu->DX, name, sizeof(name));
        if (delete_file(name))
            dos_ok(cpu);
        else
            dos_fail(cpu, 2);
        break;
    }

    case GET_OR_SET_FILE_ATTRS:
        if (al == 0) {
            uint8_t name[64];

            guest_string(memory, cpu->DS, cpu->DX, name, sizeof(name));
            if (dos_file_size(name)) {
                cpu->CX = 0x20; /* archive */
                dos_ok(cpu);
            } else {
                dos_fail(cpu, 2);
            }
        } else {
            dos_ok(cpu);
        }
        break;

    case DEVICE_DRIVER_CONTROL: /* IOCTL */
        if (al == 0x00) {
            /* Handles 0-2 are character devices, everything else is a file. */
            cpu->DX = (cpu->BX <= 2) ? 0x80D3 : 0x0002;
            dos_ok(cpu);
        } else {
            cpu->AX = 0;
            dos_ok(cpu);
        }
        break;

    case DUPLICATE_HANDLE:
    case FORCE_DUPLICATE_HANDLE:
        cpu->AX = cpu->BX;
        dos_ok(cpu);
        break;

    case GET_CURRENT_DIRECTORY: {
        /* The directory relative names are read in, the DOS way: no drive
         * and no leading backslash ("mnt\fat\THEM"). "C:\" and it make the
         * r2 path again, since the drive's root is r2's root; answering ""
         * would send a guest that builds its names from it into "/", which
         * holds no files. */
        uint8_t cwd[40];
        uint16_t n = 0;

        dos_cwd(cwd, sizeof(cwd));
        if (cwd[0] == '/')
            for (uint16_t i = 1; cwd[i] && n < 63; i++)
                mem_w8(memory, cpu->DS, (uint16_t)(cpu->SI + n++), (uint8_t)(cwd[i] == '/' ? '\\' : cwd[i]));
        mem_w8(memory, cpu->DS, (uint16_t)(cpu->SI + n), 0);
        dos_ok(cpu);
        break;
    }

    case ALLOC_MEMORY: {
        uint16_t want = cpu->BX;
        uint16_t largest = 0;
        uint16_t chosen = 0;
        /* Every free range begins at the arena start or a live block's end. */
        for (uint16_t i = 0; i <= DOS_BLOCKS; i++) {
            uint16_t base;
            if (i == DOS_BLOCKS)
                base = arena_start;
            else if (blocks[i].owner)
                base = blocks[i].seg + blocks[i].size;
            else
                continue;
            uint16_t room = block_room(base, -1);
            if (room > largest)
                largest = room;
            if (room >= want && room && (!chosen || base < chosen))
                chosen = base;
        }
        if (!chosen || block_add(chosen, want ? want : 1, psp_segment) < 0) {
            cpu->BX = largest;
            dos_fail(cpu, 8);
            break;
        }
        block_cursor(memory);
        cpu->AX = chosen;
        dos_ok(cpu);
        break;
    }

    case RELEASE_MEMORY: {
        int16_t i = block_find(cpu->ES);
        if (i < 0) {
            dos_fail(cpu, 9);
            break;
        }
        blocks[i].owner = 0;
        block_cursor(memory);
        dos_ok(cpu);
        break;
    }

    case RESERVED_4AH: { /* 4Ah: resize a memory block */
        int16_t i = block_find(cpu->ES);
        if (i < 0) {
            dos_fail(cpu, 9);
            break;
        }
        uint16_t room = block_room(cpu->ES, i);
        if (cpu->BX > room) {
            cpu->BX = room;
            dos_fail(cpu, 8);
            break;
        }
        blocks[i].size = cpu->BX;
        block_cursor(memory);
        log_line((const uint8_t *)"resize es=%x size=%x next=%x", (unsigned int)cpu->ES, (unsigned int)cpu->BX, (unsigned int)memory->next_free_seg);
        dos_ok(cpu);
        break;
    }

    case TERMINATE_PROGRAM_WITH_RET_CODE:
        dos_terminate(memory, cpu, al);
        break;

    case EXECUTE_PROGRAM:
        /* AL=00h loads and runs; AL=03h loads an overlay, which needs no
         * process of its own and is not handled here. */
        if (al == 0x00 && dos_exec(memory, cpu))
            return; /* the child is running now; do not touch its registers */

        dos_fail(cpu, 2);
        break;

    case GET_RET_CODE:
        /* The code the last child ended with, and how it ended. */
        cpu->AX = child_code;
        dos_ok(cpu);
        break;

    case GET_COUNTRY_INFORMATION:
        dos_ok(cpu);
        break;

    case SET_CURRENT_DIRECTORY:
    case CREATE_SUBDIRECTORY:
    case DELETE_SUBDIRECTORY:
        dos_ok(cpu);
        break;

    case GET_EXTENDED_ERROR_INFORMATION:
        cpu->AX = 0;
        cpu->BX = 0;
        cpu->CX = 0;
        break;

    case SEARCH_FOR_FIRST_MATCH: {
        uint8_t path[80];
        uint8_t dir[64];
        const uint8_t *mask;

        guest_string(memory, cpu->DS, cpu->DX, path, sizeof(path));
        mask = split_dir(path, dir, sizeof(dir));

        search.count = list_dir_retry(dir, search.entries);
        search.next = 0;
        search.attr = (uint8_t)(cpu->CX & 0xFF);
        pattern83(mask, search.pattern);

        if (!search_step(memory)) {
            dos_fail(cpu, 18); /* no more files */
            break;
        }

        dos_ok(cpu);
        break;
    }

    case SEARCH_FOR_NEXT_MATCH:
        if (!search_step(memory)) {
            dos_fail(cpu, 18);
            break;
        }

        dos_ok(cpu);
        break;

    default:
        if (debug)
            log_line((const uint8_t *)"INT 21 svc %x NOT IMPLEMENTED", (unsigned int)service);
        dos_fail(cpu, 1);
        break;
    }
}
