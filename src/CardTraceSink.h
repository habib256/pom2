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

// Where a card writes a line about its own traffic — the Disk II's head
// moves, a SoftCard II's 6502 BIOS calls — for the OS-call trace
// (OsCallTrace.h, which implements it). Hung on a card only while that trace
// is on (`SlotBus::setCardTrace`, `Z80::setObserver`); unset, a card pays one
// pointer test where it would speak.

#ifndef POM2_CARD_TRACE_SINK_H
#define POM2_CARD_TRACE_SINK_H

namespace pom2 {

class CardTraceSink
{
public:
    virtual ~CardTraceSink() = default;
    virtual void onCardLine(const char* /*line*/) {}
};

} // namespace pom2

#endif // POM2_CARD_TRACE_SINK_H
