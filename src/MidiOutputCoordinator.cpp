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

// Host side of the Passport MIDI card — see MidiOutputCoordinator.h.

#include "MidiOutputCoordinator.h"

#include "HostMidiPort.h"
#include "ThreadGuard.h"

namespace pom2 {

// The browser build is single-threaded and has no host MIDI port (the null
// backend): no sender thread there, and nothing is queued for one. The .mid
// recording, which never needed the thread, works the same.
MidiOutputCoordinator::MidiOutputCoordinator()
{
#ifndef __EMSCRIPTEN__
    thread_ = guardedThread("MidiOut", [this] { run(); });
#endif
}

MidiOutputCoordinator::~MidiOutputCoordinator()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void MidiOutputCoordinator::feed(std::vector<MidiOutByte>& bytes, bool cardPresent, double cpuHz)
{
    if (cardWasPresent_ && !cardPresent) {
        assembler_.reset();
        std::lock_guard<std::mutex> lk(mtx_);
        panic_ = true;
        cv_.notify_all();
    }
    cardWasPresent_ = cardPresent;
    if (bytes.empty()) return;
    assembled_.clear();
    for (const auto& b : bytes) assembler_.feed(b.byte, b.cycle, assembled_);
    bytes.clear();
    if (assembled_.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& m : assembled_) {
        if (recordPending_) { recorder_.start(m.cycle, cpuHz); recordPending_ = false; }
        if (recorder_.recording()) recorder_.add(m);
        if (thread_.joinable() && (!openPort_.empty() || !wantPort_.empty()))
            queue_.push_back({ std::move(m), cpuHz });
    }
    // A port that will not drain (none chosen) must not grow without bound.
    while (queue_.size() > 4096) queue_.pop_front();
    cv_.notify_all();
}

void MidiOutputCoordinator::setPort(const std::string& name)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (name == wantPort_ && !reopen_) return;
    wantPort_ = name;
    reopen_ = true;
    cv_.notify_all();
}

std::string MidiOutputCoordinator::portName() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return wantPort_;
}

std::string MidiOutputCoordinator::status() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

uint64_t MidiOutputCoordinator::messagesSent() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return sent_;
}

void MidiOutputCoordinator::startRecording()
{
    std::lock_guard<std::mutex> lk(mtx_);
    recorder_.discard();
    recordPending_ = true;
}

bool MidiOutputCoordinator::recording() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return recordPending_ || recorder_.recording();
}

std::size_t MidiOutputCoordinator::recordedEvents() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return recorder_.events();
}

bool MidiOutputCoordinator::stopRecording(const std::filesystem::path& path, std::string& error)
{
    MidiFileWriter rec;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (recordPending_) { recordPending_ = false; recorder_.start(0, 1.0); }
        if (!recorder_.recording()) { error = "not recording"; return false; }
        rec = recorder_;
        recorder_.discard();
    }
    return rec.finish(path, error);   // file I/O off the lock
}

void MidiOutputCoordinator::discardRecording()
{
    std::lock_guard<std::mutex> lk(mtx_);
    recordPending_ = false;
    recorder_.discard();
}

void MidiOutputCoordinator::run()
{
    using Clock = std::chrono::steady_clock;
    HostMidiPort port;
    bool anchored = false;
    uint64_t anchorCycle = 0;
    Clock::time_point anchorAt{};
    // All Notes Off + All Sound Off on the 16 channels.
    auto quiet = [&port] {
        for (uint8_t ch = 0; ch < 16; ++ch) {
            const uint8_t off[3] = { uint8_t(0xB0 | ch), 123, 0 };
            const uint8_t snd[3] = { uint8_t(0xB0 | ch), 120, 0 };
            port.send(off, 3);
            port.send(snd, 3);
        }
    };
    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        cv_.wait(lk, [this] { return quit_ || reopen_ || panic_ || !queue_.empty(); });
        if (quit_) break;
        if (reopen_) {
            reopen_ = false;
            const std::string want = wantPort_;
            lk.unlock();
            if (port.isOpen()) quiet();
            port.close();
            std::string error, state;
            if (want.empty()) state = "no output";
            else if (port.open(want, error)) state = "playing to " + want;
            else state = error;
            lk.lock();
            openPort_ = port.isOpen() ? want : std::string();
            status_ = state;
            anchored = false;
            continue;
        }
        if (panic_) {
            panic_ = false;
            queue_.clear();
            anchored = false;
            lk.unlock();
            if (port.isOpen()) quiet();
            lk.lock();
            continue;
        }
        Timed next = std::move(queue_.front());
        queue_.pop_front();
        const double hz = next.cpuHz > 0.0 ? next.cpuHz : 1022727.0;
        const auto now = Clock::now();
        auto due = [&] {
            const double s = double(int64_t(next.msg.cycle - anchorCycle)) / hz;
            return anchorAt + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
        };
        if (!anchored || next.msg.cycle < anchorCycle) {
            anchored = true; anchorCycle = next.msg.cycle; anchorAt = now + kLatency;
        }
        auto at = due();
        if (at > now + kMaxAhead || at < now - kMaxLate) {
            anchorCycle = next.msg.cycle; anchorAt = now + kLatency; at = due();
        }
        // Wait for its instant, waking early for a port change or shutdown.
        if (cv_.wait_until(lk, at, [this] { return quit_ || reopen_ || panic_; })) {
            if (!quit_ && !panic_) queue_.push_front(std::move(next));   // replay after reopen
            continue;
        }
        lk.unlock();
        port.send(next.msg.bytes.data(), next.msg.bytes.size());
        lk.lock();
        ++sent_;
    }
    lk.unlock();
    if (port.isOpen()) quiet();
    port.close();
}

} // namespace pom2
