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

// HdvController_ImGui — ProDOS hard-disk (slot 5) status panel. Mirrors
// DiskController_ImGui in spirit: a Library list of .hdv / .2mg images
// that one-click "mount + cold boot" via $C500. Read-only by design — a
// future write-back path can plug into the same UI without changing the
// dispatch contract.

#ifndef POM2_HDV_CONTROLLER_IMGUI_H
#define POM2_HDV_CONTROLLER_IMGUI_H

#include <cstddef>
#include <string>
#include <vector>

namespace pom2 {

class HdvController_ImGui
{
public:
    struct LibraryEntry {
        std::string displayName;   // e.g. "Total Replay v6.1.hdv"
        std::string fullPath;      // e.g. "../floppyemu/Total Replay v6.1.hdv"
    };

    struct DriveSnapshot {
        bool        imageLoaded = false;
        std::string imagePath;
        size_t      blockCount  = 0;
        bool        writeBackEnabled  = true;    // process default, not a UI toggle
        bool        writeProtected    = false;   // the image's own lock: 2IMG flag or the notch
        bool        hasUnsavedChanges = false;
        bool        supportsWriteBack = false;
        std::string persistenceState, persistenceError;
        bool        isSynthVolume     = false;
        std::vector<LibraryEntry> library;
    };

    struct FrameResult {
        bool        requestEject       = false;
        bool        requestBoot        = false;     // jump PC to $C500
        // Single-click library entry: host mounts the image AND triggers
        // a cold boot through the slot-5 ROM.
        std::string requestMountAndBoot;
        // Right-click library entry: mount the image WITHOUT booting —
        // hot-swap on a running system.
        std::string requestMountOnly;
        bool        writeBackToggleChanged = false;
        bool        writeBackNewValue      = false;
    };

    /// Draw the panel. Called every frame from MainWindow::render().
    /// Returns user actions for the host to apply (mount dialog goes
    /// through MainWindow so the file path widget is in one place).
    FrameResult render(const char*          title,
                       bool&                open,
                       const DriveSnapshot& snap);

    // Mount-dialog state lives in the panel (same rationale as
    // DiskController_ImGui's insert dialog). Menu-bar "Mount HDV…"
    // shortcut flips `mountDialogOpen` directly.
    bool        mountDialogOpen = false;
    std::string dialogPath;
};

} // namespace pom2

#endif // POM2_HDV_CONTROLLER_IMGUI_H
