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

// EmulationController_Input — host input reaching the machine: the mouse's
// routing, and the RECORDING and PLAYBACK of everything that comes in from
// outside (2026-11-27).
//
// What makes a recording replay to the cycle. The machine itself is
// deterministic: from one state and one sequence of inputs it runs the same
// way every time (Step Back is built on the same fact, and checks it). What
// is not deterministic is WHEN an input lands: the UI thread writes a key or
// a paddle while the CPU worker is in the middle of a slice, at whatever
// instruction it has reached. So, while recording, Memory hands every host
// input to the tap below instead (Memory::setInputTap), and the worker
// applies them at the start of its next slice — an instruction boundary —
// stamping each with that cycle. A key is then up to one slice late (4096
// cycles, 4 ms), which no one can hear or see. A playback, started from the
// same session, cuts its slices to end exactly on each entry's cycle and
// applies the same inputs there.
//
// What is recorded besides keys, pastes, the game port and the mouse: the
// three resets (made at a slice boundary under the lock, so they are
// stamped where they ran), and every change of the CPU clock an accelerator
// or the speed menu makes (applyAcceleratorClock: it changes what the Disk
// II's platter and the beam see, so it is machine state). The clocks a
// program reads the date from (No-Slot Clock, ThunderClock) read a clock of
// the recording's own: its start instant and UTC offset plus the EMULATED
// time since (HostClock.h), so a date replays the same bytes on another day
// and in another time zone.
//
// What a recording cannot keep ends it, at that cycle, saying why: bytes
// that come in from a peer (serial, network, FujiNet — HostIoEpoch.h's
// exchange counter), a change made by hand (noteChangedByHand: the Memory
// viewer, the PC, a mount), a jump in time (rewind, snapshot, session), a
// profile switch, a boot from a slot. What it records up to there stays
// good.
//
// THE PLAYBACK IS CHECKED. Every 15 frames the recording stores the
// machine's fingerprint (cycle, registers, the 64 KiB the CPU sees); the
// playback compares at the same cycle and stops at the first difference,
// saying where — a recording never "plays" something else silently.

#include "EmulationController.h"

#include "HostClock.h"
#include "HostIoEpoch.h"
#include "Logger.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <utility>

namespace {
constexpr int kOff = 0, kRecording = 1, kPlaying = 2;
constexpr int kCheckpointFrames = 15;

bool isLevel(pom2::InputEvent::Kind k)
{
    using K = pom2::InputEvent::Kind;
    switch (k) {
    case K::KeyHeld: case K::Paddle: case K::PaddleButton: case K::OpenApple:
    case K::SolidApple: case K::Shift: case K::Control: case K::Joyport: case K::Mouse:
        return true;
    default:
        return false;
    }
}
} // namespace

struct EmulationController::InputJournalState final : pom2::InputTap {
    std::atomic<int> mode{kOff};
    // Any thread (the UI, the AI server): what waits for a slice boundary,
    // and the last value of every LEVEL input, so that a paddle written
    // every frame with the same value is one entry, not sixty a second.
    std::mutex mtx;
    std::vector<pom2::InputEvent> pending;
    std::map<std::pair<int, int>, int64_t> level;
    // The CPU side, under stateMutex.
    std::vector<pom2::TimedInput> entries;
    size_t next = 0;
    uint64_t nextCheckpoint = 0, exchangeAtStart = 0;
    uint64_t applied = 0, checks = 0;
    int64_t epoch = 0;          // the recording's own clock (HostClock.h)
    long utcOffset = 0;
    uint64_t epochCycle = 0;
    double epochHz = 1.0;
    const Memory* memory = nullptr;
    // The last outcome, read by the UI.
    mutable std::mutex statusMtx;
    std::string status, endWhy;

