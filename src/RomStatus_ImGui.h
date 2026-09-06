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

// RomStatus_ImGui — "which ROMs do I have, which am I missing, and what
// breaks without them".
//
// A dump that is absent, mis-named or the wrong variant is the common
// failure mode — and every symptom it produces
// (a profile that boots the wrong firmware, a card that silently refuses to
// plug, a Grappler+ that prints but isn't recognised by AppleWorks) shows up
// far from the cause. This panel puts the whole picture in one window:
// every ROM POM2 probes, in probe order, resolved against the live
// ResourcePaths search roots.
//
// Three sources feed it, and none is duplicated here:
//   * machine firmware + character generator — read from `profileConfig()`
//     for every profile, so a new profile appears with no edit;
//   * the hand-pickable locale fonts — `CharRomCatalog.h`, the list behind
//     View > Character set (a missing one only greys a dropdown entry, so
//     this is the one place the whole set is visible);
//   * peripheral cards — `RomCatalog.h`, which mirrors the per-card probe
//     lists at their plug sites.
//
// Host-side only: it stats files and hashes bytes, touches no emulator
// state, and takes no lock. Scanning is done on demand (open / Rescan),
// not per frame — hashing a dozen ROMs is cheap but not free.

#ifndef POM2_ROM_STATUS_IMGUI_H
#define POM2_ROM_STATUS_IMGUI_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pom2 {

class RomStatus_ImGui
{
public:
    RomStatus_ImGui() = default;
    ~RomStatus_ImGui();

    RomStatus_ImGui(const RomStatus_ImGui&)            = delete;
    RomStatus_ImGui& operator=(const RomStatus_ImGui&) = delete;

    /// Re-stat and re-hash everything. Called on first render and from the
    /// Rescan button; also worth calling after a profile switch, since the
    /// "used by the active profile" marks move.
    void rescan();

    /// Draw the window. `open` is the caller's show-flag (the title-bar
    /// close button clears it). `activeProfileName` is highlighted in the
    /// machine-firmware table so the user can see at a glance whether the
    /// machine they are running is on an exact dump or a fallback.
    void render(bool* open, const std::string& activeProfileName);

private:
    // ─── One probed file ────────────────────────────────────────────────
    struct FileState {
        FileState() = default;
        explicit FileState(std::string candidatePath)
            : candidate(std::move(candidatePath)) {}

        std::string   candidate;    ///< As written in the probe list.
        std::string   resolved;     ///< Absolute path, empty when absent.
        std::uint64_t size = 0;
        std::uint32_t crc  = 0;
        bool          found = false;
        bool          sizeOk = true;    ///< False only when a size is required.
        bool          crcKnown = false; ///< A reference CRC exists…
        bool          crcOk = false;    ///< …and it matches.
    };

    // A probe list (machine ROM for one profile, or one catalogue entry):
    // several candidates, at most one of which is the one actually used.
    struct Probe {
        std::string            group;
        std::string            name;
        std::string            note;      ///< What happens when it's absent.
        std::string            crcLabel;  ///< What the reference CRC means.
        std::size_t            requiredSize = 0;
        std::vector<FileState> files;
        int                    usedIndex = -1;   ///< First that resolved.
        /// True when the winner is not the first choice — i.e. POM2 is
        /// running on a substitute (the //e Unenhanced profile falling back
        /// to the Enhanced firmware, say), which is a fidelity warning
        /// rather than an error.
        bool                   fallback = false;
    };

    std::vector<Probe> machine_;   ///< Main firmware, one probe per profile.
    std::vector<Probe> charRom_;   ///< Character generators, ditto.
    /// Hand-pickable locale fonts (View > Character set), from
    /// `CharRomCatalog` — a different list from the per-profile probe
    /// order above, and the only place the whole set is visible.
    std::vector<Probe> locale_;
    std::vector<Probe> cards_;     ///< From RomCatalog.
    /// MAME's floppy_sound sample bank under roms/floppy_samples/. Not
    /// ROMs, but the same user-supplied-asset failure mode: missing means
    /// a mechanical sound silently never plays.
    std::vector<Probe> sounds_;
    std::vector<std::string> searchRoots_;
    std::string        romDir_;    ///< First search root that has a roms/.
    bool               scanned_ = false;
    int                missingRequired_ = 0;
    int                missingOptional_ = 0;

    // RetroBIOS fetch — worker owned by this panel. The UI thread only
    // reads the atomics / the mutex-guarded line; it never calls curl.
    std::thread        fetchThread_;
    std::atomic<bool>  fetchRunning_{false};
    std::atomic<bool>  fetchJoin_{false};
    /// Raised to abandon a run in progress. The destructor sets it before
    /// joining, so quitting mid-download costs one curl poll interval instead
    /// of waiting out its 90-second `--max-time`.
    std::atomic<bool>  fetchCancel_{false};
    std::atomic<int>   fetchDone_{0};
    std::atomic<int>   fetchTotal_{0};
    std::mutex         fetchMutex_;
    std::string        fetchLine_;
    std::string        fetchSummary_;

    void   startRetroBiosFetch();
    void   pollFetchJoin();
    void   scanProbe(Probe& p);
    void   renderTable(const char* id, std::vector<Probe>& rows,
                       const std::string& highlight);
    static std::uint32_t crc32File(const std::string& path,
                                   std::uint64_t& sizeOut);
};

}  // namespace pom2

#endif  // POM2_ROM_STATUS_IMGUI_H
