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

#include "Dac8Audio.h"

#include <algorithm>
#include <cmath>

namespace pom2 {

void Dac8Audio::write(uint64_t cycle, uint8_t code)
{
    lastCode_.store(code, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mutex_);
    events_.emplace_back(cycle, code);
    // Each event is an ABSOLUTE level, so trimming the oldest loses only
    // their (long past) contribution — no parity to preserve.
    while (events_.size() > kMaxEvents) events_.pop_front();
    latest_.store(cycle, std::memory_order_relaxed);
}

void Dac8Audio::reset()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        events_.clear();
        ++gen_;
        latest_.store(0, std::memory_order_relaxed);
    }
    resetPending_.store(true, std::memory_order_release);
}

std::size_t Dac8Audio::queuedWrites() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return events_.size();
}

void Dac8Audio::setSampleRate(uint32_t hz)
{
    sampleRate_.store(hz ? hz : kAudioSampleRate, std::memory_order_relaxed);
}

void Dac8Audio::setCpuClock(double hz)
{
    if (hz > 0.0) cpuClockHz_.store(hz, std::memory_order_relaxed);
}

void Dac8Audio::setVolume(float v)
{
    volume_.store(std::clamp(v, 0.0f, 2.0f), std::memory_order_relaxed);
}

void Dac8Audio::fillAudioBuffer(float* output, int frameCount)
{
    if (frameCount <= 0) return;
    if (resetPending_.exchange(false, std::memory_order_acquire)) {
        cursor_ = 0; frac_ = 0.0; anchored_ = false;
        level_ = levelFor(0x00); dcX_ = dcY_ = 0.0;
    }
    const uint32_t sr = sampleRate_.load(std::memory_order_relaxed);
    const double   hz = cpuClockHz_.load(std::memory_order_relaxed);
    const double   cps = hz / static_cast<double>(sr);   // CPU cycles per sample
    const uint64_t lead = static_cast<uint64_t>(kLeadSecs * hz);

    window_.clear();
    uint32_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        gen = gen_;
        const uint64_t latest = latest_.load(std::memory_order_relaxed);
        if (!events_.empty()) {
            const uint64_t front = events_.front().first;
            // First write ever, a producer far ahead (resume), or every
            // queued stamp behind us (a stall the worker resynced): anchor
            // the cursor kLeadSecs behind the oldest pending write.
            if (!anchored_ || latest > cursor_ + 2 * lead || latest < cursor_) {
                const uint64_t base = (latest > cursor_ + 2 * lead) ? latest : front;
                cursor_ = base > lead ? base - lead : 0;
                frac_ = 0.0;
                anchored_ = true;
            }
        }
        // Writes already behind the cursor still set the held level.
        while (!events_.empty() && events_.front().first < cursor_) {
            level_ = levelFor(events_.front().second);
            events_.pop_front();
        }
        const uint64_t end = cursor_ + static_cast<uint64_t>(frameCount * cps) + 2;
        while (!events_.empty() && events_.front().first <= end) {
            window_.push_back(events_.front());
            events_.pop_front();
        }
    }

    const float vol    = isMuted() ? 0.0f : volume_.load(std::memory_order_relaxed);
    const bool  dc     = dcBlock_.load(std::memory_order_relaxed);
    const double pole  = std::pow(0.995, 44100.0 / sr);
    std::size_t ev = 0;
    for (int i = 0; i < frameCount; ++i) {
        const double spanEnd = frac_ + cps;                 // relative to cursor_
        const uint64_t whole = static_cast<uint64_t>(spanEnd);
        double pos = frac_, area = 0.0;
        while (ev < window_.size() &&
               static_cast<double>(window_[ev].first - cursor_) < spanEnd) {
            const double at = std::max(pos, static_cast<double>(window_[ev].first - cursor_));
            area += level_ * (at - pos);
            pos = at;
            level_ = levelFor(window_[ev].second);
            ++ev;
        }
        area += level_ * (spanEnd - pos);
        double y = area / cps;
        if (dc) {
            const double x = y;
            y = x - dcX_ + pole * dcY_;
            dcX_ = x; dcY_ = y;
        }
        output[i] = static_cast<float>(y) * vol;
        cursor_ += whole;
        frac_ = spanEnd - static_cast<double>(whole);
    }

    if (ev < window_.size()) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (gen != gen_) return;   // a reset() in between: those writes are void
        for (std::size_t k = window_.size(); k-- > ev; ) events_.push_front(window_[k]);
    }
}

} // namespace pom2
