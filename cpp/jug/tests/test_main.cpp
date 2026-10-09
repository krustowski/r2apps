// Use the real libc++r2 containers, strings, allocator and Jug code, with
// an in-memory filesystem/task table in place of hardware syscalls.
#include "../jug.h"
#include <r2/fs.hpp>
#include <r2/heap.hpp>
#include <r2/libc.hpp>
#include <r2/process.hpp>
#include <r2/time.hpp>
extern "C" int printf(const char *, ...);

namespace fake {
struct File { r2::string path; r2::vector<uint8_t> data; bool dir = false; };
r2::vector<File> files;
r2::vector<r2::TaskInfo> table;
r2::string commands[256];
r2::string spawned[256];
bool failWrite = false, corruptNew = false, failPublish = false, missingCmd = false;
bool failSpawn = false, failKill = false, stuck = false;
uint64_t freeBytes = 32 * 1024 * 1024;
int kills = 0, starts = 0, reads = 0, changes = 0;
int relaunches = 0;
bool relaunchSupported = true;
uint8_t nextPid = 20;

File *find(r2::string_view path) {
    for (auto &f : files) if (jug::ieq(f.path.view(), path)) return &f;
    return nullptr;
}
void put(const char *path, const void *bytes, size_t n, bool dir = false) {
    File *f = find(path);
    if (!f) { File fresh; fresh.path = r2::string_view(path); (void)files.push_back(fresh); f = &files.back(); }
    f->dir = dir;
    (void)f->data.resize(n);
    if (n) memcpy(f->data.data(), bytes, n);
}
void text(const char *path, const char *value) { put(path, value, strlen(value)); }
void task(uint8_t id, const char *name, const char *cmd) {
    r2::TaskInfo t{}; t.id = id; t.mode = 1; t.status = 1;
    memset(t.name, ' ', sizeof(t.name));
    for (int i = 0; name[i] && i < 8; ++i) t.name[i] = name[i];
    (void)table.push_back(t); commands[id] = r2::string_view(cmd);
}
void reset() {
    files.clear(); table.clear();
    r2::TaskInfo kernel{}; (void)table.push_back(kernel);
    for (int i = 0; i < 256; ++i) { commands[i].clear(); spawned[i].clear(); }
    failWrite = corruptNew = failPublish = missingCmd = failSpawn = failKill = stuck = false;
    freeBytes = 32 * 1024 * 1024; kills = starts = reads = changes = 0; nextPid = 20;
    relaunches = 0; relaunchSupported = true;
}
} // namespace fake

