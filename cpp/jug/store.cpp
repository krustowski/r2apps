//
//  store.cpp --- the disks and the task table: where the programs are, what
//  their sums are, putting a download in place, and starting a program again.
//

#include "jug.h"

#include <r2/algorithm.hpp>
#include <r2/fs.hpp>
#include <r2/heap.hpp>
#include <r2/libc.hpp>
#include <r2/process.hpp>
#include <r2/time.hpp>

namespace jug {

const char *const DIR = "/mnt/tmp/jug";
const char *const REGISTRY_PATH = "/mnt/tmp/jug/JUG.REG";
const char *const LIST_PATH = "/mnt/tmp/jug/SUMS.TXT";

namespace {

const char DEFAULT_REPO[] = "https://cdn.vxn.dev/jug";
const char DEFAULT_LIST[] = "sums.txt";

//  The kernel's search (src/input/elf.rs), the working directory aside.
struct SearchDir
{
    const char *mount;
    const char *path;
    Origin origin;
};
const SearchDir SEARCH[] = {
    {"/mnt/tmp", "/mnt/tmp/jug", Origin::Jug},
    {"/mnt/tar", "/mnt/tar/bin", Origin::Tar},
    {"/mnt/iso", "/mnt/iso/bin", Origin::Iso},
};

//  Reads and writes go in pieces of this much.
const size_t CHUNK = 32 * 1024;

r2::string_view sv(const char *s) { return r2::string_view(s); }

bool ensureDir()
{
    for (const r2::fs::Entry &e : r2::fs::list("/mnt/tmp"))
        if (e.is_dir && ieq(e.name.view(), "jug"))
            return true;
    return r2::fs::make_dir("/mnt/tmp", "JUG");
}

//  The file at `path`, holding `data` and nothing else.  A write never
//  shortens a file, so an older one goes first.
bool replaceFile(const char *path, const uint8_t *data, size_t len)
{
    if (r2::fs::exists(sv(path)) && !r2::fs::remove(sv(path)))
        return false;
    for (size_t done = 0; done < len;)
    {
        size_t n = len - done < CHUNK ? len - done : CHUNK;
        int64_t put = r2::fs::write_at(sv(path), r2::const_byte_span(data + done, n), done);
        if (put != (int64_t)n)
        {
            (void)r2::fs::remove(sv(path));
            return false;
        }
        done += n;
    }
    return true;
}

//  "TNT     .ELF", as the task table has it, is `name`'s.
bool taskIs(const r2::TaskInfo &t, r2::string_view name)
{
    size_t n = 0;
    while (n < 8 && t.name[n] && t.name[n] != ' ' && t.name[n] != '.')
        n++;
    return ieq(r2::string_view((const char *)t.name, n), name);
}

bool live(const r2::TaskInfo &t) { return t.mode == 1 && t.status < 4; }

//  The task table; empty only when the scheduler stayed busy.
r2::vector<r2::TaskInfo> readTasks()
{
    for (int tries = 0; tries < 5; tries++)
    {
        r2::vector<r2::TaskInfo> tasks = r2::tasks();
        if (!tasks.empty())
            return tasks;
        r2::sleep(2);
    }
    return r2::vector<r2::TaskInfo>();
}

//  Waits for a killed task to leave the table, so its slot is free again.
bool waitGone(uint8_t id)
{
    for (int i = 0; i < 100; i++)
    {
        r2::vector<r2::TaskInfo> tasks = r2::tasks();
        bool there = tasks.empty(); // busy: cannot tell yet
        for (const r2::TaskInfo &t : tasks)
            there = there || t.id == id;
        if (!there)
            return true;
        r2::sleep(10);
    }
    return false;
}

//  The kernel searches DIR first, without changing the shared cwd.
r2::optional<uint8_t> spawnFresh(const char *name, const char *cmd)
{
    return r2::spawn(sv(name), sv(cmd));
}

} // namespace

// ─── Configuration ───────────────────────────────────────────────────────────

bool mounted(const char *mount)
{
    for (const r2::fs::Mount &m : r2::fs::mounts())
        if (ieq(m.path.view(), mount))
            return true;
    return false;
}

void Config::finish()
{
    size_t n = strlen(repo);
    while (n && repo[n - 1] == '/')
        repo[--n] = 0;
    if (r2::string_view(listName_).contains("://"))
        scopy(list, listName_, sizeof(list));
    else
    {
        scopy(list, repo, sizeof(list));
        scat(list, "/", sizeof(list));
        scat(list, listName_, sizeof(list));
    }
}

void Config::setRepo(r2::string_view url)
{
    scopy(repo, url, sizeof(repo));
    finish();
}

bool Config::load(const char *path)
{
    scopy(repo, DEFAULT_REPO, sizeof(repo));
    scopy(listName_, DEFAULT_LIST, sizeof(listName_));
    insecure = false;
    check = CHECK_DEFAULT;
    source[0] = 0;

    //  The floppy's, which a user can edit, before the boot medium's.
    static const SearchDir FILES[] = {
        {"/mnt/fat", "/mnt/fat/JUG.CFG", Origin::None},
        {"/mnt/tar", "/mnt/tar/opt/jug/jug.cfg", Origin::None},
        {"/mnt/iso", "/mnt/iso/opt/jug/jug.cfg", Origin::None},
    };
    r2::optional<r2::string> text;
    if (path)
    {
        text = r2::fs::read_text(sv(path), 16 * 1024);
        if (!text)
        {
            finish();
            return false;
        }
        scopy(source, path, sizeof(source));
    }
    else
        for (const SearchDir &f : FILES)
            if (mounted(f.mount) && (text = r2::fs::read_text(sv(f.path), 16 * 1024)))
            {
                scopy(source, f.path, sizeof(source));
                break;
            }

    r2::string_view rest = text ? text->view() : r2::string_view();
    while (!rest.empty())
    {
        auto split = rest.split('\n');
        rest = split.second;
        r2::string_view line = split.first.trim();
        if (line.empty() || line[0] == '#')
            continue;
        auto kv = line.split('=');
        r2::string_view key = kv.first.trim(), value = kv.second.trim();
        //  "insecure = 0   # comment"; a '#' inside a word stays.
        for (size_t i = 1; i < value.size(); i++)
            if (value[i] == '#' && (value[i - 1] == ' ' || value[i - 1] == '\t'))
            {
                value = value.substr(0, i).trim();
                break;
            }
        if (ieq(key, "repo"))
            scopy(repo, value, sizeof(repo));
        else if (ieq(key, "list"))
            scopy(listName_, value, sizeof(listName_));
        else if (ieq(key, "insecure"))
            insecure = value == r2::string_view("1") || ieq(value, "yes") || ieq(value, "true");
        else if (ieq(key, "check"))
        {
            //  A list a few lines long, but not fetched every second.
            uint64_t seconds = 0;
            if (r2::parse_uint(value, seconds))
                check = !seconds ? 0 : seconds < CHECK_MIN ? CHECK_MIN : seconds > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)seconds;
        }
    }
    finish();
    return true;
}

