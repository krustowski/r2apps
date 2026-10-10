#pragma once
#ifdef MEMENTO_BACKEND_R2
#include <r2/types.hpp>
#else
#include <stddef.h>
#include <stdint.h>
#endif

// Only fixed-width values and offsets cross the process boundary. The host
// owns the block, including the two trailing indexed-pixel buffers.
namespace r2web {
constexpr uint32_t Magic = 0x42573252, Version = 1, None = 2, Queue = 64;
constexpr uint32_t MaxDimension = 2048, TextCapacity = 1200;
enum Op : uint32_t { Key = 1, MouseMove, MouseButton, Wheel, Resize };
// A Key command's x on a Ctrl+V with a picture on Memento's clipboard, in a
// window that asked for pictures (telegram.elf's): text names a PNG of it, and
// y is how long it is.  Other windows never set x on a key, so an older child
// sees an ordinary paste.
constexpr int32_t PasteImage = 1;
enum SlotState : uint32_t { Ready = 0, Writing, Reading };

// Matches the input facts the browser uses, without exposing PlatformKey's
// compiler-dependent bool layout or any Memento pointers in the ABI.
#define R2WEB_KEY_FLAGS(X) \
    X(isKeyDown, 0) X(isKeyUp, 1) X(isChar, 2) X(isF, 3) \
    X(isHome, 4) X(isEnd, 5) X(isInsert, 6) X(isPageUp, 7) \
    X(isPageDown, 8) X(isDelete, 9) X(isArrowUp, 10) X(isArrowLeft, 11) \
    X(isArrowRight, 12) X(isArrowDown, 13) X(isLeftControl, 14) \
    X(isRightControl, 15) X(isLeftAlt, 16) X(isRightAlt, 17) \
    X(isSpace, 18) X(isLeftShift, 19) X(isRightShift, 20) X(isTab, 21) \
    X(isBackspace, 22) X(isEnter, 23) X(isEscape, 24)
struct Command {
    uint32_t op, flags;
    int32_t x, y;
    uint32_t value, extra;
    char text[TextCapacity]; // clipboard at the time of a paste
};
struct Frame {
    uint32_t state, serial, width, height;
    char title[96];
};
// Opt-in notifications for hosted apps that do not use initialUrl. The host
// puts this queue there before launching the child; the magic advertises it.
// Keeping it inside the existing block preserves Version 1 and pixel offsets.
constexpr uint32_t NotificationMagic = 0x31594c4e; // "NLY1"
constexpr uint32_t NotificationSlots = 8, NotificationTextCapacity = 96;
struct NotificationQueue {
    uint32_t magic, head, tail; // child writes head; host writes tail
    char text[NotificationSlots][NotificationTextCapacity];
};
struct HostBlock {
    uint32_t magic, version, capacity, maxWidth, maxHeight, colours, portBase;
    uint32_t hostBeat, clientBeat, quit, exited, head, tail, front, frame;
    // Child-to-host mailboxes: the writer waits for pending to return to 0.
    uint32_t copyPending, openPending;
    char copyText[TextCapacity], openUrl[TextCapacity], initialUrl[TextCapacity];
    char error[124];
    // Child-to-host: mark the window for attention (red title and taskbar
    // button until focused), as IRC and Telegram windows do. Taken from the
    // end of error[128], so the layout stays Version 1: an older host never
    // reads it, and an older child never writes it (its errors are short).
    uint32_t attentionPending;
    Frame frames[2];
    Command commands[Queue];
};
inline uint32_t load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void store(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
inline NotificationQueue *notifications(HostBlock *b) {
    return reinterpret_cast<NotificationQueue *>(b->initialUrl);
}
// Copy before releasing a slot: the child may immediately reuse it.
inline bool takeNotification(HostBlock *b, char (&text)[NotificationTextCapacity]) {
    auto *q = notifications(b);
    if (load(&q->magic) != NotificationMagic) return false;
    uint32_t tail = load(&q->tail), head = load(&q->head);
    if (head-tail > NotificationSlots) { store(&q->tail,head); return false; }
    if (head == tail) return false;
    for (uint32_t i=0; i<NotificationTextCapacity; ++i) text[i]=q->text[tail%NotificationSlots][i];
    text[NotificationTextCapacity-1]=0;
    store(&q->tail,tail+1);
    return true;
}
inline bool claim(uint32_t *p, uint32_t state) {
    uint32_t expected = Ready;
    return __atomic_compare_exchange_n(p, &expected, state, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
inline size_t blockSize(uint32_t capacity) { return sizeof(HostBlock) + size_t(capacity)*2; }
inline uint8_t *pixels(HostBlock *b, uint32_t slot) {
    return reinterpret_cast<uint8_t *>(b + 1) + size_t(slot)*b->capacity;
}
inline bool dimensions(const HostBlock *b, uint32_t w, uint32_t h) {
    return w && h && w <= b->maxWidth && h <= b->maxHeight && size_t(w)*h <= b->capacity;
}
static_assert(sizeof(Command) == 1224, "browser command ABI");
static_assert(sizeof(Frame) == 112, "browser frame ABI");
static_assert(sizeof(HostBlock) == 82356, "browser host ABI");
static_assert(sizeof(NotificationQueue) <= TextCapacity, "notifications fit in initialUrl");
static_assert(__builtin_offsetof(HostBlock, initialUrl) % alignof(NotificationQueue) == 0, "notification alignment");
} // namespace r2web
