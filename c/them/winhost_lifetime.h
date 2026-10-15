#ifndef _THEM_WINHOST_LIFETIME_INCLUDED_
#define _THEM_WINHOST_LIFETIME_INCLUDED_

#include "winhost.h"

/* Host-owned metadata follows the shared block, preserving the version 1
 * layout seen by the eMulator. Reserve it when the block is allocated so
 * closing a window never needs another allocation to retain ownership. */
class ThemWinLifetime {
  public:
    struct Allocation {
        ThemWinBlock block;
        Allocation *next;
        uint8_t pid;
    };

    static bool pending() { return head() != nullptr; }

    static void retire(ThemWinBlock *block, uint8_t pid) {
        auto *entry = reinterpret_cast<Allocation *>(block);
        block->quit = 1;
        entry->pid = pid;
        entry->next = head();
        head() = entry;
    }

    /* A failed task-table read must be treated as alive by the caller.
     * exited is the emulator's final store and needs no task-table query. */
    template <class Alive, class Release> static void collect(Alive alive, Release release) {
        Allocation **link = &head();
        while (*link) {
            Allocation *entry = *link;
            if (entry->block.exited || !alive(entry->pid)) {
                *link = entry->next;
                release(entry);
            } else {
                link = &entry->next;
            }
        }
    }

  private:
    static Allocation *&head() {
        static Allocation *first = nullptr;
        return first;
    }
};

#endif
