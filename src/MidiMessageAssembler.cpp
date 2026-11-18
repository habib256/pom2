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

// MIDI byte stream -> messages — see MidiMessageAssembler.h.

#include "MidiMessageAssembler.h"

namespace pom2 {

int MidiMessageAssembler::dataBytesFor(uint8_t status)
{
    switch (status & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
        case 0xC0: case 0xD0: return 1;
        default: break;
    }
    switch (status) {
        case 0xF1: case 0xF3: return 1;
        case 0xF2: return 2;
        default: return 0;
    }
}

void MidiMessageAssembler::reset()
{
    running_ = status_ = 0;
    need_ = 0;
    building_.clear();
    inSysEx_ = sysExOverflow_ = false;
}

void MidiMessageAssembler::feed(uint8_t b, uint64_t cycle, std::vector<MidiMessage>& out)
{
    if (b >= 0xF8) {                                   // real-time: on its own, anywhere
        if (b != 0xF9 && b != 0xFD) out.push_back({cycle, {b}});
        return;
    }
    if (b & 0x80) {
        if (inSysEx_) {                                // any status ends a SysEx
            if (!sysExOverflow_) {
                building_.push_back(0xF7);
                out.push_back({cycle, building_});
            }
            building_.clear();
            inSysEx_ = sysExOverflow_ = false;
            if (b == 0xF7) { running_ = 0; return; }
        }
        if (b == 0xF0) {
            running_ = 0;
            inSysEx_ = true;
            building_.assign(1, 0xF0);
            need_ = 0;
            status_ = 0;
            return;
        }
        if (b == 0xF4 || b == 0xF5 || b == 0xF7) {     // undefined, or a stray EOX
            running_ = 0; status_ = 0; need_ = 0; building_.clear();
            return;
        }
        status_ = b;
        running_ = b < 0xF0 ? b : 0;                   // system common cancels it
        need_ = dataBytesFor(b);
        building_.assign(1, b);
        if (need_ == 0) { out.push_back({cycle, building_}); building_.clear(); status_ = 0; }
        return;
    }
    // A data byte.
    if (inSysEx_) {
        if (building_.size() >= kMaxSysEx) sysExOverflow_ = true;
        else building_.push_back(b);
        return;
    }
    if (need_ == 0) {                                  // running status, or a stray byte
        if (!running_) return;
        status_ = running_;
        need_ = dataBytesFor(running_);
        building_.assign(1, running_);
    }
    building_.push_back(b);
    if (--need_ == 0) {
        out.push_back({cycle, building_});
        building_.clear();
        status_ = 0;
    }
}

} // namespace pom2