void Config::urlOf(const char *rel, char *out, size_t cap) const
{
    r2::string_view l(list);
    size_t scheme = l.find("://");
    size_t slash = l.rfind('/');
    scopy(out, slash != r2::npos && (scheme == r2::npos || slash > scheme + 2) ? l.substr(0, slash) : l, cap);
    scat(out, "/", cap);
    scat(out, rel, cap);
}

// ─── What is here ────────────────────────────────────────────────────────────

void jug_path(const char *name, char *out, size_t cap)
{
    scopy(out, DIR, cap);
    scat(out, "/", cap);
    size_t at = strlen(out);
    for (size_t i = 0; name[i] && at + 1 < cap; i++)
        out[at++] = name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 32) : name[i];
    out[at] = 0;
    scat(out, ".ELF", cap);
}

r2::vector<Local> scan_local()
{
    r2::vector<Local> out;
    for (const SearchDir &d : SEARCH)
    {
        if (!mounted(d.mount))
            continue;
        for (const r2::fs::Entry &e : r2::fs::list(sv(d.path)))
        {
            Local l;
            if (e.is_dir || !program_name(e.name.view(), l.name))
                continue;
            bool seen = false;
            for (const Local &o : out)
                seen = seen || !strcmp(o.name, l.name);
            if (seen)
                continue; // an earlier directory has it: that copy is the one that runs
            r2::string_view file = e.name.view();
            size_t semi = file.find(';');
            if (semi != r2::npos)
                file = file.substr(0, semi);
            scopy(l.path, d.path, sizeof(l.path));
            scat(l.path, "/", sizeof(l.path));
            scat(l.path, file, sizeof(l.path));
            l.origin = d.origin;
            l.size = e.size;
            (void)out.push_back(l);
        }
    }
    r2::sort(out.begin(), out.end(), [](const Local &a, const Local &b) { return strcmp(a.name, b.name) < 0; });
    return out;
}

