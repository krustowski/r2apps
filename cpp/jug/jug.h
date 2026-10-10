//
//  jug --- programs for r2, fresh from a server.
//
//  The server publishes a list (sums.txt): one line per program with its
//  SHA-256, its size and where it lies below the list.  jug compares each
//  line with the copy of that program the kernel would start now --- the one
//  in /mnt/tmp/jug when jug put one there, else the one shipped on the boot
//  medium in /mnt/tar/bin or /mnt/iso/bin --- and downloads the programs whose
//  sums differ into /mnt/tmp/jug, which fg and bg search before the shipped
//  ones (src/input/elf.rs in the kernel).  A registry there remembers the sum
//  of each copy, so a binary is read and hashed once rather than on every look.
//
//  This header is the part the command line (cli.cpp) and the Memento window
//  (window.cpp) share.  sha256.cpp and lists.cpp make no syscalls and are
//  tested on the host (tests/); store.cpp is the side that touches the disks
//  and the task table; fetch.h is the network.
//
#pragma once

#include <r2/string.hpp>
#include <r2/string_view.hpp>
#include <r2/types.hpp>
#include <r2/vector.hpp>

namespace jug {

//  Where the downloaded programs go: the RAM disk, formatted at every boot,
//  so after a restart the shipped programs are what runs again.
extern const char *const DIR;
//  The registry, and the list as the last update fetched it.
extern const char *const REGISTRY_PATH;
extern const char *const LIST_PATH;

const size_t NAME_CAP = 9; // an 8.3 base and its NUL: what fg and bg take
const size_t PATH_CAP = 64;
const size_t URL_CAP = 192;
const size_t STAMP_CAP = 48;

//  "Tnt.ELF", "bin/tnt.elf" -> "tnt".  False for anything fg could not start:
//  not an .elf, an empty or longer than eight base, a character a FAT name
//  cannot have.
bool program_name(r2::string_view file, char (&name)[NAME_CAP]);
//  A bare name as the user types it: "tnt".
bool valid_name(r2::string_view name);

// ─── SHA-256 ─────────────────────────────────────────────────────────────────

struct Digest
{
    uint8_t b[32] = {};

    bool operator==(const Digest &o) const;
    bool operator!=(const Digest &o) const { return !(*this == o); }
    //  The first `digits` (at most 64) lowercase hex digits, and a NUL.
    void hex(char *out, size_t digits = 64) const;
    static bool parse(r2::string_view hex, Digest &out);
};

class Sha256
{
public:
    Sha256();
    void update(const void *data, size_t n);
    Digest finish();

    static Digest of(const void *data, size_t n);

private:
    uint32_t h_[8];
    uint8_t block_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;

    void compress(const uint8_t *p);
};

// ─── The server's list ───────────────────────────────────────────────────────

struct Package
{
    char name[NAME_CAP] = {};
    char path[PATH_CAP] = {}; // below the list's own directory: "tnt.elf", "bin/tnt.elf"
    Digest sum;
    int64_t size = -1;        // -1 when the line does not say
};

//
//  The list, line by line, in either of two forms:
//
//      # updated 2026-10-08 12:34:56 UTC
//      <sha256>  <size>  <path>          what mksums.py writes
//      <sha256>  <path>                  what sha256sum writes
//
//  '#' starts a comment; "# updated" says when the list was made.  A line
//  that names no program --- not an .elf, a path that climbs out with "..",
//  a name fg could not take --- is counted in `skipped` and left out, and so
//  is a second line for a program already listed.
//
struct Catalog
{
    r2::vector<Package> packages; // sorted by name
    char updated[STAMP_CAP] = {};
    int skipped = 0;

