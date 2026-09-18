/*
 *  test_main.cpp — host-side tests for the parts of libc++r2 that are pure
 *  memory: the arena allocator, the containers, the string and the
 *  formatting.
 *
 *  These run natively, where a failure is a line number instead of a triple
 *  fault in QEMU.  Nothing here may call a syscall: `int 0x7f` from a Linux
 *  process is a fault, so the tests stay clear of io, fs, gfx and net.
 *
 *  Build and run:  make -C tests/host run
 */

#include "r2/algorithm.hpp"
#include "r2/function.hpp"
#include "r2/heap.hpp"
#include "r2/io.hpp"
#include "r2/memory.hpp"
#include "r2/optional.hpp"
#include "r2/string.hpp"
#include "r2/vector.hpp"

/*  The host's printf, declared by hand so that no system header is needed.  */
extern "C" int printf(const char *format, ...);

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char *what, int line) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL  line %d: %s\n", line, what);
    }
}

#define CHECK(cond) check(!!(cond), #cond, __LINE__)

void section(const char *name) { printf("%s\n", name); }

/*  A deterministic pseudo-random sequence, so a failure is reproducible.  */
uint64_t g_seed = 0x2545F4914F6CDD1DULL;
uint32_t next_random() {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return (uint32_t)(g_seed >> 32);
}

/*  Counts constructions and destructions, to prove containers do both.  */
struct Tracked {
    static int live;
    int value;

