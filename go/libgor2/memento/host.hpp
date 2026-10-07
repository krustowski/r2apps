#ifndef R2_MEMENTO_HOST_HPP
#define R2_MEMENTO_HOST_HPP

// Freestanding C++17 counterpart of libgor2/memento. The includer supplies
// uint32_t, uintptr_t and size_t (as libc++r2 does). The app owns allocation,
// task monitoring and painting; this header owns shared-memory communication.
namespace r2memento {

constexpr uint32_t QueueSize = 32;
constexpr uint32_t NoBuffer = 2;
constexpr uint32_t ExitHostClosed = 1;
constexpr uint32_t ExitHostTimeout = 2;
constexpr uint32_t ExitBadQueue = 3;
constexpr uint32_t ExitRuntime = 4;

struct Command { uint32_t op, value; };

template <class Snapshot> struct Block {
    static_assert(alignof(Snapshot) <= 4 && sizeof(Snapshot) % 4 == 0,
                  "Memento snapshots require 32-bit alignment and size");
    // Snapshot must contain only fixed-size arrays and scalars, no pointers.
    uint32_t magic, version;
    uint32_t clientBeat, frame, front, exited, tail;
    uint32_t hostBeat, reading, quit, head;
    Command commands[QueueSize];
    Snapshot snapshots[2];
    uint32_t exitReason;
    char runtimeText[128];
};

// Sequential consistency pairs with Go's sync/atomic. In particular the
// reader lease must be visible before the second front check, and the writer
// must publish front before checking the lease on its next publication.
inline uint32_t load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
inline void store(uint32_t *p, uint32_t value) { __atomic_store_n(p, value, __ATOMIC_SEQ_CST); }

template <class Snapshot> class Host {
public:
    explicit Host(Block<Snapshot> *block) : block_(block) {}

    // Call once on a zeroed allocation before spawning the child.
    void initialize(uint32_t magic, uint32_t version) {
        block_->magic = magic;
        block_->version = version;
        block_->reading = NoBuffer;
        block_->hostBeat = 1;
    }
    void keepAlive() { store(&block_->hostBeat, load(&block_->hostBeat) + 1); }
    void requestClose() { store(&block_->quit, 1); }
    bool exited() const { return load(&block_->exited) != 0; }
    uint32_t exitReason() const { return load(&block_->exitReason); }

    // One host thread writes commands. False means full or already exited.
    bool send(uint32_t op, uint32_t value = 0) {
        if (exited()) return false;
        uint32_t head = load(&block_->head);
        if (head - load(&block_->tail) >= QueueSize) return false;
        block_->commands[head % QueueSize] = {op, value};
        store(&block_->head, head + 1);
        return true;
    }

    // Copy under a reader lease; never retain pointers into shared snapshots.
    // lastFrame belongs to the window and starts at zero. False means no new
    // frame or a publication raced the lease; retry on the next idle event.
    bool read(Snapshot &snapshot, uint32_t &lastFrame) {
        uint32_t frame = load(&block_->frame);
        if (frame == lastFrame) return false;
        uint32_t front = load(&block_->front);
        if (front > 1) return false;
        store(&block_->reading, front);
        bool ready = load(&block_->front) == front;
        if (ready) {
            snapshot = block_->snapshots[front];
            lastFrame = frame;
        }
        store(&block_->reading, NoBuffer);
        return ready;
    }

private:
    Block<Snapshot> *block_;
};

// Produce "app.elf --host 0x0000000000c01234" without formatting/allocation.
// The executable is one space-free kernel argument; extra app arguments can
// be appended by the caller. False leaves an empty output string.
inline bool launchArguments(char *out, size_t capacity, const char *program, const void *block) {
    if (!out || !capacity) return false;
    out[0] = 0;
    if (!program || !*program || !block) return false;
    size_t length = 0;
    while (program[length]) {
        unsigned char ch = static_cast<unsigned char>(program[length]);
        if (ch <= ' ' || ch == 127 || length + 1 >= capacity) return false;
        length++;
    }
    constexpr char prefix[] = " --host 0x";
    constexpr size_t digits = sizeof(uintptr_t) * 2;
    if (capacity - length <= sizeof(prefix) - 1 + digits) return false;
    size_t at = 0;
    for (size_t i = 0; i < length; i++) out[at++] = program[i];
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) out[at++] = prefix[i];
    uintptr_t address = reinterpret_cast<uintptr_t>(block);
    for (size_t i = digits; i > 0; i--)
        out[at++] = "0123456789abcdef"[(address >> ((i - 1) * 4)) & 15];
    out[at] = 0;
    return true;
}

static_assert(sizeof(Command) == 8, "Memento command ABI");

} // namespace r2memento
#endif
