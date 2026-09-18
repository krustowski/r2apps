/*
 *  selftest — the half of the test suite that can only run on r2.
 *
 *  The host tests cover everything that is pure memory.  These are the parts
 *  that need a kernel underneath: startup and global construction, the console,
 *  the filesystem round trip, the clock, sysinfo and the heap under a real
 *  program's load.
 *
 *  Results go to the console AND to CXXTEST.TXT on the floppy, so the run can
 *  be checked afterwards from outside the virtual machine:
 *
 *      make -C tests/target qemu
 */

#include <r2.hpp>

using namespace r2;

namespace {

int g_checks = 0;
int g_failures = 0;
string g_failure_list;

void check(bool condition, string_view what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        /*  The report file is one 512-byte sector, so keep it inside that.  */
        if (g_failure_list.size() + what.size() < 300) {
            g_failure_list += what;
            g_failure_list += "\n";
        }
        printf("  FAIL {}\n", what);
    }
}

#define CHECK(cond) check(!!(cond), string_view(#cond))

/*  Proves .init_array ran: main checks this before anything else.  */
struct StartupWitness {
    StartupWitness() { value = 0x5A5A; }
    int value = 0;
} g_witness;

void test_startup() {
    println("startup");
    CHECK(g_witness.value == 0x5A5A);
    CHECK(arg_count() >= 1);
    CHECK(!arg(0).empty());
}

void test_console() {
    println("console");

    /*  Nothing to assert against here --- the console is write-only --- but a
     *  long line exercises the buffer boundary in Writer, which is where a
     *  crash would show up.  */
    string long_line;
    for (int i = 0; i < 700; i++)
        long_line.append((char)('a' + i % 26));

    CHECK(long_line.size() == 700);
    print(long_line.view());
    print("\n");
    printf("  formatted {} {} {}\n", 1, hex(0xABCD), fixed(1.5, 2));
}

void test_heap() {
    println("heap");

    heap::Stats before = heap::stats();
    CHECK(before.arena_bytes > 0);

    /*  Churn enough to force reuse and coalescing.  The vector of vectors
     *  lives in a scope of its own: its own buffer is an allocation too, and
     *  counting it would make the comparison below fail for the wrong
     *  reason.  */
    {
        vector<vector<uint8_t>> blocks;
        CHECK(blocks.reserve(32));

        for (int i = 0; i < 32; i++) {
            vector<uint8_t> block;
            CHECK(block.resize(1024));
            block[0] = (uint8_t)i;
            block[1023] = (uint8_t)i;
            CHECK(blocks.push_back(r2::move(block)));
        }

        for (int i = 0; i < 32; i++) {
            CHECK(blocks[i][0] == (uint8_t)i);
            CHECK(blocks[i][1023] == (uint8_t)i);
        }
    }

    heap::Stats after = heap::stats();
    CHECK(after.live_allocations == before.live_allocations);
    CHECK(after.free_bytes == before.free_bytes);
    printf("  arena {} bytes, {} free after churn\n", after.arena_bytes, after.free_bytes);
}

void test_filesystem() {
    println("filesystem");

    const string_view path = string_view("CXXPROBE.TXT");
    string payload = format("libc++r2 probe {}\n", ticks());

    CHECK(fs::write_text(path, payload.view()));

    auto read_back = fs::read_text(path);
    CHECK(read_back.has_value());
    if (read_back)
        CHECK(*read_back == payload.view());

    auto listing = fs::list("/");
    CHECK(!listing.empty());

    bool found = false;
    for (const fs::Entry &entry : listing)
        if (entry.name.view().starts_with("CXXPROBE"))
            found = true;
    CHECK(found);

    CHECK(fs::remove(path));
}

void test_time_and_system() {
    println("time and system");

    /*  Plain sleep() is best-effort on this kernel and can return early, so
     *  the check is on sleep_at_least(), which re-enters the syscall until the
     *  deadline has actually passed.  The measurement from the plain one is
     *  printed rather than asserted on.  */
    uint64_t start = ticks();
    sleep(50);
    printf("  sleep(50) moved the clock {} ms\n", ticks() - start);

    start = ticks();
    sleep_at_least(50);
    uint64_t elapsed = ticks() - start;
    CHECK(elapsed >= 40); /*  10 ms tick granularity  */
    printf("  sleep_at_least(50) moved the clock {} ms\n", elapsed);

    auto info = sysinfo();
    CHECK(info.has_value());

    auto now = clock_now();
    CHECK(now.has_value());
    if (now) {
        CHECK(now->month >= 1 && now->month <= 12);
        printf("  rtc {} {}\n", format_date(*now), format_time(*now));
    }

    auto task_list = tasks();
    CHECK(!task_list.empty());
    printf("  {} tasks in the table\n", task_list.size());
}

void test_math() {
    println("math");

    CHECK(sqrt(16.0) == 4.0);
    CHECK(r2::isqrt(144) == 12);
    CHECK(floor(-1.5) == -2.0);
    CHECK(ceil(-1.5) == -1.0);
    CHECK(round(2.5) == 3.0);

    /*  sin and cos at the points where an argument-reduction bug shows.  */
    double values[] = {0.0, r2::PI / 6, r2::PI / 2, r2::PI, 3 * r2::PI / 2, 7.0, -2.0};
    for (double v : values) {
        double s = sin(v);
        double c = cos(v);
        CHECK(s * s + c * c > 0.999 && s * s + c * c < 1.001);
    }

    CHECK(sin(0.0) < 0.0001 && sin(0.0) > -0.0001);
    CHECK(cos(0.0) > 0.9999);
    CHECK(sin(r2::PI / 2) > 0.9999);
}

void write_report() {
    string report;

    report += format("libc++r2 selftest\nchecks {}\nfailures {}\n", g_checks, g_failures);
    report += g_failures == 0 ? string_view("RESULT PASS\n") : string_view("RESULT FAIL\n");
    if (g_failures != 0)
        report += format("failed:\n{}", g_failure_list);

    if (!fs::write_text("CXXTEST.TXT", report.view()))
        println("selftest: could not write CXXTEST.TXT");
}

} // namespace

int main() {
    println("\n=== libc++r2 selftest ===");

    test_startup();
    test_console();
    test_heap();
    test_filesystem();
    test_time_and_system();
    test_math();

    printf("\n{} checks, {} failures\n", g_checks, g_failures);
    write_report();

    return g_failures == 0 ? 0 : 1;
}
