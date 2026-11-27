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

// HostIoEpoch — "the guest just exchanged something with the outside world".
//
// The debugger's Step Back and Run Back re-execute the guest's code from a
// rewind frame (EmulationController_TimeTravel.cpp). Inside the machine that
// is harmless. But a byte the guest SENT to a serial peer, a network or a
// MIDI port would be sent again, and one it RECEIVED would be taken from the
// host a second time — the peer cannot be rewound. So the cards with a live
// host side bump this counter at the point where the guest's own access
// makes the exchange, and no replay crosses a bump: the controller turns it
// into a replay barrier (EmulationController.h, replayBarriers_ and the
// rule beside hostIoSeen_).
//
// It is NOT the media-write epoch (Block512Backing.h): that one empties the
// rewind ring, because a rewind of any kind must not cross a write to a
// file. An ordinary rewind scrub over a serial session is the user's call,
// as it always was; only the two verbs that RE-EXECUTE are fenced here.
//
// Who bumps: SuperSerialCard (a byte transmitted, a received byte taken),
// PassportMidiCard (a byte sent), the Uthernet I's CS8900A (a frame sent, a
// received frame taken), the Uthernet II's W5100 (a socket command) and
// FujiNetCard (a call to the device). NOT a register read or an address
// write: a network driver polls its chip every frame, and counting that
// closed the way back for the whole session. Printers already count as
// irreversible media (CLAUDE.md).
//
// The same counter takes host INPUT, for the mirror reason: a key reaching
// the keyboard latch, a held-key change, a pasted character taken, a paddle
// or button value changing, a mouse card's counters moving. A replay reads
// those as they stand NOW, not as they stood during the frame, and so would
// take another path than the original run did. Reported by Keyboard,
// PaddleInputs and the three mouse devices — on a CHANGE only, and never
// from a state being restored.

#ifndef POM2_HOST_IO_EPOCH_H
#define POM2_HOST_IO_EPOCH_H

#include <atomic>
#include <cstdint>

namespace pom2 {

inline std::atomic<uint64_t>& hostIoEpoch()
{
    static std::atomic<uint64_t> epoch{0};
    return epoch;
}

/// Call from the guest's access that exchanges a byte with the host.
inline void noteHostIo() { hostIoEpoch().fetch_add(1, std::memory_order_relaxed); }

/// Bytes that came IN from a peer (a serial line, the network, FujiNet):
/// they also count as host I/O above, and a recording of host input
/// (EmulationController_Input.cpp) cannot replay them — it ends there.
/// Keys, the mouse and the game port are not this: a recording keeps them.
inline std::atomic<uint64_t>& hostExchangeEpoch()
{
    static std::atomic<uint64_t> epoch{0};
    return epoch;
}

inline void noteHostExchange()
{
    hostExchangeEpoch().fetch_add(1, std::memory_order_relaxed);
    noteHostIo();
}

} // namespace pom2

#endif // POM2_HOST_IO_EPOCH_H