Digest downloads_stamp()
{
    Sha256 sha;
    if (!mounted("/mnt/tmp"))
        return sha.finish();
    Registry reg;
    if (r2::optional<r2::string> text = r2::fs::read_text(sv(REGISTRY_PATH), 64 * 1024))
        (void)reg.parse(text->view());
    r2::vector<Record> here;
    for (const r2::fs::Entry &e : r2::fs::list(sv(DIR)))
    {
        Record r;
        if (e.is_dir || !program_name(e.name.view(), r.name))
            continue; // not SUMS.TXT, nor a .NEW or .BAK on its way
        r.size = e.size;
        //  A same-size build put in place by another jug shows in its record.
        if (const Record *known = reg.find(r.name))
            r.sum = known->sum;
        (void)here.push_back(r);
    }
    r2::sort(here.begin(), here.end(), [](const Record &a, const Record &b) { return strcmp(a.name, b.name) < 0; });
    for (const Record &r : here)
    {
        sha.update(r.name, sizeof(r.name));
        sha.update(&r.size, sizeof(r.size));
        sha.update(r.sum.b, sizeof(r.sum.b));
    }
    return sha.finish();
}

bool hash_file(const char *path, uint32_t size, Digest &out)
{
    uint8_t small[512];
    uint8_t *buf = (uint8_t *)r2::heap::allocate(CHUNK);
    size_t cap = CHUNK;
    if (!buf)
    {
        buf = small;
        cap = sizeof(small);
    }
    Sha256 sha;
    uint64_t off = 0;
    while (off < size)
    {
        size_t want = size - off < cap ? (size_t)(size - off) : cap;
        int64_t got = r2::fs::read_at(sv(path), r2::byte_span(buf, want), off);
        if (got <= 0)
            break;
        sha.update(buf, (size_t)got);
        off += (uint64_t)got;
    }
    if (buf != small)
        r2::heap::deallocate(buf);
    out = sha.finish();
    return off == size;
}

bool local_digest(const Local &local, Registry &reg, Digest &out)
{
    // The boot medium is immutable. Downloads can be changed by another
    // Jug instance or a file editor, even without changing their size.
    if (Record *r = local.origin == Origin::Jug ? nullptr : reg.find(local.name))
        if (r->size == local.size && ieq(r->path, local.path))
        {
            out = r->sum;
            return true;
        }
    if (!hash_file(local.path, local.size, out))
        return false;
    Record r;
    scopy(r.name, local.name, sizeof(r.name));
    r.sum = out;
    r.size = local.size;
    scopy(r.path, local.path, sizeof(r.path));
    (void)reg.put(r);
    return true;
}

// ─── Installing ──────────────────────────────────────────────────────────────

