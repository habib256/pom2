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

// DiskController_ImGui — minimal Disk II status panel. Shows whether a
// boot PROM is loaded, whether a disk image is mounted, the current
// track / motor state, and exposes Insert / Eject buttons.
//
// Read-only by design (matching DiskIICard's first-cut). The panel reads
// snapshot state from DiskIICard via accessors under the caller's
// stateMutex — no direct emulator-thread interaction.

#ifndef POM2_DISK_CONTROLLER_IMGUI_H
#define POM2_DISK_CONTROLLER_IMGUI_H

#include <cstdint>
#include <string>
#include <vector>

class DiskIICard;
class EmulationController;

namespace pom2 {

class DiskController_ImGui
{
public:
    struct LibraryEntry {
        std::string displayName;   // e.g. "dos33_master.dsk"
        std::string fullPath;      // e.g. "../disks_5.4/dos33_master.dsk"
    };

    struct DriveSnapshot {
        bool        bootRomLoaded = false;
        bool        diskLoaded    = false;
        bool        motorOn       = false;
        int         track         = 0;
        int         halfTrack     = 0;
        int         trackPos      = 0;
        std::string diskPath;
        // Mirrors `DiskImage::getLastError()` for the active drive. Shown
        // in red under the slot when no disk is currently loaded and the
        // last insert attempt set an error. Lets the user see why a
        // refused-format / corrupt-2IMG / wrong-size file didn't mount,
        // instead of guessing.
        std::string lastError;
        bool        turboWhileMotor = true;   // user toggle, persisted by host
        bool        turboActive     = false;  // currently boosting?
        bool        writeBackEnabled = true; // writable by default; write-protect is the opt-out
        bool        hasUnsavedChanges = false;// track has been written
        /// Physical write-protect of the medium (WOZ INFO+2 / 2IMG flag).
        /// With `writeBackEnabled` this tells the panel WHY the guest sees
        /// a read-only disk — the notch on the sleeve, or the host toggle.
        bool        fileWriteProtected = false;
        // Disks library — populated by the host from the disks_5.4/ directory.
        // The panel shows them as a one-click "insert + cold boot" list.
        std::vector<LibraryEntry> library;
    };

    struct FrameResult {
        bool        requestEject        = false;
        bool        requestBoot         = false;     // jump PC to $C600 directly
        // Single-click library entry: host inserts the disk AND triggers
        // a cold boot so the Autostart's slot-6 probe runs the new image.
        std::string requestInsertAndBoot;
        // Right-click library entry: insert the disk WITHOUT booting —
        // hot-swap on a running system, e.g. multi-disk games.
        std::string requestInsertOnly;
        bool        turboToggleChanged  = false;
        bool        turboNewValue       = true;
        bool        writeBackToggleChanged = false;
        bool        writeBackNewValue      = false;
        std::string statusMessage;
    };

    /// Draw the panel. Called every frame from MainWindow::render().
    /// Returns user actions for the host to apply (eject + load dialog go
    /// through MainWindow so the file path widget is in one place).
    FrameResult render(const char*        title,
                       bool&              open,
                       const DriveSnapshot& snap);

    // Insert-dialog state lives in the panel rather than in MainWindow so
    // the panel owns its own UX surface. MainWindow reads/writes these
    // when rendering the modal popup (the popup itself stays in
    // MainWindow because it needs the DiskIICard pointer + state mutex).
    // Menu-bar "Insert disk…" shortcut flips `insertDialogOpen` directly.
    bool        insertDialogOpen = false;
    std::string dialogPath;
};

} // namespace pom2

#endif // POM2_DISK_CONTROLLER_IMGUI_H