    //  False when nothing in `text` is a package line: an error page, say.
    bool parse(r2::string_view text);
    const Package *find(r2::string_view name) const;
    //  The same programs, paths, sums and sizes, and the same "updated".
    bool same(const Catalog &o) const;
    //  The programs this list has a build of that `older` did not: new
    //  programs, and ones whose sum changed.
    r2::vector<r2::string> freshSince(const Catalog &older) const;
};

// ─── The registry ────────────────────────────────────────────────────────────

//  What jug knows of one copy of a program: the sum of the file at `path`,
//  which was `size` bytes when it was hashed.  The shipped binaries do not
//  change while the system runs, and jug writes the ones in DIR itself, so a
//  record whose path and size still match still holds.
struct Record
{
    char name[NAME_CAP] = {};
    Digest sum;
    uint32_t size = 0;
    char path[PATH_CAP] = {};
};

//  A text file, one program a line: "tnt <sha256> 78968 /mnt/tar/bin/tnt.elf".
struct Registry
{
    r2::vector<Record> records;

    bool parse(r2::string_view text);
    r2::string serialize() const;
    Record *find(r2::string_view name);
    bool put(const Record &r);
    bool erase(r2::string_view name);

    bool load();       // store.cpp
    bool save() const; // store.cpp
};

// ─── ELF ─────────────────────────────────────────────────────────────────────

//  Why the kernel could not run `image`, or nullptr when it could: an x86-64
//  ELF executable whose loadable segments all lie in the process window
//  (0x600000-0xA00000) and in the file.  The kernel's loader asserts on the
//  magic, so an error page saved where a program should be would take the
//  whole machine down at the next fg.
const char *elf_problem(const uint8_t *image, size_t len);

// ─── Text ────────────────────────────────────────────────────────────────────

void scopy(char *dst, r2::string_view src, size_t cap);
void scat(char *dst, r2::string_view src, size_t cap);
void scatU(char *dst, uint64_t v, size_t cap);
//  "512 B", "77 KiB", "1.4 MiB"
void scatSize(char *dst, uint64_t bytes, size_t cap);
bool ieq(r2::string_view a, r2::string_view b);

// ─── This machine (store.cpp) ────────────────────────────────────────────────

//  Where the server is.  The defaults, then jug.cfg:
//
//      repo     = https://cdn.vxn.dev/jug
//      list     = sums.txt            (a name below repo, or a whole URL)
//      insecure = 0                   (1: take any TLS certificate)
//      check    = 600                 (the window's list checks, in seconds; 0: none)
//
struct Config
{
    static const uint32_t CHECK_DEFAULT = 600, CHECK_MIN = 30;

    char repo[URL_CAP] = {};
    char list[URL_CAP] = {};    // the list's URL, made whole by load()
    bool insecure = false;
    uint32_t check = CHECK_DEFAULT; // seconds between the window's list checks; 0: never
    char source[PATH_CAP] = {}; // the file read, or "" for the defaults

