//
//  lists.cpp --- the server's list, the registry, and what makes an ELF one
//  the kernel can run.  Text and bytes only: no syscalls, so tests/ runs all
//  of it on the host.
//

#include "jug.h"

#include <r2/algorithm.hpp>
#include <r2/libc.hpp>

namespace jug {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

bool nameChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

//  The next whitespace-separated word of `line`, which loses it.
r2::string_view word(r2::string_view &line)
{
    size_t b = 0;
    while (b < line.size() && isSpace(line[b]))
        b++;
    size_t e = b;
    while (e < line.size() && !isSpace(line[e]))
        e++;
    r2::string_view w = line.substr(b, e - b);
    line.remove_prefix(e);
    return w;
}

//  Each line of `text`, without its end.
template <class F> void eachLine(r2::string_view text, F f)
{
    while (!text.empty())
    {
        auto split = text.split('\n');
        f(split.first.trim());
        text = split.second;
    }
}

bool safePath(r2::string_view p)
{
    if (p.empty() || p.size() >= PATH_CAP || p[0] == '/')
        return false;
    for (char c : p)
        if (!nameChar(c) && c != '.' && c != '/')
            return false;
    //  No component may climb out of the list's directory.
    r2::string_view rest = p;
    while (!rest.empty())
    {
        auto split = rest.split('/');
        if (split.first == r2::string_view("..") || split.first.empty())
            return false;
        rest = split.second;
    }
    return true;
}

uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace

// ─── Text ────────────────────────────────────────────────────────────────────

void scopy(char *dst, r2::string_view src, size_t cap)
{
    if (!cap)
        return;
    size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    memcpy(dst, src.data(), n);
    dst[n] = 0;
}

void scat(char *dst, r2::string_view src, size_t cap)
{
    size_t at = strnlen(dst, cap);
    if (at < cap)
        scopy(dst + at, src, cap - at);
}

void scatU(char *dst, uint64_t v, size_t cap)
{
    char num[24];
    int k = 0;
    do
    {
        num[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    char out[24];
    for (int i = 0; i < k; i++)
        out[i] = num[k - 1 - i];
    scat(dst, r2::string_view(out, (size_t)k), cap);
}

void scatSize(char *dst, uint64_t bytes, size_t cap)
{
    if (bytes < 1024)
    {
        scatU(dst, bytes, cap);
        scat(dst, " B", cap);
    }
    else if (bytes < 1024 * 1024)
    {
        scatU(dst, (bytes + 512) / 1024, cap);
        scat(dst, " KiB", cap);
    }
    else
    {
        uint64_t tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
        scatU(dst, tenths / 10, cap);
        scat(dst, ".", cap);
        scatU(dst, tenths % 10, cap);
        scat(dst, " MiB", cap);
    }
}

bool ieq(r2::string_view a, r2::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

// ─── Names ───────────────────────────────────────────────────────────────────

bool valid_name(r2::string_view name)
{
    if (name.empty() || name.size() > NAME_CAP - 1)
        return false;
    for (char c : name)
        if (!nameChar(c))
            return false;
    return true;
}

bool program_name(r2::string_view file, char (&name)[NAME_CAP])
{
    size_t slash = file.rfind('/');
    if (slash != r2::npos)
        file = file.substr(slash + 1);
    //  "TNT.ELF;1": the CD's version number is not part of the name.
    size_t semi = file.find(';');
    if (semi != r2::npos)
        file = file.substr(0, semi);
    size_t dot = file.rfind('.');
    if (dot == r2::npos || !ieq(file.substr(dot + 1), "elf"))
        return false;
    r2::string_view base = file.substr(0, dot);
    if (!valid_name(base))
        return false;
    for (size_t i = 0; i < base.size(); i++)
        name[i] = lower(base[i]);
    name[base.size()] = 0;
    return true;
}

// ─── The server's list ───────────────────────────────────────────────────────

bool Catalog::parse(r2::string_view text)
{
    packages = r2::vector<Package>();
    updated[0] = 0;
    skipped = 0;

    eachLine(text, [&](r2::string_view line) {
        if (line.empty())
            return;
        if (line[0] == '#')
        {
            r2::string_view rest = line.substr(1).trim();
            if (rest.size() >= 7 && ieq(rest.substr(0, 7), "updated"))
            {
                rest = rest.substr(7).trim();
                if (!rest.empty() && rest[0] == ':')
                    rest = rest.substr(1).trim();
                scopy(updated, rest, sizeof(updated));
            }
            return;
        }

        Package p;
        r2::string_view rest = line;
        r2::string_view hex = word(rest);
        r2::string_view second = word(rest);
        r2::string_view path = rest.trim();
        if (path.empty())
            path = second; // sha256sum's "<sum>  <path>"
        else
        {
            uint64_t size = 0;
            if (!r2::parse_uint(second, size) || size > 0xFFFFFFFFull)
            {
                skipped++;
                return;
            }
            p.size = (int64_t)size;
        }
        if (!path.empty() && path[0] == '*') // sha256sum -b
            path.remove_prefix(1);
        if (path.starts_with("./"))
            path.remove_prefix(2);
        if (!Digest::parse(hex, p.sum) || !safePath(path) || !program_name(path, p.name) || find(p.name))
        {
            skipped++;
            return;
        }
        scopy(p.path, path, sizeof(p.path));
        if (!packages.push_back(p))
            skipped++;
    });

    r2::sort(packages.begin(), packages.end(),
             [](const Package &a, const Package &b) { return strcmp(a.name, b.name) < 0; });
    return !packages.empty() || (updated[0] && !skipped);
}

const Package *Catalog::find(r2::string_view name) const
{
    for (const Package &p : packages)
        if (ieq(p.name, name))
            return &p;
    return nullptr;
}

// ─── The registry ────────────────────────────────────────────────────────────

bool Registry::parse(r2::string_view text)
{
    records = r2::vector<Record>();
    bool good = true;
    eachLine(text, [&](r2::string_view line) {
        if (line.empty() || line[0] == '#')
            return;
        Record r;
        r2::string_view rest = line;
        r2::string_view name = word(rest), hex = word(rest), size = word(rest);
        r2::string_view path = rest.trim();
        uint64_t n = 0;
        if (!valid_name(name) || !Digest::parse(hex, r.sum) || !r2::parse_uint(size, n) || n > 0xFFFFFFFFull ||
            path.empty() || path.size() >= PATH_CAP)
        {
            good = false; // a damaged line is dropped: its program is hashed again
            return;
        }
        for (size_t i = 0; i < name.size(); i++)
            r.name[i] = lower(name[i]);
        r.size = (uint32_t)n;
        scopy(r.path, path, sizeof(r.path));
        (void)put(r);
    });
    return good;
}

r2::string Registry::serialize() const
{
    r2::string out("# jug registry: program, SHA-256, size, the file they are of\n");
    for (const Record &r : records)
    {
        char hex[65];
        r.sum.hex(hex);
        char size[24] = {};
        scatU(size, r.size, sizeof(size));
        out.append(r.name);
        out.append(' ');
        out.append(hex);
        out.append(' ');
        out.append(size);
        out.append(' ');
        out.append(r.path);
        out.append('\n');
    }
    return out;
}

Record *Registry::find(r2::string_view name)
{
    for (Record &r : records)
        if (ieq(r.name, name))
            return &r;
    return nullptr;
}

bool Registry::put(const Record &r)
{
    if (Record *old = find(r.name))
    {
        *old = r;
        return true;
    }
    return records.push_back(r);
}

bool Registry::erase(r2::string_view name)
{
    for (auto it = records.begin(); it != records.end(); ++it)
        if (ieq(it->name, name))
        {
            records.erase(it);
            return true;
        }
    return false;
}

// ─── ELF ─────────────────────────────────────────────────────────────────────

const char *elf_problem(const uint8_t *image, size_t len)
{
    //  Where the kernel will put a program (src/input/elf.rs).
    const uint64_t WINDOW_START = 0x600000, WINDOW_END = 0xA00000;
    const size_t EHDR = 64, PHDR = 56;
    const uint32_t PT_LOAD = 1;

    if (len < EHDR)
        return "too short to be a program";
    if (memcmp(image, "\x7f" "ELF", 4) != 0)
        return "not an ELF file";
    if (image[4] != 2 || image[5] != 1)
        return "not a 64-bit little-endian ELF";
    if (image[6] != 1 || rd32(image + 20) != 1 || rd16(image + 52) != EHDR)
        return "its ELF header is damaged";
    if (rd16(image + 16) != 2)
        return "not an executable";
    if (rd16(image + 18) != 0x3E)
        return "not an x86-64 program";

    uint64_t entry = rd64(image + 24), phoff = rd64(image + 32);
    uint16_t phentsize = rd16(image + 54), phnum = rd16(image + 56);
    if (phentsize != PHDR || phnum == 0 || phoff > len || (uint64_t)phnum * PHDR > len - phoff)
        return "its program headers are damaged";

    int loads = 0;
    bool entryLoaded = false;
    for (uint16_t i = 0; i < phnum; i++)
    {
        const uint8_t *ph = image + phoff + (size_t)i * PHDR;
        if (rd32(ph) != PT_LOAD)
            continue;
        uint64_t offset = rd64(ph + 8), vaddr = rd64(ph + 16), filesz = rd64(ph + 32), memsz = rd64(ph + 40);
        if (filesz > memsz)
            return "a segment is bigger in the file than in memory";
        if (offset > len || filesz > len - offset)
            return "a segment runs past the end of the file";
        if (vaddr < WINDOW_START || vaddr > WINDOW_END || memsz > WINDOW_END - vaddr)
            return "it loads outside the process window (0x600000-0xA00000)";
        if (entry >= vaddr && entry - vaddr < memsz)
            entryLoaded = true;
        loads++;
    }
    if (!loads)
        return "it has nothing to load";
    if (!entryLoaded)
        return "its entry point is not in what it loads";
    return nullptr;
}

const char *state_name(State s)
{
    switch (s)
    {
    case State::Unknown:
        return "?";
    case State::Current:
        return "current";
    case State::Outdated:
        return "update";
    case State::Available:
        return "new";
    case State::Gone:
        return "gone";
    }
    return "?";
}

const char *origin_name(Origin o)
{
    switch (o)
    {
    case Origin::Jug:
        return "jug";
    case Origin::Tar:
        return "tar";
    case Origin::Iso:
        return "iso";
    default:
        return "-";
    }
}

} // namespace jug
