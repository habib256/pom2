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

// MidiFileWriter — a recording of what a MIDI OUT sent, as a Standard MIDI
// File (format 0, one track). Timing comes from the messages' emulated CPU
// cycles, never from the wall clock: a recording made under the speed
// menu's x4, or while the window was frozen, has the tempo the program
// played at, and two runs of one program give the same file.
//
// [B] Standard MIDI Files 1.0 (MMA): MThd (format 0, 1 track, division
// 480 ticks per quarter note), MTrk with variable-length delta times, a Set
// Tempo meta-event (FF 51 03, 500 000 µs per quarter: 960 ticks a second),
// channel messages as sent, SysEx as F0 <length> <data…F7>, End of Track
// (FF 2F 00). Real-time and system common messages have no place in an SMF
// and are left out.

#ifndef POM2_MIDI_FILE_WRITER_H
#define POM2_MIDI_FILE_WRITER_H

#include "MidiMessageAssembler.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pom2 {

class MidiFileWriter
{
public:
    static constexpr int kTicksPerQuarter = 480;
    static constexpr int kMicrosPerQuarter = 500000;   // 120 bpm
    static constexpr double kTicksPerSecond = 960.0;

    /// Start a recording whose time zero is `startCycle`, at `cpuHz`.
    void start(uint64_t startCycle, double cpuHz);
    bool recording() const { return recording_; }
    void add(const MidiMessage& m);
    /// Events kept so far (channel messages + SysEx).
    std::size_t events() const { return events_; }

    /// The whole file as bytes (End of Track included).
    std::vector<uint8_t> bytes() const;
    /// Stop and write the file atomically. False with `error` set.
    bool finish(const std::filesystem::path& path, std::string& error);
    void discard();

private:
    void putVarLen(uint32_t v);

    bool recording_ = false;
    uint64_t startCycle_ = 0;
    double cpuHz_ = 1022727.0;
    uint64_t lastTick_ = 0;
    std::vector<uint8_t> track_;     ///< events after the tempo, no End of Track
    std::size_t events_ = 0;
};

} // namespace pom2

#endif
