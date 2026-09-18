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

bool remove(string_view path);
bool rename(string_view from, string_view to);

/*  Creates `name` inside the directory `parent` (an absolute VFS path).  */
bool make_dir(string_view parent, string_view name);

/*  Changes the kernel's working directory (syscall 0x2e).  */
bool change_dir(string_view path);

/*  Lists a directory by absolute path; works on FAT12 and ISO9660 alike.  */
vector<Entry> list(string_view path);

/*  The mount table (syscall 0x2c).  */
vector<Mount> mounts();

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