    bool takeInput(const pom2::InputEvent& e) override
    {
        const int m = mode.load(std::memory_order_acquire);
        if (m == kPlaying) return true;     // the recording drives; the host is not heard
        if (m != kRecording) return false;
        std::lock_guard<std::mutex> lk(mtx);
        if (isLevel(e.kind)) {
            const bool mouse = e.kind == pom2::InputEvent::Kind::Mouse;
            const std::pair<int, int> key{static_cast<int>(e.kind), mouse ? 0 : e.a};
            const int64_t value = mouse ? (e.a | (e.b << 8) | (e.c << 16)) : e.b;
            const auto it = level.find(key);
            if (it != level.end() && it->second == value) return true;
            level[key] = value;
        }
        pending.push_back(e);
        return true;
    }

    void setStatus(std::string s)
    {
        std::lock_guard<std::mutex> lk(statusMtx);
        status = std::move(s);
    }
};

std::shared_ptr<EmulationController::InputJournalState> EmulationController::makeInputJournalState()
{
    return std::make_shared<InputJournalState>();
}

namespace {
// The recording's clock: its start instant, in the recording's time zone,
// plus the EMULATED seconds since (HostClock.h). On the CPU thread.
std::tm journalTime(void* context)
{
    auto* j = static_cast<EmulationController::InputJournalState*>(context);
    const uint64_t now = j->memory ? j->memory->getCycleCounter() : j->epochCycle;
    const double elapsed = static_cast<double>(now - std::min(now, j->epochCycle)) / j->epochHz;
    return pom2::utcTime(static_cast<std::time_t>(j->epoch + j->utcOffset + static_cast<int64_t>(elapsed)));
}

uint64_t fnv(uint64_t h, const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
} // namespace

uint64_t EmulationController::machineFingerprintLocked() const
{
    uint64_t h = 1469598103934665603ULL;
    const uint64_t cycle = mem.getCycleCounter();
    const uint8_t regs[7] = {
        static_cast<uint8_t>(processor.getProgramCounter() & 0xFF),
        static_cast<uint8_t>(processor.getProgramCounter() >> 8),
        processor.getAccumulator(), processor.getXRegister(), processor.getYRegister(),
        processor.getStatusRegister(), processor.getStackPointer() };
    h = fnv(h, &cycle, sizeof cycle);
    h = fnv(h, regs, sizeof regs);
    std::vector<uint8_t> view(0x10000, 0);
    mem.snapshotCpuView(view.data());
    return fnv(h, view.data(), view.size());
}

EmulationController::InputJournalMode EmulationController::inputJournalMode() const
{
    switch (inputJournal_->mode.load(std::memory_order_acquire)) {
    case kRecording: return InputJournalMode::Recording;
    case kPlaying:   return InputJournalMode::Playing;
    default:         return InputJournalMode::Off;
    }
}

std::string EmulationController::inputJournalStatus() const
{
    std::lock_guard<std::mutex> lk(inputJournal_->statusMtx);
    return inputJournal_->status;
}

const char* EmulationController::inputRecordingRefusalLocked() const
{
    if (inputJournal_->mode.load() != kOff)
        return "A recording or a playback is already running.";
    if (inputRecordingWaiting_.load())
        return "The last recording has not been saved yet.";
    if (scrubIndex_.load() != pom2::RewindBuffer::kNoFrame)
        return "The Rewind panel is scrubbing: press Play there first.";
    if (cardHoldsHostConnection())
        return "A card is connected to the outside (a network socket, a serial peer\n"
               "with bytes waiting): what it receives cannot be replayed.";
    if (mem.pendingPasteSize() != 0)
        return "A paste is being typed into the machine: wait for it to finish.";
    if (tape && tape->isPlaybackActive())
        return "A cassette is playing: stop the deck first.";
    for (int s = 0; s < SlotBus::kSlotCount; ++s) {
        const SlotPeripheral* card = mem.slotBus().peripheral(s);
        if (card && card->readsHostPadsItself())
            return "A 4play or SNES MAX card reads the host's game pads itself:\n"
                   "its input is not recorded.";
    }
    return nullptr;
}

bool EmulationController::beginInputRecordingLocked(std::string& error)
{
    if (const char* why = inputRecordingRefusalLocked()) { error = why; return false; }
    InputJournalState& j = *inputJournal_;
    const uint64_t now = mem.getCycleCounter();
    {
        std::lock_guard<std::mutex> lk(j.mtx);
        j.pending.clear();
        j.level.clear();
    }
    j.entries.clear();
    j.next = 0; j.applied = 0; j.checks = 0;
    j.endWhy.clear();
    j.exchangeAtStart = pom2::hostExchangeEpoch().load(std::memory_order_relaxed);
    // The recording's own clock starts on the host's, in its time zone.
    const std::time_t host = std::time(nullptr);
    j.epoch = static_cast<int64_t>(host);
    j.utcOffset = pom2::utcOffsetSeconds(host);
    j.epochCycle = now;
    j.epochHz = static_cast<double>(pom2VideoTiming(videoStandard_.load()).cpuClockHz);
    j.memory = &mem;
    pom2::TimedInput start;
    start.cycle = now;
    start.event.kind = pom2::InputEvent::Kind::Start;
    start.event.hash = machineFingerprintLocked();
    start.event.when = j.epoch;
    start.event.a = static_cast<int32_t>(j.utcOffset);
    j.entries.push_back(start);
    // The clock as it runs now: a playback starts from a session that
    // restores the machine's model, not an accelerator's speed of the moment.
    pom2::TimedInput clock;
    clock.cycle = now;
    clock.event.kind = pom2::InputEvent::Kind::Clock;
    clock.event.hz = cpuClockHz_.load();
    clock.event.stockHz = stockClockHz();
    clock.event.machineHz = machineCpuClockHz_.load();
    clock.event.ratio = 1.0 / mem.videoClockRatio();
    j.entries.push_back(clock);
    j.nextCheckpoint = now + static_cast<uint64_t>(kCheckpointFrames) *
        static_cast<uint64_t>(pom2VideoTiming(videoStandard_.load()).cyclesPerFrame);
    pom2::setHostTimeOverride(&journalTime, &j);
    j.mode.store(kRecording, std::memory_order_release);
    mem.setInputTap(&j);
    j.setStatus("Recording host input");
    pom2::log().info("Input", "recording host input from cycle " + std::to_string(now));
    return true;
}

void EmulationController::endInputJournalLocked(const char* why)
{
    InputJournalState& j = *inputJournal_;
    const int m = j.mode.load();
    if (m == kOff) return;
    const uint64_t now = mem.getCycleCounter();
    if (m == kRecording) {
        // What the user did last still reaches the machine — and the record.
        std::vector<pom2::InputEvent> take;
        { std::lock_guard<std::mutex> lk(j.mtx); take.swap(j.pending); }
        j.mode.store(kOff, std::memory_order_release);   // applyJournalInputLocked applies, not taps
        mem.setInputTap(nullptr);
        for (const pom2::InputEvent& e : take) {
            applyJournalInputLocked(e);
            j.entries.push_back({now, e});
        }
        pom2::TimedInput check;
        check.cycle = now;
        check.event.kind = pom2::InputEvent::Kind::Checkpoint;
        check.event.hash = machineFingerprintLocked();
        j.entries.push_back(check);
        pom2::TimedInput end;
        end.cycle = now;
        end.event.kind = pom2::InputEvent::Kind::End;
        end.event.text = why ? why : "";
        j.entries.push_back(end);
        j.endWhy = end.event.text;
        inputRecordingWaiting_.store(true);
        j.setStatus(std::string("Recording ended: ") + j.endWhy);
    } else {
        j.mode.store(kOff, std::memory_order_release);
        mem.setInputTap(nullptr);
        j.setStatus(std::string("Playback ended: ") + (why ? why : ""));
    }
    pom2::setHostTimeOverride(nullptr, nullptr);
    pom2::log().info("Input", std::string(m == kRecording ? "recording" : "playback") +
                              " ended at cycle " + std::to_string(now) + ": " + (why ? why : ""));
}

std::vector<pom2::TimedInput> EmulationController::takeInputRecording(std::string& why)
{
    std::lock_guard<std::mutex> lk(stateMtx);
    InputJournalState& j = *inputJournal_;
    if (j.mode.load() == kRecording) endInputJournalLocked("stopped by the user");
    std::vector<pom2::TimedInput> out;
    if (inputRecordingWaiting_.exchange(false)) {
        out.swap(j.entries);
        why = j.endWhy;
    }
    return out;
}

bool EmulationController::beginInputPlaybackLocked(std::vector<pom2::TimedInput> entries, std::string& error)
{
    InputJournalState& j = *inputJournal_;
    if (j.mode.load() != kOff) { error = "A recording or a playback is already running."; return false; }
    if (entries.size() < 2 || entries[0].event.kind != pom2::InputEvent::Kind::Start) {
        error = "This recording has no start."; return false;
    }
    const uint64_t now = mem.getCycleCounter();
    if (entries[0].cycle != now || entries[0].event.hash != machineFingerprintLocked()) {
        error = "The machine does not stand where the recording starts (its session did not\n"
                "load the same machine).";
        return false;
    }
    j.entries = std::move(entries);
    j.next = 1;
    j.applied = 0; j.checks = 0;
    j.epoch = j.entries[0].event.when;
    j.utcOffset = j.entries[0].event.a;
    j.epochCycle = now;
    j.epochHz = static_cast<double>(pom2VideoTiming(videoStandard_.load()).cpuClockHz);
    j.memory = &mem;
    inputPlaybackDiverged_.store(false);
    pom2::setHostTimeOverride(&journalTime, &j);
    j.mode.store(kPlaying, std::memory_order_release);
    mem.setInputTap(&j);
    j.setStatus("Playing a recording");
    (void)inputSliceLocked(1);   // what falls on this cycle: the clock as it was
    return true;
}

void EmulationController::noteResetForInputJournalLocked(pom2::InputEvent::Kind kind)
{
    InputJournalState& j = *inputJournal_;
    const int m = j.mode.load();
    if (m == kRecording) {
        pom2::TimedInput t;
        t.cycle = mem.getCycleCounter();
        t.event.kind = kind;
        j.entries.push_back(t);
    } else if (m == kPlaying) {
        endInputJournalLocked("the machine was reset by hand");
    }
}

void EmulationController::noteClockForInputJournalLocked(double hz, double stockHz, double machineHz, double ratio)
{
    InputJournalState& j = *inputJournal_;
    if (j.mode.load() != kRecording) return;
    pom2::TimedInput t;
    t.cycle = mem.getCycleCounter();
    t.event.kind = pom2::InputEvent::Kind::Clock;
    t.event.hz = hz; t.event.stockHz = stockHz; t.event.machineHz = machineHz; t.event.ratio = ratio;
    j.entries.push_back(t);
}

void EmulationController::applyJournalInputLocked(const pom2::InputEvent& e)
{
    using K = pom2::InputEvent::Kind;
    switch (e.kind) {
    case K::Mouse: {
        const auto x = static_cast<uint8_t>(e.a), y = static_cast<uint8_t>(e.b);
        const bool button = e.c != 0;
        for (int slot = 1; slot < SlotBus::kSlotCount; ++slot)
            if (SlotPeripheral* p = mem.slotBus().peripheral(slot); p && p->takesHostMouse())
                p->routeHostMouse(x, y, button);
        break;
    }
    case K::SoftReset: softResetLocked(); break;
    case K::HardReset: hardResetLocked(); break;
    case K::ColdBoot:  coldBootLocked();  break;
    case K::Clock:     applyClockLocked(e.hz, e.stockHz, e.machineHz, e.ratio); break;
    case K::Start: case K::Checkpoint: case K::End: break;
    default:           mem.applyInput(e); break;
    }
}

int EmulationController::routeHostMouseLocked(uint8_t x, uint8_t y, bool button)
{
    int routed = 0;
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        const SlotPeripheral* p = mem.slotBus().peripheral(slot);
        if (p && p->takesHostMouse()) ++routed;
    }
    pom2::InputEvent e;
    e.kind = pom2::InputEvent::Kind::Mouse;
    e.a = x; e.b = y; e.c = button ? 1 : 0;
    if (routed == 0 || inputJournal_->takeInput(e)) return routed;
    applyJournalInputLocked(e);
    return routed;
}

