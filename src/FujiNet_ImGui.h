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

// FujiNet_ImGui — panel for the FujiNet relay card: which transport to use,
// whether a peer is attached, what devices it offers, and the call counters.
//
// Same shape as the other card panels: pure data-in / actions-out. MainWindow
// owns the FujiNetCard and applies the requested changes under the state
// mutex, so this file never touches the link itself — which matters here more
// than usual, because reconfiguring the transport stops and restarts a worker
// thread.

#ifndef POM2_FUJINET_IMGUI_H
#define POM2_FUJINET_IMGUI_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class FujiNet_ImGui
{
public:
    enum class Transport { Off, Tcp, Serial };

    struct DeviceRow {
        uint8_t     unit    = 0;
        std::string name;
        uint8_t     type    = 0;
        uint8_t     subtype = 0;
        uint32_t    blocks  = 0;
    };

    struct Snapshot {
        bool        plugged   = false;
        int         slot      = 0;
        Transport   transport = Transport::Tcp;
        bool        running   = false;   ///< worker started
        bool        connected = false;   ///< a peer is actually attached
        std::string state;               ///< link->describe()
        std::string lastError;

        uint16_t    tcpPort    = 1985;
        std::string serialPath;
        int         serialBaud = 115200;
        int         timeoutMs  = 250;

        /// Candidates from SerialPort::enumerate(), refreshed on demand
        /// rather than every frame — scanning /dev on every frame would be
        /// silly, and the list only changes when hardware is plugged in.
        std::vector<std::pair<std::string, std::string>> serialDevices;

        std::vector<DeviceRow> devices;

        uint64_t calls = 0, timeouts = 0, stale = 0, bytesIn = 0, bytesOut = 0;
        uint64_t localCalls = 0;

        /// The peer offers a printer unit, so POM2's ImageWriter is being fed
        /// from it (phase 2 tap), and how much has gone through.
        bool     printerTap      = false;
        uint64_t printerBytes    = 0;
        /// A parallel printer card outranks the tap — the user should know
        /// why their FujiNet printout is not appearing on POM2's paper.
        bool     printerOutranked = false;
        /// POM2 also has its own ProDOS clock card plugged, so the guest sees
        /// two clocks.
        bool     hostClockCard   = false;

        /// Helper process: an external FujiNet program POM2 can start for the
        /// user instead of making them run it by hand.
        bool        helperRunning = false;
        std::string helperPath;        ///< configured, or "" for auto-detect
        std::string helperResolved;    ///< what auto-detect found, or ""
        int         helperExitCode = -1;
    };

    struct Result {
        bool        transportChanged = false;
        Transport   transportTo      = Transport::Tcp;

        bool        portChanged   = false;
        uint16_t    portTo        = 1985;
        bool        serialChanged = false;
        std::string serialPathTo;
        int         serialBaudTo  = 115200;
        bool        timeoutChanged = false;
        int         timeoutTo      = 250;

        bool requestStart      = false;
        bool requestStop       = false;
        bool requestDropPeer   = false;
        bool requestBootConfig = false;
        bool requestRescan     = false;   ///< re-enumerate serial devices
        bool requestOpenWebUi  = false;   ///< TCP mode only

        bool        helperPathChanged = false;
        std::string helperPathTo;
        bool        requestHelperStart = false;
        bool        requestHelperStop  = false;
        /// Stop the FujiNet program and start it again. Its own button
        /// because the peer can die without POM2 being at fault — the
        /// firmware aborts on device calls POM2 has no business withholding —
        /// and the fix is always the same two clicks. See DEV.md § FujiNet.
        bool        requestHelperRestart = false;
    };

    Result render(const char* title, bool& open, const Snapshot& snap);

private:
    // ImGui input buffers cannot live on the stack across frames.
    std::array<char, 256> serialPathBuf_{};
    bool                  serialPathPrimed_ = false;
    std::array<char, 512> helperPathBuf_{};
    bool                  helperPathPrimed_ = false;
    std::string           helperPathMirrored_;
};

} // namespace pom2

#endif // POM2_FUJINET_IMGUI_H
