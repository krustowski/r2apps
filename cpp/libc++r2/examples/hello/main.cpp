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
    show_heap();

    println("\ndone.");
    return 0;
}
