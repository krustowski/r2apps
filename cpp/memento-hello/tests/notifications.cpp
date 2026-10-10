#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../../r2web/host.h"
#include "ui/platform/impl/r2/R2_NotificationStack.h"

static void stack()
{
    Memento::MementoR2Impl::NotificationStack<char, uint64_t> bubbles;
    char message[] = "first";
    assert(bubbles.Add(message, 100, 6000));
    message[0] = 'X'; // text was copied, not borrowed from the IPC slot
    assert(bubbles.Add("second", 200, 1000));
    assert(bubbles.Add("third", 300, 6000));
    assert(bubbles.Count() == 3 && !std::strcmp(bubbles.At(0).text, "first"));
    assert(!bubbles.Expire(1199));
    assert(bubbles.Expire(1200)); // a shorter-lived middle box expires first
    assert(bubbles.Count() == 2 && !std::strcmp(bubbles.At(1).text, "third"));
    assert(!bubbles.Add(nullptr, 1200, 10));
    assert(!bubbles.Add("", 1200, 10));
    assert(!bubbles.Add("invisible", 1200, 0));
    assert(bubbles.Expire(6100));
    assert(bubbles.Count() == 1 && !std::strcmp(bubbles.At(0).text, "third"));
    assert(bubbles.Expire(6300) && bubbles.Count() == 0);

    // Flood: only the newest twelve remain, in the same order as arrival.
    for (int i = 0; i < 20; ++i) {
        char text[20]; std::snprintf(text, sizeof(text), "message %d", i);
        bubbles.Add(text, 10000, 6000);
    }
    assert(bubbles.Count() == 12);
    assert(!std::strcmp(bubbles.At(0).text, "message 8"));
    assert(!std::strcmp(bubbles.At(11).text, "message 19"));
    char longText[200]; std::memset(longText, 'x', sizeof(longText)); longText[199] = 0;
    bubbles.Add(longText, 10000, 6000);
    assert(std::strlen(bubbles.At(11).text) == 95);
    assert(bubbles.Expire(16000) && bubbles.Count() == 0);
}

static void queue()
{
    using namespace r2web;
    static HostBlock block{};
    char text[NotificationTextCapacity];
    std::strcpy(block.initialUrl, "about:home");
    assert(!takeNotification(&block, text));
    auto *q = notifications(&block);
    *q = NotificationQueue{};
    q->magic = NotificationMagic;
    q->head = q->tail = UINT32_MAX - 3;
    assert(sizeof(*q) == 780 && __builtin_offsetof(NotificationQueue, text) == 12);
    assert(__builtin_offsetof(HostBlock, initialUrl) == 2468);
    // Match hosted.Client.Notify's producer, including counter rollover.
    for (unsigned i = 0; i < NotificationSlots; ++i) {
        std::snprintf(q->text[q->head%NotificationSlots], NotificationTextCapacity, "notice %u", i);
        store(&q->head, q->head+1);
    }
    Memento::MementoR2Impl::NotificationStack<char, uint64_t> bubbles;
    for (unsigned i = 0; i < NotificationSlots; ++i) {
        assert(takeNotification(&block, text));
        char expected[20]; std::snprintf(expected, sizeof(expected), "notice %u", i);
        assert(!std::strcmp(text, expected));
        bubbles.Add(text, 0, 6000);
    }
    assert(!takeNotification(&block, text) && bubbles.Count() == NotificationSlots);
    // A bad child cannot make the host read beyond the queue or a text slot.
    q->head = q->tail + NotificationSlots + 1;
    assert(!takeNotification(&block, text) && q->head == q->tail);
    std::memset(q->text[q->head%NotificationSlots], 'x', NotificationTextCapacity);
    ++q->head;
    assert(takeNotification(&block, text) && text[NotificationTextCapacity-1] == 0);
}

int main()
{
    stack(); queue();
    std::puts("Notification stack and hosted queue checks passed");
}
