// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// AyEventRing — the cycle-stamped register queue of a sound card, with its
// storage allocated ONCE.
//
// Why it exists (bug hunt #19). Both AY cards carried their queues in a
// `std::deque`: the producer pushed under the card mutex, and the audio
// thread spliced, popped and erased — `operator new` / `operator delete` on
// every deque block boundary, ON THE REALTIME THREAD, inside the same lock
// the CPU thread takes for every MMIO access. `Mockingboard.cpp` states the
// rule itself, for its speech scratch buffer: "this runs on the realtime
// audio thread, where a heap allocation per buffer tick is the canonical
// source of underruns/clicks". The queues broke that rule 160 lines below
// it. Steady-state music is a few hundred events per frame, so it is not a
// constant glitch — it is a latency cliff, which is worse to diagnose.
//
// What it is: a fixed-capacity FIFO with random access, sized in the
// constructor and never resized again. Deliberately NOT lock-free — the
// cards' mutex already orders producer and consumer, and the point here is
// the allocator, not the lock.
//
// Storage lives in a `std::vector` rather than a `std::array` member: a
// consumer ring is ~0.5 MB, and several tests build a card on the STACK,
// where macOS's 512 KB thread stacks would overflow (CLAUDE.md's standing
// rule — "DiskImage is 242 KB, never stack it on a thread").

#ifndef POM2_AY_EVENT_RING_H
#define POM2_AY_EVENT_RING_H

#include <cstddef>
#include <vector>

namespace pom2 {

template <typename Ev>
class AyEventRing
{
public:
    /// `capacity` events fit; one slot is kept spare so full and empty are
    /// distinguishable without a separate count.
    explicit AyEventRing(std::size_t capacity)
        : buf_(capacity + 1) {}

    bool        empty() const noexcept { return head_ == tail_; }
    std::size_t size()  const noexcept
    {
        return (tail_ >= head_) ? (tail_ - head_)
                                : (buf_.size() - head_ + tail_);
    }
    std::size_t capacity() const noexcept { return buf_.size() - 1; }

    void clear() noexcept { head_ = tail_ = 0; }

    /// Append one event. Returns false — dropping the OLDEST to make room —
    /// when the ring is full. The cards' own overflow policies (break the
    /// timeline on the producer side, drop-by-applying on the consumer's)
    /// run long before this can happen; it is the floor, not the policy.
    bool push_back(const Ev& e)
    {
        const std::size_t next = advance(tail_);
        bool ok = true;
        if (next == head_) {              // full
            head_ = advance(head_);
            ok = false;
        }
        buf_[tail_] = e;
        tail_ = next;
        return ok;
    }

    const Ev& front() const noexcept { return buf_[head_]; }
    void pop_front() noexcept { if (!empty()) head_ = advance(head_); }

    /// Indexed from the head, like the deque this replaces.
    const Ev& operator[](std::size_t i) const noexcept
    {
        std::size_t k = head_ + i;
        if (k >= buf_.size()) k -= buf_.size();
        return buf_[k];
    }

    /// Drop the first `n` events (the `erase(begin, begin + n)` this
    /// replaces). `n` larger than `size()` empties the ring.
    void eraseFront(std::size_t n) noexcept
    {
        const std::size_t have = size();
        if (n >= have) { clear(); return; }
        std::size_t k = head_ + n;
        if (k >= buf_.size()) k -= buf_.size();
        head_ = k;
    }

    /// Move everything out of `src` and onto the back of this ring — the
    /// producer-to-consumer splice, without touching the allocator.
    void appendAndClear(AyEventRing& src)
    {
        const std::size_t n = src.size();
        for (std::size_t i = 0; i < n; ++i) push_back(src[i]);
        src.clear();
    }

private:
    std::size_t advance(std::size_t i) const noexcept
    {
        return (i + 1 == buf_.size()) ? 0 : i + 1;
    }

    std::vector<Ev> buf_;
    std::size_t     head_ = 0;
    std::size_t     tail_ = 0;
};

}  // namespace pom2

#endif  // POM2_AY_EVENT_RING_H