namespace r2::fs {
vector<Mount> mounts() {
    vector<Mount> out;
    for (const char *p : {"/mnt/tmp", "/mnt/tar", "/mnt/iso", "/mnt/fat"}) {
        Mount m; m.path = string_view(p); m.type = FsType::MemDisk; (void)out.push_back(m);
    }
    return out;
}
vector<Entry> list(string_view path) {
    vector<Entry> out;
    for (const auto &f : fake::files) {
        size_t slash = f.path.view().rfind('/');
        if (slash == npos || !jug::ieq(f.path.view().substr(0, slash), path)) continue;
        Entry e; e.name = string(f.path.view().substr(slash + 1));
        e.is_dir = f.dir; e.size = f.data.size(); (void)out.push_back(e);
    }
    return out;
}
bool make_dir(string_view parent, string_view name) {
    string path(parent); path.append('/'); path.append(name);
    fake::put(path.c_str(), nullptr, 0, true); return true;
}
optional<uint32_t> size_of(string_view path) {
    auto *f = fake::find(path); return f && !f->dir ? optional<uint32_t>(f->data.size()) : nullopt;
}
int64_t read_at(string_view path, byte_span out, uint64_t off) {
    ++fake::reads;
    auto *f = fake::find(path); if (!f || f->dir) return -1;
    if (off >= f->data.size()) return 0;
    size_t n = f->data.size() - off; if (n > out.size()) n = out.size();
    memcpy(out.data(), f->data.data() + off, n);
    if (n && fake::corruptNew && path.ends_with(".NEW")) out[0] ^= 1;
    return n;
}
optional<string> read_text(string_view path, size_t cap) {
    auto *f = fake::find(path); if (!f || f->dir || f->data.size() > cap) return nullopt;
    return string((const char *)f->data.data(), f->data.size());
}
int64_t write_at(string_view path, const_byte_span data, uint64_t off) {
    if (fake::failWrite) return -1;
    auto *f = fake::find(path);
    if (!f) { string p(path); fake::put(p.c_str(), nullptr, 0); f = fake::find(path); }
    if (off + data.size() > f->data.size()) (void)f->data.resize(off + data.size());
    if (data.size()) memcpy(f->data.data() + off, data.data(), data.size());
    return data.size();
}
bool remove(string_view path) {
    for (auto it = fake::files.begin(); it != fake::files.end(); ++it)
        if (jug::ieq(it->path.view(), path)) { fake::files.erase(it); return true; }
    return false;
}
bool rename(string_view from, string_view to) {
    if (fake::failPublish && from.ends_with(".NEW")) return false;
    auto *f = fake::find(from); if (!f || fake::find(to)) return false;
    f->path = string(to); return true;
}
optional<Usage> usage(string_view) { return Usage{64 * 1024 * 1024, fake::freeBytes, FsType::MemDisk, FsFormat::Fat16}; }
bool change_dir(string_view) { ++fake::changes; return true; }
} // namespace r2::fs

namespace r2 {
vector<TaskInfo> tasks() { return fake::table; }
bool request_desktop_relaunch(uint8_t) {
    if (!fake::relaunchSupported) return false;
    ++fake::relaunches; return true;
}
optional<string> command_line(uint8_t id) { return fake::missingCmd ? nullopt : optional<string>(fake::commands[id]); }
bool kill(uint8_t id) {
    if (fake::failKill) return false;
    for (auto it = fake::table.begin(); it != fake::table.end(); ++it)
        if (it->id == id) {
            ++fake::kills; if (!fake::stuck) fake::table.erase(it); return true;
        }
    return false;
}
optional<uint8_t> spawn(string_view, string_view args) {
    if (fake::failSpawn) return nullopt;
    uint8_t id = fake::nextPid++; ++fake::starts; fake::spawned[id] = string(args);
    return id;
}
void sleep(uint64_t) noexcept {}
} // namespace r2