const char *install(const Package &pkg, const uint8_t *data, size_t len, Registry &reg)
{
    if (!valid_name(pkg.name))
        return "not a program name";
    if (pkg.size >= 0 && (uint64_t)pkg.size != len)
        return "its size is not the one the list gives";
    Digest sum = Sha256::of(data, len);
    if (sum != pkg.sum)
        return "its SHA-256 is not the one the list gives";
    if (const char *why = elf_problem(data, len))
        return why;
    if (!mounted("/mnt/tmp"))
        return "there is no RAM disk at /mnt/tmp";
    if (!ensureDir())
        return "cannot make /mnt/tmp/jug";

    char path[PATH_CAP], staged[PATH_CAP], backup[PATH_CAP];
    jug_path(pkg.name, path, sizeof(path));
    scopy(staged, r2::string_view(path).substr(0, strlen(path) - 3), sizeof(staged));
    scat(staged, "NEW", sizeof(staged));
    scopy(backup, r2::string_view(path).substr(0, strlen(path) - 3), sizeof(backup));
    scat(backup, "BAK", sizeof(backup));
    // Reserve space for a complete second copy. The old executable remains
    // runnable until the new one has been written and read back.
    if (r2::fs::exists(staged) && !r2::fs::remove(staged))
        return "cannot clear the previous staged download";
    r2::optional<uint32_t> older = r2::fs::size_of(sv(path));
    r2::optional<r2::fs::Usage> use = r2::fs::usage(sv(DIR));
    if (use && use->free < len)
        return "there is not enough room on /mnt/tmp";
    if (!replaceFile(staged, data, len))
        return "it could not be written to /mnt/tmp/jug";

    //  What the next fg will load is what was checked.
    Digest back;
    auto storedSize = r2::fs::size_of(staged);
    if (!storedSize || *storedSize != len || !hash_file(staged, (uint32_t)len, back) || back != sum)
    {
        (void)r2::fs::remove(staged);
        return "it did not read back from /mnt/tmp/jug as it was written";
    }
    if (older)
    {
        if ((r2::fs::exists(backup) && !r2::fs::remove(backup)) || !r2::fs::rename(path, backup))
        {
            (void)r2::fs::remove(staged);
            return "cannot keep the older download before replacement";
        }
    }
    if (!r2::fs::rename(staged, path))
    {
        bool restored = !older || r2::fs::rename(backup, path);
        (void)r2::fs::remove(staged);
        return restored ? "cannot publish the download; the older copy was kept"
                        : "cannot publish or restore the download; the older copy is in .BAK";
    }
    if (older)
        (void)r2::fs::remove(backup);

    Record r;
    scopy(r.name, pkg.name, sizeof(r.name));
    r.sum = sum;
    r.size = (uint32_t)len;
    scopy(r.path, path, sizeof(r.path));
    (void)reg.put(r);
    (void)reg.save();
    return nullptr;
}

const char *uninstall(r2::string_view name, Registry &reg)
{
    if (!valid_name(name))
        return "not a program name";
    char n[NAME_CAP], path[PATH_CAP];
    scopy(n, name, sizeof(n));
    jug_path(n, path, sizeof(path));
    if (!r2::fs::exists(sv(path)))
        return "nothing was downloaded to remove";
    if (!r2::fs::remove(sv(path)))
        return "it cannot be deleted (a kernel older than jug?)";
    //  The shipped copy is what runs now; it is hashed again when looked at.
    (void)reg.erase(name);
    (void)reg.save();
    return nullptr;
}

bool Registry::load()
{
    records = r2::vector<Record>();
    if (!mounted("/mnt/tmp"))
        return false;
    r2::optional<r2::string> text = r2::fs::read_text(sv(REGISTRY_PATH), 64 * 1024);
    return !text || parse(text->view()); // none yet is an empty registry
}

bool Registry::save() const
{
    if (!mounted("/mnt/tmp") || !ensureDir())
        return false;
    r2::string text = serialize();
    return !text.failed() && replaceFile(REGISTRY_PATH, (const uint8_t *)text.c_str(), text.size());
}

