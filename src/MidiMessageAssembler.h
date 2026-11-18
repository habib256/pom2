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

// MidiMessageAssembler — a MIDI byte stream (what a MIDI OUT puts on the
// wire, one byte at a time) turned into whole messages, which is what host
// MIDI APIs and a Standard MIDI File take: Windows' midiOutShortMsg packs
// one message in a DWORD, a CoreMIDI packet should not split one, an SMF
// event is one.
//
// [B] MIDI 1.0 Detailed Specification (MMA), the parts used:
//   * a status byte has bit 7 set; channel messages carry 2 data bytes
//     (8n, 9n, An, Bn, En) or 1 (Cn, Dn);
//   * running status: data bytes after a channel message repeat its status;
//     any system common or SysEx status cancels it, real-time does not;
//   * real-time F8-FF are single bytes that may arrive anywhere, even
//     between the bytes of another message, which then continues;
//   * system common F1 (1 data), F2 (2), F3 (1), F6 (0), F7 (EOX);
//     F4 / F5 undefined (dropped);
//   * SysEx runs from F0 to F7; any status other than real-time ends it.
// The bytes delivered always carry their status (running status expanded).

#ifndef POM2_MIDI_MESSAGE_ASSEMBLER_H
#define POM2_MIDI_MESSAGE_ASSEMBLER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

struct MidiMessage {
    uint64_t cycle = 0;              ///< the stamp of the message's LAST byte
    std::vector<uint8_t> bytes;      ///< status first; SysEx from F0 to F7
};

class MidiMessageAssembler
{
public:
    /// SysEx longer than this is dropped whole (a runaway stream).
    static constexpr std::size_t kMaxSysEx = 64 * 1024;

    /// One byte off the wire; completed messages are appended to `out`.
    void feed(uint8_t b, uint64_t cycle, std::vector<MidiMessage>& out);
    void reset();

private:
    static int dataBytesFor(uint8_t status);

    uint8_t running_ = 0;            ///< channel status for running status, 0 = none
    uint8_t status_ = 0;             ///< status of the message being built
    int     need_ = 0;               ///< data bytes still expected
    std::vector<uint8_t> building_;
    bool    inSysEx_ = false;
    bool    sysExOverflow_ = false;
};

} // namespace pom2

#endif
