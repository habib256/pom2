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

// SoftCard2Protocol — the SoftCard II's "6502 BIOS calls" read off the
// mailbox, one line per call, for the OS-call trace.
//
// Under CP/M 2.28B the Z80 asks the 6502 for every disk sector, character
// and screen operation: a command byte, its parameters, then the 6502's
// reply, all through the Am2950. The 17 commands, their names (from
// Microsoft's manual) and their byte counts are sbabcoc's SoftCard2
// HARDWARE-REFERENCE.md "Command protocol", read off both ends of the disk's
// code and run one by one:
//
//    #  name       Z80 -> 6502                         6502 -> Z80
//    0  CALLSUB    address, A, X, Y                    A, X, Y, P
//    1  READMEM    address                             byte
//    2  WRITEMEM   address, value                      —
//    3  READSEC    track, drive, slot, sector          status (+256 if 0)
//    4  WRITESEC   track, drive, slot, sector, 256     status
//    5  READSLOT   slot                                byte
//    6  WRITESLOT  slot … character                    console status between
//    7  STATSLOT   slot                                $FF ready / $00
//    8  INITSLOT   slot                                —
//    9  WSTART     —                                   2560 bytes
//   10  FORMAT     slot, drive                         status
//   11  UPDATE     34 bytes                            —
//   12  BEEP       duration, period                    —
//   13  CLEAR      even, odd                           —
//   14  INVERT     —                                   —
//   15  SETPT1     address, XOR, AND                   —
//   16  SETPT2     address, XOR                        —
//
// The bootstrap's own transfers (the system image, the slot table) happen
// before any command and go 6502 -> Z80 only; an idle decoder ignores them.

#ifndef POM2_SOFTCARD2_PROTOCOL_H
#define POM2_SOFTCARD2_PROTOCOL_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pom2 {

class SoftCard2Protocol
{
public:
    using Sink = std::function<void(const std::string&)>;
    explicit SoftCard2Protocol(Sink sink = {}) : sink_(std::move(sink)) {}
    void setSink(Sink sink) { sink_ = std::move(sink); }

    /// A byte the Z80 put in the mailbox for the 6502.
    void z80Sent(uint8_t b);
    /// A byte the 6502 put in the mailbox for the Z80.
    void appleSent(uint8_t b);
    /// The Z80 was reset: any call in flight is gone.
    void reset() { cmd_ = -1; z80_.clear(); apple_.clear(); }

    static const char* commandName(int cmd);

private:
    void maybeFinish();
    void emit(const std::string& s) { if (sink_) sink_(s); }

    Sink sink_;
    int cmd_ = -1;                 // -1 = idle
    std::vector<uint8_t> z80_;     // parameters after the command byte
    std::vector<uint8_t> apple_;   // the reply so far
};

} // namespace pom2

#endif // POM2_SOFTCARD2_PROTOCOL_H
