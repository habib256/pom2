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

// NetworkCoordinator — the host side of the FujiNet relay.
//
// SlotBus owns the card; this class resolves it under the machine lock,
// publishes an immutable snapshot for ImGui, and applies a frame's commands
// after re-resolving. Nothing SlotBus owns escapes a call.
//
// It also holds the host-only state that has no emulated counterpart: the
// serial-device scan, the helper program's configured and resolved paths, and
// the status line. Those belong together because they are all answers about
// the HOST, and none of them needs the machine lock.

#ifndef POM2_NETWORK_COORDINATOR_H
#define POM2_NETWORK_COORDINATOR_H

#include "FujiNet_ImGui.h"

#include <string>
#include <utility>
#include <vector>

class EmulationController;

namespace pom2 {

class NetworkCoordinator
{
public:
    /// One acquisition for the whole panel, with the card resolved from the
    /// live SlotBus inside it. The card, its link and its helper never leave.
    FujiNet_ImGui::Snapshot captureFujiNetPanel(
        EmulationController& controller) const;

    /// Apply a frame's requests. Everything that touches the card is done in
    /// ONE critical section after re-resolving it; the host-side halves (the
    /// serial scan, the helper program's path, the status line) are done
    /// outside it. The single exception is STOPPING the helper process, whose
    /// 2 s grace period would freeze the CPU worker and the paint thread with
    /// it — that one runs between two critical sections, with the card
    /// re-resolved for the second.
    void applyFujiNetPanel(EmulationController& controller,
                           const FujiNet_ImGui::Result& command);

    /// Enumerate host serial ports. Scans /dev (or the registry), so it is
    /// deliberately on demand and never under the machine lock.
    void rescanSerialDevices();

    const std::vector<std::pair<std::string, std::string>>&
    serialDevices() const noexcept { return serialDevices_; }

    const std::string& status() const noexcept { return status_; }

    const std::string& helperPath() const noexcept { return helperPath_; }
    const std::string& helperResolved() const noexcept
    { return helperResolved_; }
    /// Sets the configured path AND re-resolves it against PATH, because the
    /// two are one fact: a configured name the host cannot find is what the
    /// panel must show as unresolved.
    void setHelperPath(std::string path);

private:
    std::string status_;
    std::vector<std::pair<std::string, std::string>> serialDevices_;
    std::string helperPath_;
    std::string helperResolved_;
};

} // namespace pom2

#endif // POM2_NETWORK_COORDINATOR_H
