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

// Disk35Controller_ImGui — //c+ Sony 3.5" drives status panel. Same UX as
// `DiskController_ImGui` for the 5.25" Disk II, scaled up to two drives
// (internal + external) stacked vertically:
//
//   • Drive header with motor LED (red when spinning).
//   • Inserted image path, track, head, write-protect / write-back state.
//   • Mount / Eject buttons + per-drive Write-back checkbox.
//   • Shared library list at the bottom — left-click mounts + cold-boots
//     into drive 1 (internal), right-click opens a context menu offering
//     mount-only and explicit drive selection. Currently-inserted images
//     are prefixed with `* ` so the user can spot a re-click.
//
// Read-only snapshot pattern: MainWindow builds the snapshot under
// `stateMutex` and the panel only sees a frozen view, returning user
// actions through `FrameResult` for the host to apply.

#ifndef POM2_DISK35_CONTROLLER_IMGUI_H
#define POM2_DISK35_CONTROLLER_IMGUI_H

#include <array>
#include <string>
#include <vector>

namespace pom2 {

class Disk35Controller_ImGui
{
public:
    struct LibraryEntry {
        std::string displayName;   // e.g. "ProDOS_2_4_2.po"
        std::string fullPath;      // e.g. "../disks_3.5/ProDOS_2_4_2.po"
    };

    struct DriveSnapshot {
        bool        diskLoaded         = false;
        bool        motorOn            = false;
        int         track              = 0;       // 0..79
        bool        side1              = false;   // false = head 0
        bool        writeProtected     = true;
        /// True when this drive holds a `.woz`. A 3.5" WOZ is ALWAYS
        /// write-protected — POM2 decodes Sony GCR but cannot encode it, so
        /// guest writes would have nothing to be folded back into. The panel
        /// says so in those words and offers the conversion instead of
        /// leaving the user to wonder which of the write-protect reasons
        /// applies to them.
        bool        isWoz              = false;
        /// Where "Convert to writable .po" would write, already de-duplicated
        /// against existing files. Empty when the drive holds no WOZ.
        std::string convertTargetPath;
        bool        writeBackEnabled   = true;   // process default (MediaWritePolicy.h), not a UI toggle
        bool        fileWriteProtected = false;  // the disk's own protection: tab / 2IMG lock / WOZ
        bool        hasUnsavedChanges  = false;   // a sector has been written
        std::string diskPath;
        std::string lastError;        // last failed mount, if any
    };

    struct PanelSnapshot {
        // Two drives — index 0 = internal (the //c+ on-board 3.5"),
        // index 1 = external (daisy-chained 3.5" #2).
        std::array<DriveSnapshot, 2> drives{};
        bool                         supportedByProfile = false;
        std::vector<LibraryEntry>    library;
    };

    struct FrameResult {
        // Per-drive eject request.
        std::array<bool, 2> requestEject{};
        // Per-drive write-back toggle (host wires through Disk35Image::
        // setWriteBackEnabled). When `requestWriteBackToggle[i]` is true,
        // apply `newWriteBack[i]`.
        std::array<bool, 2> requestWriteBackToggle{};
        std::array<bool, 2> newWriteBack{};
        // Library actions. `requestMountPath` non-empty → mount only into
        // `requestMountDrive`. `requestInsertAndBoot` non-empty → mount
        // into `insertAndBootDrive` AND trigger a host-side cold boot so
        // the //c+ ROM probes the SmartPort and boots the new image.
        std::string         requestMountPath;
        int                 requestMountDrive    = 0;
        std::string         requestInsertAndBoot;
        int                 insertAndBootDrive   = 0;
        // Mount-dialog open request. Host listens for this and pops the
        // modal at the end of the frame.
        bool                openMountDialog          = false;
        int                 openMountDialogForDrive  = 0;
        /// Convert this drive's read-only WOZ to a writable `.po` and mount
        /// the copy in its place. -1 = not requested.
        int                 requestConvertDrive      = -1;
    };

    FrameResult render(const char*          title,
                       bool&                open,
                       const PanelSnapshot& snap);

    // Mount-dialog state — same convention as the other panels. When
    // `mountDialogOpen` is true, MainWindow renders the popup and
    // resolves the chosen path against `mountDialogForDrive`.
    bool        mountDialogOpen      = false;
    int         mountDialogForDrive  = 0;
    std::string dialogPath;
};

} // namespace pom2

#endif // POM2_DISK35_CONTROLLER_IMGUI_H
