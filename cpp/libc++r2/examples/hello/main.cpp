/*
 *  hello — what a C++ program for r2 looks like.
 *
 *  Console output, command-line arguments, the containers, the filesystem and
 *  the clock.  Build it with:
 *
 *      make build      # produces hello.elf
 *      make install    # copies HELLO.ELF onto fat.img
 *
 *  and run it from the r2 shell with `fg HELLO.ELF`.
 */

#include <r2.hpp>

using namespace r2;

namespace {

/*  A global with a constructor: proof that .init_array runs before main.  */
struct Banner {
    Banner() { println("libc++r2 ", VERSION, " -- global constructors run"); }
    static constexpr string_view VERSION = string_view("0.1");
} g_banner;

void show_arguments() {
    println("\nargv (", arg_count(), " entries)");
    for (int i = 0; i < arg_count(); i++)
        printf("  [{}] {}\n", i, arg(i));
}

void show_containers() {
    println("\ncontainers");

    vector<string_view> words{"delta", "alpha", "charlie", "bravo"};
    sort(words.begin(), words.end());

    print("  sorted:");
    for (string_view word : words)
        print(" ", word);
    print("\n");

    string joined;
    for (size_t i = 0; i < words.size(); i++) {
        if (i)
            joined += ", ";
        joined += words[i];
    }
    printf("  joined: {}\n", joined);
    printf("  formatted: {} in hex is {}, padded {}\n", 48879, hex(48879), pad(7, 4, '0'));
}

void show_system() {
    println("\nsystem");

    if (auto info = sysinfo()) {
        printf("  host:    {}\n", (const char *)info->system_name);
        printf("  user:    {}\n", (const char *)info->system_user);
        printf("  version: {}\n", (const char *)info->system_version);
        printf("  uptime:  {} s\n", info->system_uptime);
    } else {
        println("  sysinfo unavailable");
    }

    if (auto now = clock_now())
        printf("  clock:   {} {}\n", format_date(*now), format_time(*now));

    printf("  ticks:   {} ms since boot\n", ticks());

    for (const TaskInfo &task : tasks())
        printf("  task {}: {} ({})\n", task.id, (const char *)task.name,
               task_status_name(task.status));
}

void show_filesystem() {
    println("\nfilesystem");

    const string_view path = string_view("CXXHELLO.TXT");
    const string_view text = string_view("written by a C++ program on r2\n");

    if (fs::write_text(path, text)) {
        printf("  wrote {} ({} bytes)\n", path, text.size());

        if (auto read_back = fs::read_text(path))
            printf("  read back: {}", *read_back);
        else
            println("  read back failed");
    } else {
        println("  write failed (is the floppy writable?)");
    }

    auto entries = fs::list("/");
    printf("  {} entries in /\n", entries.size());
    size_t shown = 0;
    for (const fs::Entry &entry : entries) {
        if (shown++ >= 5)
            break;
        printf("    {}{}  {} bytes\n", entry.name, entry.is_dir ? string_view("/") : string_view(),
               entry.size);
    }
}

#if R2_CXX20_OR_LATER

/*
 *  The C++20/23 half of the library.  Guarded, so this same file still builds
 *  with `make build STD=c++17`, where the guard simply removes it.
 */

enum class ConfigError { Missing, Malformed };

/*  expected<T, E>: the value, or the reason there isn't one.  */
expected<int, ConfigError> read_port(string_view text) {
    if (text.empty())
        return unexpected(ConfigError::Missing);

    int64_t value = 0;
    if (!parse_int(text, value) || value <= 0 || value > 65535)
        return unexpected(ConfigError::Malformed);

    return (int)value;
}

string_view describe(ConfigError error) {
    return error == ConfigError::Missing ? string_view("missing") : string_view("malformed");
}

/*  A coroutine: values are produced one at a time, not collected up front.  */
generator<string> directory_names(string_view path) {
    for (const fs::Entry &entry : fs::list(path))
        co_yield entry.name;
}

void show_cxx23() {
    println("\nc++23");

    for (string_view text : {string_view("8080"), string_view("nonsense"), string_view()}) {
        auto port = read_port(text);
        if (port)
            printf("  port \"{}\" -> {}\n", text, *port);
        else
            printf("  port \"{}\" -> {}\n", text, describe(port.error()));
    }

    /*  The monadic form: the error passes straight through the chain.  */
    auto doubled = read_port("21").transform([](int value) { return value * 2; });
    printf("  21 doubled -> {}\n", doubled.value_or(-1));

    size_t count = 0;
    for (const string &name : directory_names("/")) {
        if (count++ < 3)
            printf("  lazily: {}\n", name);
    }
    printf("  {} names pulled from the generator\n", count);

    /*  Three-way comparison, on the library's own types.  */
    printf("  \"abc\" <=> \"abd\" is {}\n",
           (string_view("abc") <=> string_view("abd")) < 0 ? string_view("less")
                                                           : string_view("not less"));
}

#endif

void show_heap() {
    println("\nheap");

    heap::Stats stats = heap::stats();
    printf("  arena:   {} bytes\n", stats.arena_bytes);
    printf("  in use:  {} bytes in {} blocks\n", stats.used_bytes, stats.live_allocations);
    printf("  free:    {} bytes, largest block {}\n", stats.free_bytes, stats.largest_free_block);
    printf("  total:   {} allocations, {} refused\n", stats.total_allocations,
           stats.failed_allocations);
}

} // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    println("\n=== hello from C++ on r2 ===");

    show_arguments();
    show_containers();
    show_system();
    show_filesystem();
#if R2_CXX20_OR_LATER
    show_cxx23();
#endif
    show_heap();

    println("\ndone.");
    return 0;
}