    Tracked() : value(0) { live++; }
    explicit Tracked(int v) : value(v) { live++; }
    Tracked(const Tracked &other) : value(other.value) { live++; }
    Tracked(Tracked &&other) noexcept : value(other.value) {
        other.value = -1;
        live++;
    }
    Tracked &operator=(const Tracked &other) {
        value = other.value;
        return *this;
    }
    Tracked &operator=(Tracked &&other) noexcept {
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~Tracked() { live--; }
};
int Tracked::live = 0;

/* ------------------------------------------------------------------------ */

void test_heap() {
    section("heap");

    auto before = r2::heap::stats();
    CHECK(before.arena_bytes > 0);

    void *a = r2::heap::allocate(100);
    void *b = r2::heap::allocate(100);
    void *c = r2::heap::allocate(100);
    CHECK(a && b && c);
    CHECK(a != b && b != c);

    /*  Allocations are 16-byte aligned and do not overlap.  */
    CHECK(((uintptr_t)a & 15) == 0);
    CHECK(((uintptr_t)b & 15) == 0);
    CHECK(r2::heap::block_size(a) >= 100);

    unsigned char *pa = (unsigned char *)a;
    unsigned char *pb = (unsigned char *)b;
    for (int i = 0; i < 100; i++)
        pa[i] = 0xAA;
    for (int i = 0; i < 100; i++)
        pb[i] = 0x55;
    for (int i = 0; i < 100; i++)
        CHECK(pa[i] == 0xAA);

    auto during = r2::heap::stats();
    CHECK(during.live_allocations == before.live_allocations + 3);
    CHECK(during.used_bytes > before.used_bytes);

    r2::heap::deallocate(a);
    r2::heap::deallocate(b);
    r2::heap::deallocate(c);

    auto after = r2::heap::stats();
    CHECK(after.live_allocations == before.live_allocations);
    CHECK(after.used_bytes == before.used_bytes);

    /*  Coalescing: three neighbours freed in any order must merge back into
     *  one block, or a long-running program fragments to death.  */
    void *x = r2::heap::allocate(1000);
    void *y = r2::heap::allocate(1000);
    void *z = r2::heap::allocate(1000);
    r2::heap::deallocate(y);
    r2::heap::deallocate(x);
    r2::heap::deallocate(z);
    auto merged = r2::heap::stats();
    CHECK(merged.largest_free_block >= after.largest_free_block);
    CHECK(merged.free_bytes == after.free_bytes);

    /*  realloc keeps the contents.  */
    char *grow = (char *)r2::heap::allocate(16);
    for (int i = 0; i < 16; i++)
        grow[i] = (char)('a' + i % 26);
    char *grown = (char *)r2::heap::reallocate(grow, 512);
    CHECK(grown != nullptr);
    bool preserved = true;
    for (int i = 0; i < 16; i++)
        preserved = preserved && grown[i] == (char)('a' + i % 26);
    CHECK(preserved);
    r2::heap::deallocate(grown);

    /*  Over-alignment.  */
    void *aligned = r2::heap::allocate_aligned(64, 64);
    CHECK(aligned != nullptr);
    CHECK(((uintptr_t)aligned & 63) == 0);
    r2::heap::deallocate_aligned(aligned);

    /*  An impossible request fails cleanly instead of returning garbage.  */
    void *too_big = r2::heap::allocate(before.arena_bytes * 2);
    CHECK(too_big == nullptr);

    /*  Churn: allocate and free in a pattern, and end where we started.  */
    void *blocks[64] = {};
    for (int round = 0; round < 200; round++) {
        int slot = (int)(next_random() % 64);
        if (blocks[slot]) {
            r2::heap::deallocate(blocks[slot]);
            blocks[slot] = nullptr;
        } else {
            blocks[slot] = r2::heap::allocate(next_random() % 700 + 1);
            CHECK(blocks[slot] != nullptr);
        }
    }
    for (int i = 0; i < 64; i++)
        r2::heap::deallocate(blocks[i]);

    auto final_stats = r2::heap::stats();
    CHECK(final_stats.live_allocations == before.live_allocations);
    CHECK(final_stats.used_bytes == before.used_bytes);
    CHECK(final_stats.free_bytes == after.free_bytes);
}

void test_vector() {
    section("vector");

    r2::vector<int> v;
    CHECK(v.empty());
    CHECK(v.size() == 0);

    for (int i = 0; i < 100; i++)
        CHECK(v.push_back(i));

    CHECK(v.size() == 100);
    CHECK(v[0] == 0);
    CHECK(v[99] == 99);
    CHECK(v.front() == 0);
    CHECK(v.back() == 99);
    CHECK(v.capacity() >= 100);
    CHECK(v.at(100) == nullptr);
    CHECK(*v.at(5) == 5);

    v.pop_back();
    CHECK(v.size() == 99);

    r2::vector<int> copy = v;
    CHECK(copy == v);
    CHECK(copy.size() == 99);

    r2::vector<int> moved = r2::move(copy);
    CHECK(moved.size() == 99);
    CHECK(copy.size() == 0);

    moved.erase(moved.begin());
    CHECK(moved.size() == 98);
    CHECK(moved[0] == 1);

    moved.erase(moved.begin(), moved.begin() + 10);
    CHECK(moved.size() == 88);
    CHECK(moved[0] == 11);

    int *inserted = moved.insert(moved.begin(), 7);
    CHECK(inserted != nullptr);
    CHECK(moved[0] == 7);
    CHECK(moved[1] == 11);

    r2::vector<int> from_list{1, 2, 3, 4};
    CHECK(from_list.size() == 4);
    CHECK(from_list[3] == 4);

    /*  Elements are destroyed exactly once.  */
    CHECK(Tracked::live == 0);
    {
        r2::vector<Tracked> tracked;
        for (int i = 0; i < 40; i++)
            CHECK(tracked.emplace_back(i) != nullptr);
        CHECK(Tracked::live == 40);
        CHECK(tracked[39].value == 39);

        r2::vector<Tracked> second = tracked;
        CHECK(Tracked::live == 80);
        second.clear();
        CHECK(Tracked::live == 40);
    }
    CHECK(Tracked::live == 0);

    /*  resize both ways.  */
    r2::vector<int> sized;
    CHECK(sized.resize(10, 3));
    CHECK(sized.size() == 10 && sized[9] == 3);
    CHECK(sized.resize(4));
    CHECK(sized.size() == 4);
}

void test_string() {
    section("string");

    r2::string s;
    CHECK(s.empty());
    CHECK(s.size() == 0);
    CHECK(s.c_str()[0] == '\0');

    /*  Short strings stay inside the object.  */
    s.append("hello");
    CHECK(s.size() == 5);
    CHECK(s == r2::string_view("hello"));
    CHECK(s.capacity() == r2::string::SSO_CAPACITY);

    s += ", world";
    CHECK(s == r2::string_view("hello, world"));
    CHECK(s.size() == 12);

    /*  Crossing out of the inline buffer keeps the contents.  */
    for (int i = 0; i < 50; i++)
        s.append('x');
    CHECK(s.size() == 62);
    CHECK(s.starts_with("hello, world"));
    CHECK(s.back() == 'x');
    CHECK(!s.failed());
    CHECK(s.c_str()[s.size()] == '\0');

    r2::string copy = s;
    CHECK(copy == s);

    r2::string moved = r2::move(copy);
    CHECK(moved == s);
    CHECK(moved.size() == 62);

    r2::string small = "abc";
    r2::string small_moved = r2::move(small);
    CHECK(small_moved == r2::string_view("abc"));

    CHECK(r2::string("abc") < r2::string("abd"));
    CHECK(r2::string("abc").compare("abc") == 0);

    r2::string path = "/SYS/BOOT/KERNEL.ELF";
    CHECK(path.rfind('/') == 9);
    CHECK(path.substr(10) == r2::string_view("KERNEL.ELF"));
    CHECK(path.find("BOOT") == 5);
    CHECK(path.ends_with(".ELF"));
    CHECK(!path.contains("nope"));

    r2::string upper = "MiXeD";
    upper.to_lower();
    CHECK(upper == r2::string_view("mixed"));
    upper.to_upper();
    CHECK(upper == r2::string_view("MIXED"));

    r2::string built = r2::string_view("a") + r2::string_view("b");
    CHECK(built == r2::string_view("ab"));
}

void test_string_view() {
    section("string_view");

    r2::string_view sv("  key=value  ");
    CHECK(sv.trim() == r2::string_view("key=value"));

    auto parts = sv.trim().split('=');
    CHECK(parts.first == r2::string_view("key"));
    CHECK(parts.second == r2::string_view("value"));

    r2::string_view none("novalue");
    auto split_none = none.split('=');
    CHECK(split_none.first == none);
    CHECK(split_none.second.empty());

    CHECK(r2::string_view("abcdef").substr(2, 3) == r2::string_view("cde"));
    CHECK(r2::string_view("abcdef").find('z') == r2::npos);
    CHECK(r2::string_view("abcdef").find("cd") == 2);
    CHECK(r2::string_view("abcdef").starts_with("abc"));
    CHECK(r2::string_view("abcdef").ends_with("def"));
}

void test_numbers() {
    section("numbers and formatting");

    CHECK(r2::to_string((int64_t)0) == r2::string_view("0"));
    CHECK(r2::to_string((int64_t)-42) == r2::string_view("-42"));
    CHECK(r2::to_string((int64_t)1234567890) == r2::string_view("1234567890"));
    CHECK(r2::to_string((uint64_t)255, 16) == r2::string_view("ff"));
    CHECK(r2::to_string((uint64_t)5, 10, 3, '0') == r2::string_view("005"));
    CHECK(r2::to_string((int64_t)-5, 10, 4, '0') == r2::string_view("-005"));
    CHECK(r2::to_string((int64_t)-5, 10, 4, ' ') == r2::string_view("  -5"));

    /*  The extremes, where a naive negation would overflow.  */
    int64_t min_value = (int64_t)0x8000000000000000ULL;
    CHECK(r2::to_string(min_value) == r2::string_view("-9223372036854775808"));
    CHECK(r2::to_string((uint64_t)0xFFFFFFFFFFFFFFFFULL) ==
          r2::string_view("18446744073709551615"));

    CHECK(r2::to_string(3.14159, 2) == r2::string_view("3.14"));
    CHECK(r2::to_string(-0.5, 3) == r2::string_view("-0.500"));
    CHECK(r2::to_string(2.0, 0) == r2::string_view("2"));

    int64_t parsed = 0;
    CHECK(r2::parse_int("-123", parsed) && parsed == -123);
    CHECK(r2::parse_int("  42  ", parsed) && parsed == 42);
    CHECK(!r2::parse_int("12x", parsed));
    CHECK(!r2::parse_int("", parsed));

    uint64_t unsigned_parsed = 0;
    CHECK(r2::parse_uint("ff", unsigned_parsed, 16) && unsigned_parsed == 255);
    CHECK(r2::parse_uint("0xFF", unsigned_parsed, 16) && unsigned_parsed == 255);
    CHECK(!r2::parse_uint("99999999999999999999999", unsigned_parsed));

    /*  format() and the {} substitution.  */
    CHECK(r2::format("{}:{}", 10, 20) == r2::string_view("10:20"));
    CHECK(r2::format("{}", r2::string_view("text")) == r2::string_view("text"));
    CHECK(r2::format("no args") == r2::string_view("no args"));
    CHECK(r2::format("{} left over", 1, 2) == r2::string_view("1 left over"));
    CHECK(r2::format("missing {}") == r2::string_view("missing {}"));
    CHECK(r2::format("{}", true) == r2::string_view("true"));
    CHECK(r2::format("{}", r2::hex(255, 4)) == r2::string_view("00ff"));
    CHECK(r2::format("{}", r2::pad(7, 3, '0')) == r2::string_view("007"));
    CHECK(r2::format("{}", 'c') == r2::string_view("c"));
    CHECK(r2::concat("a", 1, 'b') == r2::string_view("a1b"));
}

void test_algorithm() {
    section("algorithm");

    r2::vector<int> data;
    for (int i = 0; i < 500; i++)
        CHECK(data.push_back((int)(next_random() % 1000)));

    r2::sort(data.begin(), data.end());
    bool ordered = true;
    for (size_t i = 1; i < data.size(); i++)
        ordered = ordered && data[i - 1] <= data[i];
    CHECK(ordered);
    CHECK(data.size() == 500);

    /*  Already sorted, and reverse sorted: the cases a naive quicksort turns
     *  quadratic on.  */
    r2::sort(data.begin(), data.end());
    CHECK(data[0] <= data[1]);

    r2::sort(data.begin(), data.end(), r2::greater<>{});
    bool descending = true;
    for (size_t i = 1; i < data.size(); i++)
        descending = descending && data[i - 1] >= data[i];
    CHECK(descending);

    r2::sort(data.begin(), data.end());
    CHECK(r2::binary_search(data.begin(), data.end(), data[250]));
    CHECK(*r2::lower_bound(data.begin(), data.end(), data[100]) == data[100]);

    int small[] = {5, 3, 1, 4, 2};
    r2::sort(small, small + 5);
    CHECK(small[0] == 1 && small[4] == 5);

    int single[] = {9};
    r2::sort(single, single + 1);
    CHECK(single[0] == 9);

    CHECK(r2::min(3, 7) == 3);
    CHECK(r2::max(3, 7) == 7);
    CHECK(r2::clamp(9, 0, 5) == 5);

    int values[] = {4, 8, 15, 16, 23, 42};
    CHECK(*r2::max_element(values, values + 6) == 42);
    CHECK(*r2::min_element(values, values + 6) == 4);
    CHECK(r2::find(values, values + 6, 15) == values + 2);
    CHECK(r2::count(values, values + 6, 99) == 0);
    r2::reverse(values, values + 6);
    CHECK(values[0] == 42 && values[5] == 4);
}

void test_memory_and_utility() {
    section("memory, optional, function");

    CHECK(Tracked::live == 0);
    {
        auto owned = r2::make_unique<Tracked>(11);
        CHECK(owned);
        CHECK(owned->value == 11);
        CHECK(Tracked::live == 1);

        auto taken = r2::move(owned);
        CHECK(!owned);
        CHECK(taken->value == 11);
        CHECK(Tracked::live == 1);
    }
    CHECK(Tracked::live == 0);

    {
        auto array = r2::make_unique_array<Tracked>(8);
        CHECK(array);
        CHECK(Tracked::live == 8);
        array[3].value = 5;
        CHECK(array[3].value == 5);
    }
    CHECK(Tracked::live == 0);

    r2::optional<int> maybe;
    CHECK(!maybe.has_value());
    CHECK(maybe.value_or(9) == 9);
    maybe = 4;
    CHECK(maybe.has_value());
    CHECK(*maybe == 4);
    maybe.reset();
    CHECK(!maybe);

    {
        r2::optional<Tracked> tracked_opt;
        tracked_opt.emplace(3);
        CHECK(Tracked::live == 1);
        CHECK(tracked_opt->value == 3);
        tracked_opt.reset();
        CHECK(Tracked::live == 0);
    }

    int captured = 5;
    r2::function<int(int)> fn = [captured](int x) { return x + captured; };
    CHECK(fn);
    CHECK(fn(10) == 15);

    r2::function<int(int)> copied = fn;
    CHECK(copied(1) == 6);

    r2::function<void()> empty;
    CHECK(!empty);

    r2::pair<int, r2::string> p = {1, r2::string("one")};
    CHECK(p.first == 1);
    CHECK(p.second == r2::string_view("one"));

    int a = 1, b = 2;
    r2::swap(a, b);
    CHECK(a == 2 && b == 1);
    CHECK(r2::exchange(a, 7) == 2);
    CHECK(a == 7);
}

} // namespace

int main() {
    printf("libc++r2 host tests\n\n");

    test_heap();
    test_vector();
    test_string();
    test_string_view();
    test_numbers();
    test_algorithm();
    test_memory_and_utility();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
