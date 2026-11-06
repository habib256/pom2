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

// Uthernet_ImGui — status panel for the two Ethernet cards. Read-only
// snapshot model, same shape as LeChatMauve_ImGui: MainWindow captures
// card state under EmulationController's stateMutex, hands the snapshot
// to render(), and dispatches the returned FrameResult back through the
// lock. Nothing here touches a card directly.
//
// The panel exists mostly to answer the question a user actually has when
// Ethernet does not work — "is the card there, is the host transport up,
// and is anything moving?" — so the backend identity and the per-socket
// table are the headline content, not the register dump.

#ifndef POM2_UTHERNET_IMGUI_H
#define POM2_UTHERNET_IMGUI_H

#include "W5100Device.h"

#include <array>
#include <cstdint>
#include <string>

namespace pom2 {

class Uthernet_ImGui
{
public:
    struct Snapshot {
        /// True when the build has libslirp. Drives the "why is there no
        /// transport" hint rather than leaving the user guessing.
        bool slirpCompiledIn = false;
        /// Settings value: "slirp" | "builtin" | "loopback" | "none".
        std::string backendChoice = "slirp";

        // ── Uthernet I (CS8900A) ──────────────────────────────────────
        bool                   u1Plugged      = false;
        int                    u1Slot         = 0;
        std::string            u1Backend      = "none";
        bool                   u1BackendValid = false;
        std::array<uint8_t, 6> u1Mac{};
        bool                   u1RxEnabled    = false;
        bool                   u1TxEnabled    = false;
        bool                   u1Promiscuous  = false;
        uint16_t               u1PacketPagePtr = 0;
        std::size_t            u1Queued       = 0;
        uint64_t               u1FramesSent     = 0;
        uint64_t               u1FramesReceived = 0;
        uint64_t               u1FramesFiltered = 0;

        // ── Uthernet II (W5100) ───────────────────────────────────────
        bool                   u2Plugged      = false;
        int                    u2Slot         = 0;
        std::string            u2Backend      = "none";
        bool                   u2BackendValid = false;
        std::array<uint8_t, 6> u2Mac{};
        uint32_t               u2Ip           = 0;   // network byte order
        bool                   u2VirtualDns   = true;
        uint64_t               u2BytesSent     = 0;
        uint64_t               u2BytesReceived = 0;
        std::array<W5100Device::SocketInfo, W5100Device::kSocketCount> u2Sockets{};
    };

    struct FrameResult {
        bool requestResetU1 = false;
        bool requestResetU2 = false;
        bool requestVirtualDns = false;
        bool virtualDnsTo     = true;
    };

    FrameResult render(const char* title, bool& open, const Snapshot& snap);
};

/// "02:55:31:00:00:01"
std::string formatMac(const std::array<uint8_t, 6>& mac);
/// "10.0.2.15" from an IPv4 address in network byte order.
std::string formatIpv4(uint32_t networkOrder);
/// Human name for a W5100 SN_SR value ("ESTABLISHED", "MACRAW", …).
const char* w5100StatusName(uint8_t status);
/// Human name for the protocol nibble of a W5100 SN_MR value.
const char* w5100ProtocolName(uint8_t mode);

} // namespace pom2

#endif // POM2_UTHERNET_IMGUI_H
