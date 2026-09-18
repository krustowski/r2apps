/*
 *  fs.cpp — files and directories.
 *
 *  Paths crossing the ABI have to be NUL-terminated and inside the range the
 *  kernel accepts, so every entry point here copies the caller's string_view
 *  into a stack buffer first.  The buffers are small on purpose: the kernel
 *  itself only looks at the first 64 bytes of a path.
 */

#include "r2/fs.hpp"
#include "r2/libc.hpp"

namespace r2::fs {

namespace {

constexpr size_t PATH_MAX = 64;

/*  Copies a path into a NUL-terminated buffer; false if it does not fit.  */
bool to_path(string_view text, char (&buffer)[PATH_MAX]) {
    if (text.empty() || text.size() >= PATH_MAX)
        return false;
    memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    return true;
}

char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

bool same_name(string_view a, string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (upper(a[i]) != upper(b[i]))
            return false;
    return true;
}

} // namespace

int64_t read_at(string_view path, byte_span buffer, uint64_t offset) {
    char name[PATH_MAX];
    if (!to_path(path, name) || buffer.empty())
        return -1;

    ReadRange range;
    range.buffer = (uint64_t)buffer.data();
    range.offset = offset;
    range.length = buffer.size();

    return raw_syscall(Sys::ReadFileAt, (int64_t)name, (int64_t)&range);
}

optional<vector<uint8_t>> read(string_view path, size_t max_bytes) {
    constexpr size_t CHUNK = 512;

    vector<uint8_t> data;
    uint64_t offset = 0;

    while (offset < max_bytes) {
        size_t want = max_bytes - (size_t)offset;
        if (want > CHUNK)
            want = CHUNK;

        /*  Grow the capacity geometrically.  resize() alone would reserve the
         *  exact length every iteration, which for a file read 512 bytes at a
         *  time means reallocating and copying the whole buffer on every
         *  chunk.  */
        size_t needed = (size_t)offset + want;
        if (data.capacity() < needed) {
            size_t target = data.capacity() ? data.capacity() * 2 : 4096;
            if (target < needed)
                target = needed;
            if (!data.reserve(target))
                return nullopt; /*  the arena could not hold the file  */
        }

        if (!data.resize(needed))
            return nullopt;

        int64_t got = read_at(path, byte_span(data.data() + offset, want), offset);
        if (got < 0)
            return nullopt;

        offset += (uint64_t)got;

        if ((size_t)got < want) {
            (void)data.resize((size_t)offset);
            return data;
        }
    }

    return data;
}

optional<string> read_text(string_view path, size_t max_bytes) {
    auto bytes = read(path, max_bytes);
    if (!bytes)
        return nullopt;

    string text;
    if (!text.reserve(bytes->size()))
        return nullopt;

    /*  Files written through syscall 0x21 are padded to 512 bytes with zeros;
     *  stop at the first one rather than handing back the padding.  */
    size_t length = 0;
    while (length < bytes->size() && (*bytes)[length] != 0)
        length++;

    text.append(string_view((const char *)bytes->data(), length));
    return text;
}

bool write(string_view path, const_byte_span data) {
    char name[PATH_MAX];
    if (!to_path(path, name))
        return false;

    if (data.size() > MAX_WRITE_BYTES)
        return false;

    /*
     *  The kernel reads exactly 512 bytes from this pointer whatever the
     *  caller meant to write, so the staging buffer is not an optimisation:
     *  handing it a shorter buffer is an out-of-bounds read.
     */
    uint8_t sector[MAX_WRITE_BYTES];
    memset(sector, 0, sizeof(sector));
    if (!data.empty())
        memcpy(sector, data.data(), data.size());

    return raw_syscall(Sys::WriteFile, (int64_t)name, (int64_t)sector) == 0;
}

bool write_text(string_view path, string_view text) {
    return write(path, const_byte_span((const uint8_t *)text.data(), text.size()));
}

bool remove(string_view path) {
    char name[PATH_MAX];
    if (!to_path(path, name))
        return false;
    return raw_syscall(Sys::DeleteFile, (int64_t)name, 0) == 0;
}

bool rename(string_view from, string_view to) {
    char old_name[PATH_MAX];
    char new_name[PATH_MAX];
    if (!to_path(from, old_name) || !to_path(to, new_name))
        return false;
    return raw_syscall(Sys::RenameFile, (int64_t)old_name, (int64_t)new_name) == 0;
}

bool make_dir(string_view parent, string_view name) {
    char parent_buf[PATH_MAX];
    char name_buf[PATH_MAX];
    if (!to_path(parent, parent_buf) || !to_path(name, name_buf))
        return false;
    return raw_syscall(Sys::WriteSubdir, (int64_t)parent_buf, (int64_t)name_buf) == 0;
}

bool change_dir(string_view path) {
    char buffer[PATH_MAX];
    if (!to_path(path, buffer))
        return false;
    return raw_syscall(Sys::Chdir, (int64_t)buffer, 0) == 0;
}

vector<Entry> list(string_view path) {
    vector<Entry> result;

    char buffer[PATH_MAX];
    if (!to_path(path, buffer))
        return result;

    /*
     *  Syscall 0x2d carries no capacity argument and writes up to 64 entries,
     *  so the buffer must have room for all 64 whatever the directory holds.
     */
    constexpr size_t MAX_ENTRIES = 64;
    VfsDirEntry entries[MAX_ENTRIES];
    memset(entries, 0, sizeof(entries));

    int64_t count = raw_syscall(Sys::ListDirPath, (int64_t)buffer, (int64_t)entries);
    if (count < 0 || count > (int64_t)MAX_ENTRIES)
        return result;

    if (!result.reserve((size_t)count))
        return result;

    for (int64_t i = 0; i < count; i++) {
        const VfsDirEntry &raw = entries[i];
        uint8_t length = raw.name_len;
        if (length > sizeof(raw.name))
            length = sizeof(raw.name);

        Entry entry;
        entry.name = string((const char *)raw.name, length);
        entry.is_dir = raw.is_dir != 0;
        entry.size = raw.size;

        (void)result.push_back(r2::move(entry));
    }

    return result;
}

optional<uint32_t> size_of(string_view path) {
    /*  There is no stat syscall; the size comes from the directory listing.  */
    size_t slash = path.rfind('/');

    string_view directory = slash == npos ? string_view("/") : path.substr(0, slash);
    string_view name = slash == npos ? path : path.substr(slash + 1);

    if (directory.empty())
        directory = string_view("/");

    for (const Entry &entry : list(directory)) {
        if (!entry.is_dir && same_name(entry.name.view(), name))
            return entry.size;
    }

    return nullopt;
}

vector<Mount> mounts() {
    vector<Mount> result;

    constexpr size_t MAX_MOUNTS = 8;
    MountInfo raw[MAX_MOUNTS];
    memset(raw, 0, sizeof(raw));

    int64_t count = raw_syscall(Sys::ListMounts, 0, (int64_t)raw);
    if (count <= 0 || count > (int64_t)MAX_MOUNTS)
        return result;

    if (!result.reserve((size_t)count))
        return result;

    for (int64_t i = 0; i < count; i++) {
        uint8_t length = raw[i].path_len;
        if (length > sizeof(raw[i].path))
            length = sizeof(raw[i].path);

        Mount mount;
        mount.path = string((const char *)raw[i].path, length);
        mount.type = (FsType)raw[i].fs_type;

        (void)result.push_back(r2::move(mount));
    }

    return result;
}

optional<FsckReport> check() {
    FsckReport report;
    memset(&report, 0, sizeof(report));

    if (raw_syscall(Sys::RunFsCheck, 0, (int64_t)&report) != 0)
        return nullopt;

    return report;
}

int64_t FileReader::next(uint8_t *buffer, size_t len) {
    int64_t got = read_at(path_.view(), byte_span(buffer, len), offset_);
    if (got > 0)
        offset_ += (uint64_t)got;
    return got;
}

} // namespace r2::fs
