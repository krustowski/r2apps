#ifndef _R2CXX_FS_HPP_
#define _R2CXX_FS_HPP_

/*
 *  fs.hpp — files and directories.
 *
 *  Two things about the r2 filesystem ABI are worth knowing before using this:
 *
 *  Reading.  Syscall 0x20 reads a whole file and is never told how big the
 *  buffer is, so a file larger than the caller expected overruns it.  Nothing
 *  here calls it.  read() and read_text() work through syscall 0x39
 *  (read_file_at), which takes a length, one chunk at a time.
 *
 *  Writing.  Syscall 0x21 reads exactly 512 bytes from the pointer it is
 *  given, whatever the caller meant to write, and the file it produces is
 *  always one 512-byte sector.  write() therefore stages through a zeroed
 *  512-byte buffer and refuses anything longer, rather than letting the kernel
 *  read off the end of a shorter one.
 */

#include "optional.hpp"
#include "span.hpp"
#include "string.hpp"
#include "syscall.hpp"
#include "vector.hpp"

namespace r2::fs {

/*  The largest payload syscall 0x21 can store in one file.  */
inline constexpr size_t MAX_WRITE_BYTES = 512;

enum class FsType : uint8_t {
    None = 0,
    RootFs = 1,
    Fat12 = 2,
    Iso9660 = 3,
    Tar = 4,
    MemDisk = 5, /*  /mnt/tmp: the RAM disk, FAT16 (FAT12 when only 2 MiB)  */
};

/*  What a filesystem is on its medium, whatever it is mounted as.  */
enum class FsFormat : uint8_t {
    None = 0, /*  the root: no filesystem, the way to the mounts  */
    Fat12 = 1,
    Fat16 = 2,
    Iso9660 = 3,
    Tar = 4,
};

/*  How big the filesystem a path is on is, and how much of it is free.  */
struct Usage {
    uint64_t total;   /*  the whole volume, in bytes; 0 for the root  */
    uint64_t free;    /*  what files can still take; 0 when read-only  */
    FsType type;      /*  the mount's  */
    FsFormat format;  /*  the medium's  */
};

struct Entry {
    string name;
    bool is_dir;
    uint32_t size;
};

struct Mount {
    string path;
    FsType type;
};

/*
 *  Reads the whole file.  Returns nullopt when the file does not exist or the
 *  arena cannot hold it; stops at max_bytes so a huge file on the CD cannot
 *  exhaust the heap by surprise.
 */
optional<vector<uint8_t>> read(string_view path, size_t max_bytes = 256 * 1024);

/*  The same, as text.  */
optional<string> read_text(string_view path, size_t max_bytes = 256 * 1024);

/*
 *  Reads at most buffer.size() bytes starting at `offset`.  Returns the number
 *  of bytes read --- short at the end of the file, 0 past it --- or -1 on
 *  error.  This is the way to walk a file that does not fit in memory.
 */
int64_t read_at(string_view path, byte_span buffer, uint64_t offset);

/*  Size of a file in bytes, found by looking it up in its directory.  */
optional<uint32_t> size_of(string_view path);

inline bool exists(string_view path) { return size_of(path).has_value(); }

/*
 *  Writes up to 512 bytes and returns false if given more.  The stored file is
 *  always 512 bytes, zero-padded --- that is what the ABI does.
 */
bool write(string_view path, const_byte_span data);
bool write_text(string_view path, string_view text);

/*
 *  Writes `data` at `offset` into the file, creating it (on FAT12 only; the
 *  CD is read-only) and growing it as far as it takes (syscall 0x3a).  What
 *  was there past the end of `data` is left: to replace a file, remove it
 *  first.  Returns the bytes written --- short when the disk filled --- or -1.
 */
int64_t write_at(string_view path, const_byte_span data, uint64_t offset);

/*  Deletes a file.  Like the kernel, only resolves a bare name against the
 *  working directory, or a path at the top of the floppy: change_dir() to the
 *  file's own directory first.  */
bool remove(string_view path);
/*  Deletes a directory, which has to be empty (syscall 0x23 with 1).  */
bool remove_dir(string_view path);
bool rename(string_view from, string_view to);

/*  Creates `name` inside the directory `parent` (an absolute VFS path).  */
bool make_dir(string_view parent, string_view name);

/*  Changes the kernel's working directory (syscall 0x2e).  */
bool change_dir(string_view path);

/*  Lists a directory by absolute path; works on FAT12 and ISO9660 alike.  */
vector<Entry> list(string_view path);

/*  The mount table (syscall 0x2c).  */
vector<Mount> mounts();

/*
 *  The size of the filesystem `path` is on (syscall 0x40).  nullopt when no
 *  mount holds the path, its medium cannot be read, or the kernel is too old
 *  to say.
 */
optional<Usage> usage(string_view path);

/*  Runs the FAT12 consistency check (syscall 0x2b).  */
optional<FsckReport> check();

/*
 *  Reads a file in fixed-size pieces:
 *
 *      FileReader r("/BIG.DAT");
 *      uint8_t chunk[512];
 *      while (int64_t n = r.next(chunk, sizeof chunk)) { ... }
 */
class FileReader {
  public:
    explicit FileReader(string_view path) : path_(path), offset_(0) {}

    /*  Fills up to `len` bytes; returns how many, 0 at the end, -1 on error. */
    int64_t next(uint8_t *buffer, size_t len);

    uint64_t offset() const noexcept { return offset_; }
    void seek(uint64_t offset) noexcept { offset_ = offset; }

  private:
    string path_;
    uint64_t offset_;
};

} // namespace r2::fs

#endif