    //  `path` when given, else the first of /mnt/tmp/jug/jug.cfg,
    //  /mnt/fat/JUG.CFG and the boot medium's opt/jug/jug.cfg.  False only
    //  when `path` cannot be read.
    bool load(const char *path = nullptr);
    //  --repo on the command line: the list moves with it.
    void setRepo(r2::string_view url);
    //  A package's path, made a URL: below the directory the list is in.
    void urlOf(const char *rel, char *out, size_t cap) const;

private:
    char listName_[URL_CAP] = {};
    void finish();
};

bool mounted(const char *mount);

enum class Origin : uint8_t
{
    None,
    Jug, // /mnt/tmp/jug: downloaded
    Tar, // /mnt/tar/bin: the boot medium's archive
    Iso, // /mnt/iso/bin: the CD
};
const char *origin_name(Origin o);

//  The copy of a program the kernel would start --- the working directory
//  aside --- and where it is.
struct Local
{
    char name[NAME_CAP] = {};
    Origin origin = Origin::None;
    char path[PATH_CAP] = {};
    uint32_t size = 0;
};

//  Every program in DIR, /mnt/tar/bin and /mnt/iso/bin, once each: the copy
//  the kernel's search finds first.
r2::vector<Local> scan_local();

//  "/mnt/tmp/jug/TNT.ELF"
void jug_path(const char *name, char *out, size_t cap);

//  What the downloads are now: the programs in DIR, their sizes and their
//  sums in the registry.  Another stamp means another jug (the console's
//  `jug upgrade`, say) downloaded, removed or replaced one.  Reads the RAM
//  disk only, so it is cheap enough to ask every few seconds.
Digest downloads_stamp();

//  The SHA-256 of a file, read in pieces; false unless exactly `size` bytes
//  could be read.
bool hash_file(const char *path, uint32_t size, Digest &out);
//  The sum of `local`: from the registry when it still holds, else hashed
//  and put there (the caller saves the registry). Writable downloads are
//  rehashed when inspected, including same-size edits by other programs.
bool local_digest(const Local &local, Registry &reg, Digest &out);

//  Puts a downloaded program in place: checks it against its line in the
//  list and as an ELF, writes and verifies a .NEW file, then renames it into
//  place while keeping the old copy for rollback. nullptr, or why not.
const char *install(const Package &pkg, const uint8_t *data, size_t len, Registry &reg);

//  Deletes the downloaded copy, so the shipped one runs again.
const char *uninstall(r2::string_view name, Registry &reg);

//  The list as fetched goes to LIST_PATH, with `stamp` (the server's
//  Last-Modified) as its "# updated" line when it has none of its own.
bool save_list(const uint8_t *data, size_t len, const char *stamp, const char *source);
bool load_list(Catalog &out, const char *source);

//  The user tasks running `name`, at most `cap` of them.
int running(r2::string_view name, uint8_t *ids, int cap);

//  Stops each running `name` and starts it again with the command line it was
//  started with (syscall 0x41) --- which now finds the copy in DIR first.
//  jug itself is never stopped, nor Memento when `hosted`.  Says what
//  happened in `msg`; the number started again, or -1.
int restart(r2::string_view name, bool hosted, char *msg, size_t cap);

// ─── The two put together ────────────────────────────────────────────────────

enum class State : uint8_t
{
    Unknown,   // the local copy is not hashed yet
    Current,   // what runs is what the server has
    Outdated,  // the server has another build
    Available, // on the server, not here
    Gone,      // downloaded, and no longer on the server
};
const char *state_name(State s);

//  One program: a line of the list, the copy here, or both.
struct Row
{
    char name[NAME_CAP] = {};
    int pkg = -1;   // index into catalog.packages; -1: not on the server
    int local = -1; // index into locals; -1: not here
    bool hashed = false;
    bool unreadable = false; // the local copy could not be read whole
    Digest sum;     // of the local copy, once hashed
    State state = State::Unknown;
    uint8_t pids[10] = {};
    int npids = 0;  // running copies
};

struct Model
{
    Config config;
    Catalog catalog;
    Registry registry;
    r2::vector<Local> locals;
    r2::vector<Row> rows; // the list's programs by name, then dropped downloads
    bool haveList = false;

    //  The configuration, the registry, the list the last update kept, and
    //  what is on the disks.  False when the configuration cannot be read.
    bool open(const char *configPath = nullptr);
    //  A list just fetched: kept for the next look, and taken in.  False
    //  when it is not a list.  With `changed`, a list the same as the one
    //  held is left at that (*changed false): the rows keep their sums.
    bool takeList(const uint8_t *data, size_t len, const char *lastModified, bool *changed = nullptr);
    //  Looks at the disks again and makes the rows anew.
    void rescan();
    //  The same after another jug changed the downloads: its registry too.
    void reload();
    //  Hashes the next local copy that has no sum yet; false once none is
    //  left (and the registry is saved, when it learnt anything).
    bool hashNext();
    void hashAll();
    void readRunning();

    Row *row(r2::string_view name);
    const Package *package(const Row &r) const { return r.pkg < 0 ? nullptr : &catalog.packages[r.pkg]; }
    const Local *local(const Row &r) const { return r.local < 0 ? nullptr : &locals[r.local]; }
    int count(State s) const;

private:
    bool registryDirty_ = false;
    void rebuild();
    void judge(Row &r);
};

} // namespace jug
