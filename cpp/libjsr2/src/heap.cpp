//
//  heap.cpp --- jsr2::Heap, the memory one engine runs in.  See jsr2.h.
//
//  Every block carries its size class just in front of it; a free block's
//  first word links its class's free list.  Chunks are cut front to back and
//  never shrink: a page's scripts live as long as the page, and release()
//  returns everything together.
//
#include "jsr2.h"
#include <string.h>

namespace jsr2 {
namespace {

const uint32_t MAGIC = 0x6a737232; // "jsr2"
const uint32_t BIG = 0xffffffffu;

//  Payload bytes per class: 16-byte steps to 128, then four steps to each
//  doubling up to MaxClassBytes.  QuickJS's 4 KiB arenas fit one exactly.
struct ClassTable
{
    uint32_t size[48];
    int n = 0;
    ClassTable()
    {
        for (uint32_t s = 16; s <= 128; s += 16)
            size[n++] = s;
        for (uint32_t base = 128; base < Heap::MaxClassBytes; base *= 2)
            for (uint32_t q = 5; q <= 8; q++)
                size[n++] = base * q / 4;
    }
};
const ClassTable &table()
{
    static const ClassTable t;
    return t;
}

int classFor(size_t n)
{
    const ClassTable &t = table();
    if (n <= 128)
        return n ? (int)((n + 15) / 16) - 1 : 0;
    for (int i = 8; i < t.n; i++)
        if (n <= t.size[i])
            return i;
    return -1;
}

struct Tag
{
    uint32_t cls;
    uint32_t magic;
};

struct SmallHdr
{
    uint64_t unused;
    Tag tag;
};
static_assert(sizeof(SmallHdr) == 16, "small block header keeps payloads 16-aligned");

Tag *tagOf(const void *p) { return (Tag *)((uint8_t *)p - sizeof(Tag)); }

} // namespace

struct Heap::Chunk
{
    Chunk *next;
    size_t size;
};

struct Heap::Big
{
    Big *prev, *next;
    size_t size;
    Tag tag;
};

void *Heap::carve(int cls)
{
    static_assert(sizeof(Chunk) == 16, "chunk header keeps blocks 16-aligned");
    size_t need = sizeof(SmallHdr) + table().size[cls];
    if (!bump_ || (size_t)(bumpEnd_ - bump_) < need)
    {
        //  What is left of the old chunk goes to the smaller classes, so a
        //  run of big requests does not strand it.
        while (bump_ && (size_t)(bumpEnd_ - bump_) >= sizeof(SmallHdr) + 16)
        {
            size_t room = (size_t)(bumpEnd_ - bump_) - sizeof(SmallHdr);
            int c = classFor(room);
            if (c < 0)
                c = table().n - 1;
            else if (table().size[c] > room)
                c--;
            if (c < 0)
                break;
            SmallHdr *h = (SmallHdr *)bump_;
            h->tag.cls = (uint32_t)c;
            h->tag.magic = MAGIC;
            void *p = h + 1;
            *(void **)p = free_[c];
            free_[c] = p;
            bump_ += sizeof(SmallHdr) + table().size[c];
        }
        if (reserved_ + ChunkBytes > limit_)
            return nullptr;
        Chunk *c = (Chunk *)platform().pageAlloc(ChunkBytes);
        if (!c)
            return nullptr;
        c->next = chunks_;
        c->size = ChunkBytes;
        chunks_ = c;
        reserved_ += ChunkBytes;
        bump_ = (uint8_t *)(c + 1);
        bumpEnd_ = (uint8_t *)c + ChunkBytes;
    }
    SmallHdr *h = (SmallHdr *)bump_;
    bump_ += need;
    h->tag.cls = (uint32_t)cls;
    h->tag.magic = MAGIC;
    return h + 1;
}

void *Heap::alloc(size_t n)
{
    int cls = classFor(n);
    if (cls >= 0)
    {
        void *p = free_[cls];
        if (p)
            free_[cls] = *(void **)p;
        else if (!(p = carve(cls)))
            return nullptr;
        used_ += table().size[cls];
        return p;
    }
    size_t total = sizeof(Big) + n;
    if (total < n || reserved_ + total > limit_)
        return nullptr;
    Big *b = (Big *)platform().pageAlloc(total);
    if (!b)
        return nullptr;
    b->prev = nullptr;
    b->next = bigs_;
    if (bigs_)
        bigs_->prev = b;
    bigs_ = b;
    b->size = n;
    b->tag.cls = BIG;
    b->tag.magic = MAGIC;
    reserved_ += total;
    used_ += n;
    return b + 1;
}

void Heap::free(void *p)
{
    if (!p)
        return;
    Tag *t = tagOf(p);
    if (t->magic != MAGIC)
        return; // not ours: refuse rather than corrupt a list
    if (t->cls == BIG)
    {
        Big *b = (Big *)p - 1;
        if (b->prev)
            b->prev->next = b->next;
        else
            bigs_ = b->next;
        if (b->next)
            b->next->prev = b->prev;
        reserved_ -= sizeof(Big) + b->size;
        used_ -= b->size;
        b->tag.magic = 0;
        platform().pageFree(b);
        return;
    }
    used_ -= table().size[t->cls];
    *(void **)p = free_[t->cls];
    free_[t->cls] = p;
}

size_t Heap::usable(const void *p) const
{
    if (!p)
        return 0;
    const Tag *t = tagOf(p);
    if (t->magic != MAGIC)
        return 0;
    return t->cls == BIG ? ((const Big *)p - 1)->size : table().size[t->cls];
}

void *Heap::realloc(void *p, size_t n)
{
    if (!p)
        return alloc(n);
    if (!n)
    {
        free(p);
        return nullptr;
    }
    size_t have = usable(p);
    if (n <= have && tagOf(p)->cls != BIG)
        return p;
    void *fresh = alloc(n);
    if (!fresh)
        return nullptr;
    memcpy(fresh, p, have < n ? have : n);
    free(p);
    return fresh;
}

void Heap::release()
{
    while (bigs_)
    {
        Big *next = bigs_->next;
        platform().pageFree(bigs_);
        bigs_ = next;
    }
    while (chunks_)
    {
        Chunk *next = chunks_->next;
        platform().pageFree(chunks_);
        chunks_ = next;
    }
    memset(free_, 0, sizeof(free_));
    bump_ = bumpEnd_ = nullptr;
    reserved_ = used_ = 0;
}

} // namespace jsr2
