#include <cstddef>
#include <cstdint>
#include <cassert>
#include <cstring>
#include "../../../spotify/host.h"
#include "../examples/counter/host.hpp"

struct Snapshot { uint32_t count; char text[12]; };
using Block = r2memento::Block<Snapshot>;

static_assert(sizeof(Block) == 464 && offsetof(Block, commands) == 44 &&
              offsetof(Block, snapshots) == 300 && offsetof(Block, exitReason) == 332 &&
              offsetof(Block, runtimeText) == 336, "generic ABI offsets");
static_assert(offsetof(SpotifyHostBlock, commands) == 44 &&
              offsetof(SpotifyHostBlock, snapshots) == 300 &&
              offsetof(SpotifyHostBlock, exitReason) == 3908 &&
              offsetof(SpotifyHostBlock, runtimeText) == 3912, "Spotify v4 ABI offsets");

int main() {
    Block block{};
    r2memento::Host<Snapshot> host(&block);
    host.initialize(123, 1);
    assert(block.magic == 123 && block.version == 1 && block.reading == 2 && block.hostBeat == 1);
    host.keepAlive();
    assert(block.hostBeat == 2);

    block.head = block.tail = UINT32_MAX - 2;
    for (uint32_t i = 0; i < r2memento::QueueSize; i++) assert(host.send(i + 1, ~i));
    assert(!host.send(99));
    for (uint32_t i = 0; i < r2memento::QueueSize; i++) {
        auto command = block.commands[block.tail++ % r2memento::QueueSize];
        assert(command.op == i + 1 && command.value == ~i);
    }
    assert(host.send(99));

    uint32_t lastFrame = 0;
    Snapshot snapshot{};
    assert(!host.read(snapshot, lastFrame));
    block.snapshots[1].count = 42;
    r2memento::store(&block.front, 1);
    r2memento::store(&block.frame, 1);
    assert(host.read(snapshot, lastFrame) && snapshot.count == 42 && lastFrame == 1);
    assert(block.reading == r2memento::NoBuffer);
    assert(!host.read(snapshot, lastFrame));
    block.front = 2; block.frame = 2;
    assert(!host.read(snapshot, lastFrame) && lastFrame == 1);

    char args[64];
    assert(r2memento::launchArguments(args, sizeof(args), "app.elf", reinterpret_cast<void *>(0xc01234)));
    assert(!strcmp(args, "app.elf --host 0x0000000000c01234"));
    assert(!r2memento::launchArguments(args, 4, "app.elf", &block) && args[0] == 0);
    assert(!r2memento::launchArguments(args, sizeof(args), "two words", &block) && args[0] == 0);
    assert(!r2memento::launchArguments(args, sizeof(args), "app.elf", nullptr) && args[0] == 0);
    constexpr size_t required = sizeof("app.elf --host 0x0000000000c01234");
    assert(r2memento::launchArguments(args, required, "app.elf", &block));
    assert(!r2memento::launchArguments(args, required - 1, "app.elf", &block));

    host.requestClose();
    assert(block.quit == 1 && !host.exited());
    block.exited = 1; block.exitReason = r2memento::ExitHostClosed;
    assert(host.exited() && host.exitReason() == r2memento::ExitHostClosed && !host.send(1));
}
