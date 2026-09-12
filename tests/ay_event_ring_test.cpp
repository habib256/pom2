// AyEventRing — the sound cards' cycle-stamped queue, with its storage
// allocated once (bug hunt #19).
//
// The deque it replaces called the allocator on every block boundary, on the
// REALTIME audio thread, inside the mutex the CPU thread takes for every MMIO
// access. This pins the two properties that make the replacement safe: the
// FIFO semantics the render loop relies on (front/pop, indexed access,
// eraseFront, the producer splice), and the fact that the storage never
// moves — a global `operator new` counter proves no allocation happens after
// construction, which is the whole point.

#include "AyEventRing.h"

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

// Global allocation counter. `operator new` is replaceable, so this sees
// every heap allocation the ring would make.
std::size_t g_allocs = 0;

struct Ev {
    uint64_t cycle = 0;
    uint8_t  chip = 0, reg = 0, val = 0;
};

}  // namespace

void* operator new(std::size_t n)
{
    ++g_allocs;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main()
{
    // Every ring this test uses is constructed FIRST: a constructor does
    // allocate (once, by design), and the property under test is that
    // nothing after it does.
    pom2::AyEventRing<Ev> ring(8);
    pom2::AyEventRing<Ev> producer(8);
    pom2::AyEventRing<Ev> consumer(16);
    assert(ring.capacity() == 8 && ring.empty() && ring.size() == 0);

    // Everything below happens with the allocator watched.
    const std::size_t allocsAfterCtor = g_allocs;

    // ── FIFO order, indexed access, front/pop ───────────────────────────
    for (uint8_t i = 0; i < 5; ++i) assert(ring.push_back(Ev{i, 0, i, i}));
    assert(ring.size() == 5);
    assert(ring.front().reg == 0);
    for (std::size_t i = 0; i < ring.size(); ++i) assert(ring[i].reg == i);
    ring.pop_front();
    assert(ring.size() == 4 && ring.front().reg == 1 && ring[0].reg == 1);

    // ── eraseFront, including past the end ──────────────────────────────
    ring.eraseFront(2);
    assert(ring.size() == 2 && ring.front().reg == 3);
    ring.eraseFront(99);
    assert(ring.empty());

    // ── Wrapping: the head and tail cross the end of the buffer ─────────
    for (uint8_t round = 0; round < 20; ++round) {
        for (uint8_t i = 0; i < 5; ++i)
            assert(ring.push_back(Ev{uint64_t(round * 5 + i), 0, i, i}));
        for (int i = 0; i < 5; ++i) {
            assert(ring.front().cycle == uint64_t(round * 5 + i));
            ring.pop_front();
        }
        assert(ring.empty());
    }

    // ── Full: push_back reports it, and drops the OLDEST ────────────────
    for (uint8_t i = 0; i < 8; ++i) assert(ring.push_back(Ev{i, 0, i, i}));
    assert(ring.size() == 8);
    assert(!ring.push_back(Ev{100, 0, 100, 100}) && "a full ring says so");
    assert(ring.size() == 8);
    assert(ring.front().reg == 1 && "the oldest event made room");
    assert(ring[7].reg == 100);

    // ── The producer splice: everything moves, the source empties ───────
    for (uint8_t i = 0; i < 6; ++i) producer.push_back(Ev{i, 0, i, i});
    consumer.push_back(Ev{99, 0, 99, 99});
    consumer.appendAndClear(producer);
    assert(producer.empty() && consumer.size() == 7);
    assert(consumer[0].reg == 99 && consumer[1].reg == 0 && consumer[6].reg == 5);

    // ── …and not one allocation in any of it ────────────────────────────
    if (g_allocs != allocsAfterCtor) {
        std::printf("FAIL: %zu allocation(s) after construction — the ring "
                    "must not call the allocator on the audio thread\n",
                    g_allocs - allocsAfterCtor);
        return 1;
    }

    std::puts("ay_event_ring OK");
    return 0;
}