int EmulationController::inputSliceLocked(int chunk)
{
    InputJournalState& j = *inputJournal_;
    const int m = j.mode.load(std::memory_order_relaxed);
    if (m == kOff) return chunk;
    const uint64_t now = mem.getCycleCounter();
    if (m == kRecording) {
        if (pom2::hostExchangeEpoch().load(std::memory_order_relaxed) != j.exchangeAtStart) {
            endInputJournalLocked("bytes came in from outside (a serial peer, the network): "
                                  "a playback could not reproduce them");
            return chunk;
        }
        std::vector<pom2::InputEvent> take;
        { std::lock_guard<std::mutex> lk(j.mtx); take.swap(j.pending); }
        for (const pom2::InputEvent& e : take) {
            applyJournalInputLocked(e);
            j.entries.push_back({now, e});
        }
        if (now >= j.nextCheckpoint) {
            pom2::TimedInput check;
            check.cycle = now;
            check.event.kind = pom2::InputEvent::Kind::Checkpoint;
            check.event.hash = machineFingerprintLocked();
            j.entries.push_back(check);
            j.nextCheckpoint = now + static_cast<uint64_t>(kCheckpointFrames) *
                static_cast<uint64_t>(pom2VideoTiming(videoStandard_.load()).cyclesPerFrame);
        }
        return chunk;
    }
    // Playing: what falls on this cycle, in the order it was recorded.
    while (j.next < j.entries.size() && j.entries[j.next].cycle == now) {
        const pom2::TimedInput& t = j.entries[j.next++];
        if (t.event.kind == pom2::InputEvent::Kind::Checkpoint) {
            if (t.event.hash != machineFingerprintLocked()) {
                inputPlaybackDiverged_.store(true);
                char why[160];
                std::snprintf(why, sizeof why, "the machine left the recording before cycle %llu "
                              "(%llu inputs played, %llu checks passed)",
                              static_cast<unsigned long long>(now),
                              static_cast<unsigned long long>(j.applied),
                              static_cast<unsigned long long>(j.checks));
                endInputJournalLocked(why);
                setMode(Mode::Stopped);
                return chunk;
            }
            ++j.checks;
        } else if (t.event.kind == pom2::InputEvent::Kind::End) {
            char why[160];
            std::snprintf(why, sizeof why, "finished - %llu inputs played, %llu checks passed",
                          static_cast<unsigned long long>(j.applied),
                          static_cast<unsigned long long>(j.checks));
            endInputJournalLocked(why);
            return chunk;
        } else {
            applyJournalInputLocked(t.event);
            if (t.event.kind != pom2::InputEvent::Kind::Clock) ++j.applied;
        }
    }
    if (j.next >= j.entries.size()) {
        endInputJournalLocked("finished (the recording has no end entry)");
        return chunk;
    }
    const uint64_t at = j.entries[j.next].cycle;
    if (at < now) {
        inputPlaybackDiverged_.store(true);
        char why[200];
        std::snprintf(why, sizeof why, "the machine left the recording before cycle %llu: no "
                      "instruction ended where the next input was made (%llu inputs played, "
                      "%llu checks passed)", static_cast<unsigned long long>(at),
                      static_cast<unsigned long long>(j.applied),
                      static_cast<unsigned long long>(j.checks));
        endInputJournalLocked(why);
        setMode(Mode::Stopped);
        return chunk;
    }
    return static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(chunk), at - now));
}