bool save_list(const uint8_t *data, size_t len, const char *stamp, const char *source)
{
    if (!mounted("/mnt/tmp") || !ensureDir())
        return false;
    r2::string_view body((const char *)data, len);
    Catalog probe;
    (void)probe.parse(body);
    r2::string text;
    text.append("# jug source ");
    text.append(source);
    text.append('\n');
    if (!probe.updated[0] && stamp && *stamp)
    {
        text.append("# updated ");
        text.append(stamp);
        text.append('\n');
    }
    text.append(body);
    return !text.failed() && replaceFile(LIST_PATH, (const uint8_t *)text.c_str(), text.size());
}

bool load_list(Catalog &out, const char *source)
{
    if (!mounted("/mnt/tmp"))
        return false;
    r2::optional<r2::string> text = r2::fs::read_text(sv(LIST_PATH), 256 * 1024);
    if (!text)
        return false;
    auto first = text->view().split('\n');
    r2::string expected("# jug source ");
    expected.append(source);
    return !expected.failed() && first.first == expected.view() && out.parse(first.second);
}

// ─── Running programs ────────────────────────────────────────────────────────

int running(r2::string_view name, uint8_t *ids, int cap)
{
    int n = 0;
    for (const r2::TaskInfo &t : readTasks())
        if (live(t) && taskIs(t, name) && n < cap)
            ids[n++] = t.id;
    return n;
}

int restart(r2::string_view name, bool /*hosted*/, char *msg, size_t cap)
{
    char n[NAME_CAP];
    scopy(n, name, sizeof(n));
    scopy(msg, n, cap);
    if (!valid_name(name))
    {
        scat(msg, ": not a program name", cap);
        return -1;
    }
    if (ieq(name, "jug"))
    {
        scat(msg, ": not restarted from here, it would take jug with it", cap);
        return -1;
    }
    uint8_t ids[8];
    int count = running(name, ids, 8);
    if (!count)
    {
        scat(msg, " is not running", cap);
        return 0;
    }

    if (ieq(name, "memento"))
    {
        // Never kill the desktop, including from CLI/TNT: that wakes the boot
        // supervisor's ordinary reboot path. Verify the installed update,
        // then let Memento release its own windows and hosted children.
        Registry reg;
        char path[PATH_CAP];
        jug_path("memento", path, sizeof(path));
        bool loaded = reg.load();
        Record *record = loaded ? reg.find("memento") : nullptr;
        auto size = r2::fs::size_of(sv(path));
        Digest sum;
        if (!record || !ieq(record->path, path) || !size || *size != record->size ||
            !hash_file(path, *size, sum) || sum != record->sum)
        {
            scat(msg, ": download and verify its update first", cap);
            return -1;
        }
        int requested = 0;
        for (int i = 0; i < count; ++i)
            if (r2::request_desktop_relaunch(ids[i]))
                ++requested;
        scat(msg, requested ? ": desktop relaunch requested; windows will close"
                            : ": relaunch unsupported; update the kernel and Memento first", cap);
        return requested == count ? requested : -1;
    }

    int started = 0, failed = 0;
    char pids[48] = {};
    for (int i = 0; i < count; i++)
    {
        // Refuse to stop a task unless its arguments can be preserved.
        char cmd[128];
        r2::optional<r2::string> line = r2::command_line(ids[i]);
        if (!line || line->empty() || line->size() >= sizeof(cmd))
        {
            failed++;
            continue;
        }
        scopy(cmd, line->view(), sizeof(cmd));
        if (!r2::kill(ids[i]))
        {
            failed++;
            continue;
        }
        if (!waitGone(ids[i]))
        {
            failed++;
            continue;
        }
        r2::optional<uint8_t> pid = spawnFresh(n, cmd);
        if (!pid)
        {
            failed++;
            continue;
        }
        started++;
        scat(pids, " ", sizeof(pids));
        scatU(pids, *pid, sizeof(pids));
    }
    if (started)
    {
        scat(msg, ": started again as", cap);
        scat(msg, pids, cap);
    }
    if (failed)
        scat(msg, started ? "; some instances could not be restarted" : ": could not restart its instances", cap);
    return failed ? -1 : started;
}

