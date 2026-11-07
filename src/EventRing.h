// POM2 — GPL-3.0-or-later
#pragma once
// A FIFO of timestamped events for an audio-thread consumer that may hand
// back the tail it did not reach (`push_front`) — the std::deque it replaces
// could allocate a block there, inside the realtime callback (libstdc++
// frees a node on every emptying pop_front). Here only the PRODUCER ever
// allocates: `push_back` keeps the capacity at least twice the size, so a
// consumer that pops k events and pushes back j <= k of them always finds
// room, whatever the producer added in between (bug hunt 2026-11-07).
// Not thread-safe: both sides hold the owner's lock, as they did.

#include <cstddef>
#include <utility>
#include <vector>

namespace pom2 {

template <typename T>
class EventRing {
public:
    explicit EventRing(std::size_t initialCapacity = 1024)
        : buf_(initialCapacity < 2 ? 2 : initialCapacity) {}

    bool        empty() const { return size_ == 0; }
    std::size_t size()  const { return size_; }
    const T&    front() const { return buf_[head_]; }
    void        clear()       { head_ = size_ = 0; }

    void push_back(const T& v)
    {
        if (2 * (size_ + 1) > buf_.size()) grow(2 * buf_.size());
        buf_[(head_ + size_) % buf_.size()] = v;
        ++size_;
    }
    template <typename... A>
    void emplace_back(A&&... a) { push_back(T(std::forward<A>(a)...)); }

    void pop_front()
    {
        if (size_ == 0) return;
        head_ = (head_ + 1) % buf_.size();
        --size_;
    }
    /// Never allocates. False (and nothing stored) when full, which the
    /// producer's 2x headroom rules out for a tail the consumer popped.
    bool push_front(const T& v)
    {
        if (size_ == buf_.size()) return false;
        head_ = (head_ + buf_.size() - 1) % buf_.size();
        buf_[head_] = v;
        ++size_;
        return true;
    }

private:
    void grow(std::size_t cap)
    {
        std::vector<T> next(cap);
        for (std::size_t i = 0; i < size_; ++i) next[i] = buf_[(head_ + i) % buf_.size()];
        buf_.swap(next);
        head_ = 0;
    }

    std::vector<T> buf_;
    std::size_t    head_ = 0;
    std::size_t    size_ = 0;
};

}  // namespace pom2