namespace {
int checks = 0, failed = 0;
void check(bool ok, const char *expr, int line) {
    ++checks; if (!ok) { ++failed; printf("FAIL line %d: %s\n", line, expr); }
}
#define CHECK(expr) check(bool(expr), #expr, __LINE__)
void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
void elf(uint8_t (&data)[160], uint8_t payload = 1) {
    memset(data, 0, sizeof(data)); memcpy(data, "\x7f" "ELF", 4);
    data[4] = 2; data[5] = data[6] = 1;
    wr16(data + 16, 2); wr16(data + 18, 0x3e); wr32(data + 20, 1);
    wr64(data + 24, 0x600000); wr64(data + 32, 64);
    wr16(data + 52, 64); wr16(data + 54, 56); wr16(data + 56, 1);
    wr32(data + 64, 1); wr32(data + 68, 5); wr64(data + 72, 120);
    wr64(data + 80, 0x600000); wr64(data + 96, 40); wr64(data + 104, 64);
    memset(data + 120, payload, 40);
}
jug::Package package(const uint8_t *data, size_t n) {
    jug::Package p; jug::scopy(p.name, "tnt", sizeof(p.name));
    jug::scopy(p.path, "bin/tnt.elf", sizeof(p.path));
    p.sum = jug::Sha256::of(data, n); p.size = n; return p;
}
bool sameFile(const char *path, const uint8_t *bytes, size_t n) {
    auto *f = fake::find(path); return f && f->data.size() == n && !memcmp(f->data.data(), bytes, n);
}
void shaTests() {
    char hex[65];
    jug::Sha256::of("", 0).hex(hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    jug::Sha256::of("abc", 3).hex(hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *longText = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    jug::Sha256 sha; for (size_t i = 0; i < strlen(longText); ++i) sha.update(longText + i, 1);
    sha.finish().hex(hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    char block[1000]; memset(block, 'a', sizeof(block)); jug::Sha256 million;
    for (int i = 0; i < 1000; ++i) million.update(block, sizeof(block));
    million.finish().hex(hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    jug::Digest parsed; CHECK(jug::Digest::parse(hex, parsed)); CHECK(!jug::Digest::parse("00", parsed));
}
void listTests() {
    char hex[65]; jug::Sha256::of("abc", 3).hex(hex);
    r2::string text("# updated: 2026-10-08 12:00:00 UTC\r\n");
    for (const char *tail : {"  160 bin/TNT.ELF\r\n", " *sh.elf\n", "  5 ../bad.elf\n", "  9 tnt.elf\n", "  0 toolongname.elf\n"}) {
        text.append(hex); text.append(tail);
    }
    jug::Catalog c; CHECK(c.parse(text.view())); CHECK(c.packages.size() == 2); CHECK(c.skipped == 3);
    CHECK(!strcmp(c.packages[0].name, "sh")); CHECK(c.find("TNT")); CHECK(c.find("tnt")->size == 160);
    CHECK(c.find("sh")->size == -1); CHECK(!strcmp(c.updated, "2026-10-08 12:00:00 UTC"));
    CHECK(!c.parse("<html>server error</html>")); CHECK(c.parse("# updated empty catalog\n"));
    CHECK(!c.parse("# updated bad catalog\ngarbage\n"));
    jug::Catalog a, b; CHECK(a.parse(text.view())); CHECK(b.parse(text.view()));
    CHECK(a.same(b)); CHECK(b.freshSince(a).empty());
    b.packages[1].sum.b[0] ^= 1; CHECK(!a.same(b)); // tnt rebuilt
    jug::scopy(b.packages[0].name, "aa", sizeof(b.packages[0].name)); // sh gone, aa new
    auto fresh = b.freshSince(a); CHECK(fresh.size() == 2);
    CHECK(fresh[0].view() == r2::string_view("aa")); CHECK(fresh[1].view() == r2::string_view("tnt"));
    b = a; jug::scopy(b.updated, "later", sizeof(b.updated)); CHECK(!a.same(b)); CHECK(b.freshSince(a).empty());
    CHECK(a.freshSince(jug::Catalog()).size() == 2);
    char name[jug::NAME_CAP]; CHECK(jug::program_name("bin/TNT.ELF;1", name)); CHECK(!strcmp(name, "tnt"));
    CHECK(!jug::valid_name("../tnt")); CHECK(!jug::valid_name("123456789"));
    jug::Registry reg; jug::Record r; jug::scopy(r.name, "tnt", sizeof(r.name));
    jug::scopy(r.path, "/mnt/tmp/jug/TNT.ELF", sizeof(r.path)); r.size = 160; r.sum = jug::Sha256::of("abc", 3);
    CHECK(reg.put(r)); r.size = 161; CHECK(reg.put(r)); CHECK(reg.records.size() == 1);
    jug::Registry loaded; CHECK(loaded.parse(reg.serialize().view())); CHECK(loaded.find("TNT")->size == 161);
    CHECK(!loaded.parse("broken\n")); CHECK(loaded.records.empty()); CHECK(reg.erase("TNT"));
}
void elfTests() {
    uint8_t image[160]; elf(image); CHECK(!jug::elf_problem(image, sizeof(image)));
    CHECK(jug::elf_problem(image, 63)); image[4] = 1; CHECK(jug::elf_problem(image, sizeof(image)));
    elf(image); wr64(image + 104, 20); CHECK(jug::elf_problem(image, sizeof(image)));
    elf(image); wr64(image + 72, 140); CHECK(jug::elf_problem(image, sizeof(image)));
    elf(image); wr64(image + 80, 0x500000); CHECK(jug::elf_problem(image, sizeof(image)));
    elf(image); wr64(image + 24, 0x700000); CHECK(jug::elf_problem(image, sizeof(image)));
    elf(image); wr64(image + 32, ~uint64_t(0)); CHECK(jug::elf_problem(image, sizeof(image)));
    elf(image); wr64(image + 104, ~uint64_t(0)); CHECK(jug::elf_problem(image, sizeof(image)));
}
void storeTests() {
    fake::reset(); jug::Config cfg; CHECK(cfg.load()); CHECK(!strcmp(cfg.list, "https://cdn.vxn.dev/jug/sums.txt"));
    fake::text("/mnt/tar/opt/jug/jug.cfg", "repo = https://example.test/programs/\nlist = bin/list.txt\n");
    CHECK(cfg.load()); CHECK(!strcmp(cfg.list, "https://example.test/programs/bin/list.txt"));
    char url[jug::URL_CAP]; cfg.urlOf("tnt.elf", url, sizeof(url));
    CHECK(!strcmp(url, "https://example.test/programs/bin/tnt.elf"));
    fake::text("/mnt/fat/JUG.CFG", "repo = http://local.test/jug\nlist = sums.txt\ninsecure = yes # local only\n");
    CHECK(cfg.load()); CHECK(cfg.insecure); CHECK(!strcmp(cfg.source, "/mnt/fat/JUG.CFG"));
    CHECK(cfg.check == jug::Config::CHECK_DEFAULT);
    fake::text("/mnt/fat/JUG.CFG", "check = 0\n"); CHECK(cfg.load()); CHECK(cfg.check == 0);
    fake::text("/mnt/fat/JUG.CFG", "check = 5 # often\n"); CHECK(cfg.load()); CHECK(cfg.check == jug::Config::CHECK_MIN);
    fake::text("/mnt/fat/JUG.CFG", "check = 3600\n"); CHECK(cfg.load()); CHECK(cfg.check == 3600);
    fake::text("/mnt/fat/JUG.CFG", "check = soon\n"); CHECK(cfg.load()); CHECK(cfg.check == jug::Config::CHECK_DEFAULT);
    fake::text("/mnt/fat/JUG.CFG", "repo = http://local.test/jug\nlist = sums.txt\ninsecure = yes # local only\n");
    CHECK(!cfg.load("/missing.cfg"));
    uint8_t older[160], fresh[160]; elf(older, 1); elf(fresh, 2);
    auto p = package(fresh, sizeof(fresh)); jug::Registry reg;
    fake::put("/mnt/tmp/jug/TNT.ELF", older, sizeof(older));
    auto wrong = p; wrong.size++; CHECK(jug::install(wrong, fresh, sizeof(fresh), reg));
    wrong = p; wrong.sum.b[0] ^= 1; CHECK(jug::install(wrong, fresh, sizeof(fresh), reg));
    CHECK(sameFile("/mnt/tmp/jug/TNT.ELF", older, sizeof(older)));
    fake::failWrite = true; CHECK(jug::install(p, fresh, sizeof(fresh), reg)); fake::failWrite = false;
    CHECK(sameFile("/mnt/tmp/jug/TNT.ELF", older, sizeof(older)));
    fake::corruptNew = true; CHECK(jug::install(p, fresh, sizeof(fresh), reg)); fake::corruptNew = false;
    CHECK(sameFile("/mnt/tmp/jug/TNT.ELF", older, sizeof(older)));
    fake::freeBytes = 1; CHECK(jug::install(p, fresh, sizeof(fresh), reg)); fake::freeBytes = 32 * 1024 * 1024;
    CHECK(sameFile("/mnt/tmp/jug/TNT.ELF", older, sizeof(older)));
    fake::failPublish = true; CHECK(jug::install(p, fresh, sizeof(fresh), reg)); fake::failPublish = false;
    CHECK(sameFile("/mnt/tmp/jug/TNT.ELF", older, sizeof(older)));
    CHECK(!jug::install(p, fresh, sizeof(fresh), reg)); CHECK(sameFile("/mnt/tmp/jug/TNT.ELF", fresh, sizeof(fresh)));
    CHECK(!fake::find("/mnt/tmp/jug/TNT.NEW")); CHECK(!fake::find("/mnt/tmp/jug/TNT.BAK"));
    CHECK(reg.find("tnt") && reg.find("tnt")->sum == p.sum);
    fake::put("/mnt/tar/bin/tnt.elf", older, sizeof(older)); fake::put("/mnt/iso/bin/tnt.elf", older, sizeof(older));
    auto locals = jug::scan_local(); CHECK(locals.size() == 1); CHECK(locals[0].origin == jug::Origin::Jug);
    jug::Digest sum; CHECK(jug::local_digest(locals[0], reg, sum)); CHECK(sum == p.sum);
    fake::put("/mnt/tmp/jug/TNT.ELF", older, sizeof(older));
    CHECK(jug::local_digest(locals[0], reg, sum)); CHECK(sum != p.sum); // same-size change
    CHECK(!jug::uninstall("tnt", reg)); locals = jug::scan_local(); CHECK(locals[0].origin == jug::Origin::Tar);
    CHECK(jug::local_digest(locals[0], reg, sum)); int reads = fake::reads;
    CHECK(jug::local_digest(locals[0], reg, sum)); CHECK(fake::reads == reads); // immutable cache
    char hex[65]; p.sum.hex(hex); r2::string list(hex); list.append(" 160 tnt.elf\n");
    jug::Model model; CHECK(model.open()); CHECK(model.takeList((const uint8_t *)list.c_str(), list.size(), "today"));
    model.hashAll(); CHECK(model.row("tnt")->state == jug::State::Outdated);
    jug::Catalog cached; CHECK(jug::load_list(cached, model.config.list)); CHECK(!strcmp(cached.updated, "today"));
    CHECK(!jug::load_list(cached, "https://different.test/sums.txt"));
    CHECK(!model.takeList((const uint8_t *)"bad", 3, "later")); CHECK(model.haveList);
    // A check that finds the same list keeps the rows and their sums.
    bool changed = true; reads = fake::reads;
    CHECK(model.takeList((const uint8_t *)list.c_str(), list.size(), "today", &changed)); CHECK(!changed);
    CHECK(model.row("tnt")->state == jug::State::Outdated); CHECK(fake::reads == reads);
    CHECK(model.takeList((const uint8_t *)list.c_str(), list.size(), "tomorrow", &changed)); CHECK(changed);
    CHECK(!strcmp(model.catalog.updated, "tomorrow"));
}
void stampTests() {
    fake::reset(); jug::Registry reg;
    uint8_t older[160], fresh[160], other[160]; elf(older, 1); elf(fresh, 2); elf(other, 3);
    jug::Digest empty = jug::downloads_stamp();
    fake::text("/mnt/tmp/jug/SUMS.TXT", "# list\n"); fake::text("/mnt/tmp/jug/TNT.NEW", "partial");
    CHECK(jug::downloads_stamp() == empty); // neither the list nor a download on its way
    CHECK(!jug::install(package(older, sizeof(older)), older, sizeof(older), reg));
    jug::Digest one = jug::downloads_stamp(); CHECK(one != empty);
    CHECK(jug::downloads_stamp() == one);
    // Another jug: a same-size build, known by its registry record.
    jug::Registry console; CHECK(console.load());
    CHECK(!jug::install(package(fresh, sizeof(fresh)), fresh, sizeof(fresh), console));
    jug::Digest two = jug::downloads_stamp(); CHECK(two != one);
    // Records of shipped programs, and their order, do not count.
    jug::Record shipped; jug::scopy(shipped.name, "aaa", sizeof(shipped.name)); shipped.size = 5;
    jug::scopy(shipped.path, "/mnt/tar/bin/aaa.elf", sizeof(shipped.path));
    CHECK(console.load()); CHECK(console.put(shipped)); CHECK(console.save());
    CHECK(jug::downloads_stamp() == two);
    fake::put("/mnt/tmp/jug/SH.ELF", other, sizeof(other)); CHECK(jug::downloads_stamp() != two);
    CHECK(!jug::uninstall("sh", console)); CHECK(jug::downloads_stamp() == two);
    // The window's model, told by the stamp, takes the console's registry.
    jug::Model model; CHECK(model.open());
    CHECK(!jug::install(package(older, sizeof(older)), older, sizeof(older), console));
    CHECK(model.registry.find("tnt")->sum != console.find("tnt")->sum);
    model.reload(); CHECK(model.registry.find("tnt")->sum == console.find("tnt")->sum);
}
void restartTests() {
    fake::reset(); fake::task(4, "TNT", "tnt eth"); fake::task(5, "TNT", "tnt other");
    char msg[160]; CHECK(jug::restart("tnt", true, msg, sizeof(msg)) == 2);
    CHECK(fake::kills == 2 && fake::starts == 2); CHECK(fake::spawned[20].view() == r2::string_view("tnt eth"));
    CHECK(fake::spawned[21].view() == r2::string_view("tnt other")); CHECK(fake::changes == 0);
    CHECK(jug::restart("jug", false, msg, sizeof(msg)) < 0);
    CHECK(jug::restart("memento", true, msg, sizeof(msg)) == 0); // none running
    fake::reset(); fake::task(4, "MEMENTO", "memento");
    CHECK(jug::restart("memento", true, msg, sizeof(msg)) < 0);
    CHECK(fake::kills == 0 && fake::relaunches == 0);
    uint8_t wm[160]; elf(wm, 7);
    auto p = package(wm, sizeof(wm)); jug::scopy(p.name, "memento", sizeof(p.name));
    jug::Registry reg; CHECK(!jug::install(p, wm, sizeof(wm), reg));
    CHECK(jug::restart("memento", true, msg, sizeof(msg)) == 1);
    CHECK(fake::relaunches == 1 && fake::kills == 0 && fake::starts == 0);
    CHECK(jug::restart("memento", false, msg, sizeof(msg)) == 1);
    CHECK(fake::relaunches == 2 && fake::kills == 0);
    fake::relaunchSupported = false;
    CHECK(jug::restart("memento", true, msg, sizeof(msg)) < 0); CHECK(fake::kills == 0);
    fake::relaunchSupported = true; wm[159] ^= 1;
    fake::put("/mnt/tmp/jug/MEMENTO.ELF", wm, sizeof(wm));
    CHECK(jug::restart("memento", true, msg, sizeof(msg)) < 0);
    CHECK(fake::relaunches == 2 && fake::kills == 0);
    fake::reset(); fake::task(4, "TNT", "tnt eth"); fake::missingCmd = true;
    CHECK(jug::restart("tnt", false, msg, sizeof(msg)) < 0); CHECK(fake::kills == 0);
    fake::missingCmd = false; fake::failKill = true;
    CHECK(jug::restart("tnt", false, msg, sizeof(msg)) < 0); CHECK(fake::starts == 0);
    fake::failKill = false; fake::stuck = true;
    CHECK(jug::restart("tnt", false, msg, sizeof(msg)) < 0); CHECK(fake::starts == 0);
    fake::reset(); fake::task(4, "TNT", "tnt eth"); fake::failSpawn = true;
    CHECK(jug::restart("tnt", false, msg, sizeof(msg)) < 0);
    fake::reset(); CHECK(jug::restart("tnt", false, msg, sizeof(msg)) == 0);
}
} // namespace

int main() {
    shaTests(); listTests(); elfTests(); storeTests(); stampTests(); restartTests();
    printf("Jug: %d checks, %d failures\n", checks, failed); return failed ? 1 : 0;
}
