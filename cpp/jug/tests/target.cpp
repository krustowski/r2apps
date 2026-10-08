// Real kernel/VFS smoke test. The test ISO carries this as jugtest.elf,
// shipped.elf as jprobe.elf, and a floppy INIT.RC containing `fg jugtest`.
#include "../jug.h"
#include <r2/fs.hpp>
#include <r2/heap.hpp>
#include <r2/io.hpp>
#include <r2/libc.hpp>
#include <r2/process.hpp>
#include <r2/time.hpp>
R2_HEAP_ARENA_GROWING(256 * 1024)

static int failure(const char *what) {
    r2::println("JUG TARGET FAIL: ", what); r2::out.flush();
    r2::sleep_at_least(60000); return 1;
}
#define CHECK(expr) do { if (!(expr)) return failure(#expr); } while (0)

extern "C" int main() {
    if (r2::arg(1) == r2::string_view("--probe")) {
#ifdef JUG_SHIPPED_PROBE
        const char marker[] = "old";
#else
        const char marker[] = "fresh";
#endif
        (void)r2::fs::write_at("/mnt/tmp/PROBE.TXT", r2::const_byte_span((const uint8_t *)marker, sizeof(marker) - 1), 0);
        for (;;) r2::sleep(1000);
    }
    jug::Config cfg; CHECK(cfg.load());
    CHECK(!strcmp(cfg.list, "https://cdn.vxn.dev/jug/sums.txt"));
    CHECK(!strcmp(cfg.source, "/mnt/tar/opt/jug/jug.cfg"));
    auto image = r2::fs::read("/mnt/tar/bin/jugtest.elf", 1024 * 1024); CHECK(image);
    jug::Package p; jug::scopy(p.name, "jprobe", sizeof(p.name));
    p.size = image->size(); p.sum = jug::Sha256::of(image->data(), image->size());
    jug::Registry reg;
    const char *why = jug::install(p, image->data(), image->size(), reg);
    if (why) return failure(why);
    auto stored = r2::fs::size_of("/mnt/tmp/jug/JPROBE.ELF"); CHECK(stored && *stored == image->size());
    // Replacing an existing download also uses the real subdirectory rename.
    CHECK(!jug::install(p, image->data(), image->size(), reg));
    CHECK(!r2::fs::exists("/mnt/tmp/jug/JPROBE.NEW"));
    CHECK(!r2::fs::exists("/mnt/tmp/jug/JPROBE.BAK"));
    CHECK(r2::fs::change_dir("/mnt/tar/bin")); // contains the old jprobe.elf
    auto first = r2::spawn("jprobe", "jprobe --probe one"); CHECK(first);
    auto second = r2::spawn("jprobe", "jprobe --probe two"); CHECK(second);
    r2::sleep_at_least(500);
    auto marker = r2::fs::read_text("/mnt/tmp/PROBE.TXT"); CHECK(marker && marker->view() == r2::string_view("fresh"));
    char msg[160]; CHECK(jug::restart("jprobe", false, msg, sizeof(msg)) == 2);
    r2::sleep_at_least(500);
    uint8_t ids[10]; int count = jug::running("jprobe", ids, 10); CHECK(count == 2);
    bool one = false, two = false;
    for (int i = 0; i < count; ++i) {
        CHECK(ids[i] != *first && ids[i] != *second);
        auto line = r2::command_line(ids[i]); CHECK(line);
        one |= line->view() == r2::string_view("jprobe --probe one");
        two |= line->view() == r2::string_view("jprobe --probe two");
        CHECK(r2::kill(ids[i]));
    }
    CHECK(one && two);
    CHECK(!jug::uninstall("jprobe", reg));
    CHECK(!r2::fs::exists("/mnt/tmp/jug/JPROBE.ELF"));
    CHECK(r2::fs::remove("/mnt/tmp/PROBE.TXT"));
    auto shipped = r2::spawn("jprobe", "jprobe --probe shipped"); CHECK(shipped);
    r2::sleep_at_least(500);
    marker = r2::fs::read_text("/mnt/tmp/PROBE.TXT"); CHECK(marker && marker->view() == r2::string_view("old"));
    CHECK(r2::kill(*shipped));
    r2::println("JUG TARGET PASS"); r2::out.flush();
    r2::sleep_at_least(60000); return 0;
}
