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

// MidiOutputCoordinator — the host side of the Passport MIDI card: takes
// the bytes the card sent (stamped in emulated CPU cycles), assembles them
// into messages, records them to a .mid on request, and plays them through
// a HostMidiPort on their own thread, on time.
//
// Timing. The UI thread drains the card once a frame, so bytes arrive in
// 16-20 ms bursts; sending them as they come would quantise every note to
// the frame. The sender thread instead maps each message's CPU cycle to a
// wall-clock instant — an anchor (cycle, instant) taken at the first
// message, `kLatency` ahead of now, advanced at the paced CPU clock — and
// waits for it: the jitter a listener hears is the OS thread wake-up, not
// the frame. Fast-forward (x4, MAX) or a pause would pull the schedule away
// from the wall: a message more than `kMaxAhead` early or `kMaxLate` late
// re-anchors there, so the music catches up instead of piling up or
// lagging for ever. Emulated time is the `emuCycles` rule (CLAUDE.md): the
// .mid recording never sees the wall clock at all.
//
// Stuck notes. When the card goes away (unplugged, profile switch) or the
// port changes, every channel gets All Notes Off (CC 123) and All Sound Off
// (CC 120) — a note-on whose note-off will never come otherwise rings on.

#ifndef POM2_MIDI_OUTPUT_COORDINATOR_H
#define POM2_MIDI_OUTPUT_COORDINATOR_H

#include "MidiFileWriter.h"
#include "MidiMessageAssembler.h"
#include "PassportMidiCard.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pom2 {

class MidiOutputCoordinator
{
public:
    static constexpr std::chrono::milliseconds kLatency{30};
    static constexpr std::chrono::milliseconds kMaxAhead{500};
    static constexpr std::chrono::milliseconds kMaxLate{150};

    MidiOutputCoordinator();
    ~MidiOutputCoordinator();
    MidiOutputCoordinator(const MidiOutputCoordinator&) = delete;
    MidiOutputCoordinator& operator=(const MidiOutputCoordinator&) = delete;

    /// One frame's drain. `cardPresent` false: no Passport is plugged (the
    /// bytes are then empty); a present -> absent edge silences the port.
    /// `cpuHz` is the paced CPU clock the stamps are counted at.
    void feed(std::vector<MidiOutByte>& bytes, bool cardPresent, double cpuHz);

    /// Choose the host output ("" = none). The open happens on the sender
    /// thread; the result shows in status().
    void setPort(const std::string& name);
    std::string portName() const;
    std::string status() const;

    /// .mid recording: starts at the next byte's cycle.
    void startRecording();
    bool recording() const;
    std::size_t recordedEvents() const;
    /// Write and stop. False with `error`.
    bool stopRecording(const std::filesystem::path& path, std::string& error);
    void discardRecording();

    uint64_t messagesSent() const;

private:
    struct Timed { MidiMessage msg; double cpuHz; };
    void run();

    MidiMessageAssembler assembler_;          // UI thread only
    std::vector<MidiMessage> assembled_;      // UI thread scratch
    bool cardWasPresent_ = false;             // UI thread only

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Timed> queue_;
    std::string wantPort_, openPort_, status_;
    bool reopen_ = false, panic_ = false, quit_ = false;
    MidiFileWriter recorder_;
    bool recordPending_ = false;
    uint64_t sent_ = 0;
    std::thread thread_;
};

} // namespace pom2

#endif
