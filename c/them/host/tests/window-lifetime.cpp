#include <stdint.h>
#include <stdio.h>
#include "winhost_lifetime.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "window lifetime regression FAIL at line %d\n", __LINE__); \
    return 1; } } while (0)

int main() {
    auto *slow = new ThemWinLifetime::Allocation{};
    auto *dead = new ThemWinLifetime::Allocation{};
    auto *done = new ThemWinLifetime::Allocation{};
    auto *later = new ThemWinLifetime::Allocation{};
    unsigned released = 0;
    auto release = [&released](ThemWinLifetime::Allocation *entry) {
        released++;
        delete entry;
    };
    ThemWinLifetime::retire(&slow->block, 1);
    ThemWinLifetime::retire(&dead->block, 2);
    ThemWinLifetime::retire(&done->block, 3);
    CHECK(slow->block.quit && dead->block.quit && done->block.quit);

    // Unknown task state retains every unfinished allocation. A final
    // detach still lets one be reclaimed without querying that state.
    done->block.exited = 1;
    unsigned queries = 0;
    ThemWinLifetime::collect([&queries](uint8_t) { queries++; return true; }, release);
    CHECK(released == 1 && queries == 2 && ThemWinLifetime::pending());

    // Reclaim a crashed task in the middle of the list, preserving a block
    // still in use. A reused pid remains conservatively alive as well.
    ThemWinLifetime::retire(&later->block, 4);
    ThemWinLifetime::collect([](uint8_t pid) { return pid != 2; }, release);
    CHECK(released == 2 && ThemWinLifetime::pending());
    for (unsigned i = 0; i < 5; i++)
        ThemWinLifetime::collect([](uint8_t) { return true; }, release);
    CHECK(released == 2);
    slow->block.exited = 1;
    ThemWinLifetime::collect([](uint8_t) { return true; }, release);
    CHECK(released == 3 && ThemWinLifetime::pending());
    later->block.exited = 1;
    ThemWinLifetime::collect([](uint8_t) { return true; }, release);
    CHECK(released == 4 && !ThemWinLifetime::pending());
    ThemWinLifetime::collect([](uint8_t) { return false; }, release);
    CHECK(released == 4);
    puts("window lifetime regression PASS");
    return 0;
}
