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

// DevicePanelCoordinator — the frontend's immutable view of slot devices.
//
// MainWindow used to cache non-owning card pointers and let menu/panel code
// dereference them directly.  That made profile/slot rebuilds a lifetime trap
// and spread stateMutex ownership across unrelated ImGui functions.  This
// coordinator resolves cards from SlotBus while holding lockState(), copies
// the state needed by one frame, and applies cheap mutations in a short
// critical section. Start/stop of the SSC telnet worker joins a thread
// and binds a socket, so those run after the lock is released — the same
// shape NetworkCoordinator uses for FujiNet. ImGui panels therefore know
// only Snapshot/FrameResult.

#ifndef POM2_DEVICE_PANEL_COORDINATOR_H
#define POM2_DEVICE_PANEL_COORDINATOR_H

#include "LeChatMauve_ImGui.h"
#include "Uthernet_ImGui.h"

#include <cstdint>
#include <string>
#include <vector>

class EmulationController;

namespace pom2 {

class Settings;

class DevicePanelCoordinator
{
public:
    struct InventorySnapshot {
        int chatMauveSlot = -1;
        int smartPortSlot = -1;
        int fujiNetSlot = -1;
        int uthernetSlot = -1;
        int uthernetIISlot = -1;
        int printerSlot = -1;
        int grapplerSlot = -1;
        int clockSlot = -1;
        bool grapplerRomLoaded = false;
        bool grapplerBusy = false;
        bool clockRomFromDump = false;
        std::vector<int> serialSlots;

        bool chatMauvePlugged() const noexcept { return chatMauveSlot >= 0; }
        bool smartPortPlugged() const noexcept { return smartPortSlot >= 0; }
        bool fujiNetPlugged() const noexcept { return fujiNetSlot >= 0; }
        bool serialPlugged() const noexcept { return !serialSlots.empty(); }
        bool printerPlugged() const noexcept { return printerSlot >= 0; }
        bool grapplerPlugged() const noexcept { return grapplerSlot >= 0; }
        bool clockPlugged() const noexcept { return clockSlot >= 0; }
        bool ethernetPlugged() const noexcept
        {
            return uthernetSlot >= 0 || uthernetIISlot >= 0;
        }
    };

    struct SerialSnapshot {
        int slot = -1;
        int port = 0;
        bool listening = false;
        bool connected = false;
        bool rawMode = false;
        bool printerTap = false;
        std::uint64_t bytesRx = 0;
        std::uint64_t bytesTx = 0;
        std::string recentRxText;
        std::string recentTxText;
    };

    struct SerialCommand {
        int slot = -1;
        std::uint16_t port = 0;
        bool requestStart = false;
        bool requestStop = false;
        bool requestRawMode = false;
        bool rawMode = false;
        bool requestPrinterTap = false;
        bool printerTap = false;

        bool empty() const noexcept
        {
            return !requestStart && !requestStop && !requestRawMode &&
                   !requestPrinterTap;
        }
    };

    struct SerialCommandResult {
        bool cardFound = false;
        bool startAttempted = false;
        bool startSucceeded = false;
    };

    DevicePanelCoordinator(EmulationController& controller, Settings& settings);

    /// Copy only topology information. Safe while the CPU worker runs and
    /// stable after the returned value leaves the critical section.
    InventorySnapshot captureInventory() const;

    Uthernet_ImGui::Snapshot captureEthernet() const;
    void applyEthernet(const Uthernet_ImGui::FrameResult& command);

    LeChatMauve_ImGui::Snapshot captureChatMauve() const;
    void applyChatMauve(const LeChatMauve_ImGui::FrameResult& command);

    std::vector<SerialSnapshot> captureSerialCards() const;
    SerialCommandResult applySerial(const SerialCommand& command);
    void persistSerial();

    /// CLI/configuration seam. Returns false when no matching card is live.
    bool setChatMauveInvertBit7(bool enabled);

private:
    EmulationController& controller_;
    Settings& settings_;
};

} // namespace pom2

#endif // POM2_DEVICE_PANEL_COORDINATOR_H
