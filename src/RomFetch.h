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

// RomFetch — pull the Apple II dumps POM2 actually probes from RetroBIOS
// (https://github.com/Abdess/retrobios, bios/Apple + a few MAME card
// romsets under bios/Arcade/MAME).
//
// The collection is not a complete POM2 romset: there is no //c / //c+
// firmware, no Liron, no TransWarp. What it does have maps onto the
// names in SystemProfile / RomCatalog / CharRomCatalog, so a missing
// file here is a missing file the ROM Status panel already knows about.
// Existing files are never overwritten — findResource() is the same
// probe the rest of the boot path uses.
//
// Host-side only. HTTPS goes through the system `curl` (and `unzip` /
// `tar` for the handful of MAME zips) so POM2 does not grow a TLS
// dependency. The browser build has no helper processes: the fetch
// returns a clear error and the panel greys the button.

#ifndef POM2_ROM_FETCH_H
#define POM2_ROM_FETCH_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pom2 {

/// One RetroBIOS object that becomes one file under roms/.
struct RomFetchEntry {
    const char* destRel;       ///< As probed everywhere else: "roms/apple2e.rom"
    const char* label;         ///< Shown in the panel while this item runs.
    std::size_t expectedSize;  ///< Reject the download on a mismatch. 0 = any.
    const char* url;           ///< raw.githubusercontent.com file or zip.
    /// Single zip member to extract. Null when `url` is already the dump.
    const char* zipMember;
    /// Null-terminated extra members concatenated AFTER `zipMember`, in
    /// order. Used for the II+ firmware (six 2 KB chips → one 12 KB image).
    const char* const* zipConcat;
    /// CRC32 (IEEE) of the dump POM2 vouches for, or 0 when there is no
    /// reference. A download that matches `expectedSize` and not this is a
    /// DIFFERENT file — a mirror serving another revision, or an error page
    /// padded to length — and installing it over the user's roms/ turns into
    /// "it doesn't boot" days later. Mirrors `RomCatalogEntry::knownCrc`,
    /// which cannot be included here: RomCatalog.h is a frontend header and
    /// this is a runtime one (cmake/Pom2Architecture.cmake).
    std::uint32_t expectedCrc;
    const char*   crcLabel;
};

/// Human-facing home of the collection. The panel quotes this; tests pin it.
constexpr const char* kRetroBiosSourceUrl =
    "https://github.com/Abdess/retrobios/tree/main/bios";

constexpr const char* kRetroBiosRawPrefix =
    "https://raw.githubusercontent.com/Abdess/retrobios/main/";

const std::vector<RomFetchEntry>& romFetchCatalog();

/// First writable `roms/` among the live search roots, else
/// `userDataDir()/roms` (created). Downloads land here so an installed
/// bundle is not written and a source-tree `roms/` is reused when it can be.
std::filesystem::path writableRomsDir();

/// Entries whose destRel does not resolve through findResource() — the
/// same "missing" the ROM Status panel reports. `present` overrides the
/// probe for tests.
std::vector<const RomFetchEntry*> romsToFetch(
    const std::function<bool(const char* destRel)>& present);

std::vector<const RomFetchEntry*> romsToFetch();

struct RomFetchResult {
    int         saved   = 0;
    int         skipped = 0;
    int         failed  = 0;
    std::string destDir;
    std::string error;     ///< Set when the run could not start (no curl, …).
    std::string summary;   ///< One line for the panel, always filled on return.
};

using RomFetchProgress = std::function<void(int done, int total,
                                            const char* label)>;

/// Polled between (and inside) downloads. Return true to abandon the run.
/// Exists because the fetch is a background thread the panel's destructor
/// JOINS: without a way to say stop, quitting mid-download blocked the whole
/// application behind curl's 90-second `--max-time`.
using RomFetchCancel = std::function<bool()>;

/// Download every missing catalog entry into `destRoot` (typically
/// writableRomsDir()). Never overwrites a destRel that findResource()
/// already resolves. Safe to call from a worker thread — no ImGui, no
/// emulator lock.
RomFetchResult fetchMissingRoms(const std::filesystem::path& destRoot,
                                const RomFetchProgress& progress = {},
                                const RomFetchCancel& cancelled = {});

}  // namespace pom2

#endif  // POM2_ROM_FETCH_H