// ─── The model ───────────────────────────────────────────────────────────────

bool Model::open(const char *configPath)
{
    bool ok = config.load(configPath);
    (void)registry.load();
    haveList = load_list(catalog, config.list);
    rescan();
    return ok;
}

bool Model::takeList(const uint8_t *data, size_t len, const char *lastModified, bool *changed)
{
    Catalog fresh;
    if (!fresh.parse(r2::string_view((const char *)data, len)))
        return false;
    if (!fresh.updated[0] && lastModified)
        scopy(fresh.updated, lastModified, sizeof(fresh.updated));
    if (changed && !(*changed = !haveList || !fresh.same(catalog)))
        return true;
    (void)save_list(data, len, lastModified, config.list);
    catalog = fresh;
    haveList = true;
    rebuild();
    return true;
}

void Model::rescan()
{
    locals = scan_local();
    rebuild();
}

void Model::reload()
{
    (void)registry.load();
    registryDirty_ = false;
    rescan();
}

void Model::judge(Row &r)
{
    const Package *p = package(r);
    if (!p)
        r.state = State::Gone;
    else if (r.local < 0)
        r.state = State::Available;
    else if (!r.hashed)
        r.state = State::Unknown;
    else
        r.state = r.sum == p->sum ? State::Current : State::Outdated;
}

void Model::rebuild()
{
    r2::vector<Row> fresh;
    auto localOf = [&](const char *name) {
        for (size_t j = 0; j < locals.size(); j++)
            if (!strcmp(locals[j].name, name))
                return (int)j;
        return -1;
    };
    for (size_t i = 0; i < catalog.packages.size(); i++)
    {
        Row r;
        scopy(r.name, catalog.packages[i].name, sizeof(r.name));
        r.pkg = (int)i;
        r.local = localOf(r.name);
        (void)fresh.push_back(r);
    }
    //  Downloads the server no longer lists: still here, and still run.
    for (size_t j = 0; j < locals.size(); j++)
        if (locals[j].origin == Origin::Jug && !catalog.find(locals[j].name))
        {
            Row r;
            scopy(r.name, locals[j].name, sizeof(r.name));
            r.local = (int)j;
            (void)fresh.push_back(r);
        }
    for (Row &r : fresh)
    {
        if (r.local >= 0 && locals[r.local].origin != Origin::Jug)
            if (Record *rec = registry.find(r.name))
                if (rec->size == locals[r.local].size && ieq(rec->path, locals[r.local].path))
                {
                    r.sum = rec->sum;
                    r.hashed = true;
                }
        judge(r);
    }
    rows = fresh;
    readRunning();
}

bool Model::hashNext()
{
    for (Row &r : rows)
    {
        if (r.local < 0 || r.hashed || r.unreadable || r.pkg < 0)
            continue;
        if (local_digest(locals[r.local], registry, r.sum))
        {
            r.hashed = true;
            registryDirty_ = true;
        }
        else
            r.unreadable = true;
        judge(r);
        return true;
    }
    if (registryDirty_)
    {
        registryDirty_ = false;
        (void)registry.save();
    }
    return false;
}

void Model::hashAll()
{
    while (hashNext())
    {
    }
}

void Model::readRunning()
{
    r2::vector<r2::TaskInfo> tasks = readTasks();
    for (Row &r : rows)
    {
        r.npids = 0;
        for (const r2::TaskInfo &t : tasks)
            if (live(t) && taskIs(t, r.name) && r.npids < (int)sizeof(r.pids))
                r.pids[r.npids++] = t.id;
    }
}

Row *Model::row(r2::string_view name)
{
    for (Row &r : rows)
        if (ieq(r.name, name))
            return &r;
    return nullptr;
}

int Model::count(State s) const
{
    int n = 0;
    for (const Row &r : rows)
        n += r.state == s;
    return n;
}

} // namespace jug
