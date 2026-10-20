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

// FloppyEmuDevice — a faithful model of the BMOW "Floppy Emu" disk emulator
// (bigmessowires.com) for the Apple II side. The real device is an SD-card +
// OLED + 3-button gadget that plugs into the disk port and *becomes* a
// 5.25"/3.5"/Smartport drive, with its own self-contained UI for picking
// images off the SD card. POM2 already emulates every drive type the Floppy
// Emu can present to the Apple II, so the value this class adds is the device's
// defining behaviour: the persistent emulation MODE (its NVRAM), the SD-card
// FILE EXPLORER, the FAVORITES (favdisks.txt) + automount directive, and the
// disk-swap workflow. The actual mounting is routed by MainWindow into the
// existing controller cards (DiskIICard / SmartPortCard units).
//
// This class is deliberately UI- and emulator-agnostic (no ImGui, no
// MainWindow, no SlotBus) so it can be unit-tested in isolation:
//   * format filtering per mode,
//   * SD-card directory navigation (bounded to the SD root),
//   * favdisks.txt parsing (automount directive + favorite paths).
//
// Reference: BMOW Floppy Emu Model C instruction manual, §3 (Disk Emulation
// Mode, Selecting a Disk Image, Favorites) and §5 (Apple II Usage).

#ifndef POM2_FLOPPY_EMU_DEVICE_H
#define POM2_FLOPPY_EMU_DEVICE_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

// The Apple II emulation modes the Floppy Emu offers. We model the four that
// map onto POM2's existing drives; the device's "Dual 5.25" and "Smartport
// Unit 2" (IIgs daisy-chain boot trick) modes are out of scope for v1.
enum class FloppyEmuMode {
    Disk525,      // "Apple II 5.25 Floppy"  — 140K, Disk II controller
    Disk35,       // "Apple II 3.5 Floppy"   — 800K dumb 3.5" (A9M0106-style)
    SmartportHD,  // "Smartport Hard Disk"   — up to 32MB ProDOS block device
    Unidisk35,    // "Unidisk 3.5"           — 800K smart, ejectable
};

class FloppyEmuDevice
{
public:
    struct Entry {
        std::string name;          // basename for display ("GAMES", "X.po")
        std::string fullPath;      // path to enter/mount; parent dir for `isUp`
        bool        isDir = false;
        bool        isUp  = false; // the ".." pseudo-entry
        uint64_t    sizeBytes = 0; // 0 for directories
    };

    struct Favorites {
        bool               present   = false;  // favdisks.txt exists
        int                automount = 0;      // 0 never, 1 first, 2 most-recent
        std::vector<Entry> entries;            // resolved favorite images
    };

    // ── Emulation mode (the device's NVRAM setting) ─────────────────────
    FloppyEmuMode mode() const { return mode_; }
    void          setMode(FloppyEmuMode m) { mode_ = m; }

    static const char* modeLabel(FloppyEmuMode m);  // OLED-faithful label
    static const char* modeKey  (FloppyEmuMode m);  // stable settings key
    static bool        modeFromKey(const std::string& key, FloppyEmuMode& out);
    static std::array<FloppyEmuMode, 4> allModes();

    // ── SD card (a host folder of disk images) ──────────────────────────
    const std::string& sdRoot() const { return sdRoot_; }
    /// Set the SD root and reset the browser to it. Empty path = no card.
    void setSdRoot(const std::string& path);
    /// True when the SD root directory exists on the host.
    bool sdPresent() const;

    // ── File Explorer (bounded to the SD root) ──────────────────────────
    const std::string& currentDir() const { return currentDir_; }
    bool atRoot() const;
    /// Directory listing for the current folder: a ".." pseudo-entry (unless
    /// at the SD root), then sub-directories, then files accepted by the
    /// current mode — directories first, each group sorted case-insensitively.
    std::vector<Entry> listing() const;
    /// Descend into a directory entry, or step up for the ".." entry.
    void enterDir(const Entry& e);
    /// Step up one level, never above the SD root.
    void goUp();

    /// True when `filename`'s extension is valid for the current mode
    /// (5.25 → dsk/do/po/nib/woz/2mg; 3.5/Unidisk → dsk/do/po/moof/woz/2mg;
    /// Smartport → po/hdv/2mg). Case-insensitive.
    bool acceptsFile(const std::string& filename) const;

    // ── Favorites (favdisks.txt in the SD root) ─────────────────────────
    /// Parse <sdRoot>/favdisks.txt: optional first meaningful "automount N"
    /// (N=0/1/2), then favorite paths; skip blank/comment lines. Paths resolve
    /// relative to the SD root and must remain inside it after canonicalization.
    /// The UI does not act on automount. Missing file → not present.
    Favorites favorites() const;

    /// Parse favdisks.txt CONTENT (for testing without touching the disk).
    /// `resolveBase` anchors relative favorite paths.
    static Favorites parseFavorites(const std::string& content,
                                    const std::string& resolveBase);

private:
    FloppyEmuMode mode_     = FloppyEmuMode::SmartportHD;
    std::string   sdRoot_;
    std::string   currentDir_;
};

} // namespace pom2

#endif // POM2_FLOPPY_EMU_DEVICE_H
