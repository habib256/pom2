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
    /// Lowercase hex SHA-256 of the dump, or nullptr when POM2 has no
    /// reference for it. CRC32 is an ERROR detector: a 32-bit non-
    /// cryptographic checksum anyone can collide on purpose, so a mirror (or
    /// anything between it and here) could serve a chosen file that matches
    /// both the size and the CRC. SHA-256 is what makes the gate mean
    /// "this is the dump POM2 vouches for" rather than "this is not
    /// corrupted". Computed from the copies that ship in the repository's
    /// roms/ — the same dumps RetroBIOS serves. Verified before install.
    const char*   expectedSha256;
    /// A LOCAL file of this size also counts as "already present", even
    /// though a fresh download must still match `expectedSize`. 0 = none.
    /// The II+ firmware is the case that needs it: RetroBIOS serves the
    /// six-chip 12 KB image while the dump POM2 ships in roms/ is a 20 KB
    /// MAME pack (4 KB pad + the $C000-$FFFF firmware), and both boot.
    /// Without this the "present is not the same as CORRECT" re-check in
    /// romsToFetch() called the shipped, working ROM missing on every launch
    /// — so "Download missing ROMs" was never done, and clicking it
    /// overwrote a good dump with a different one (bug hunt #10).
    std::size_t   altPresentSize;
};

/// Human-facing home of the collection. The panel quotes this; tests pin it.
constexpr const char* kRetroBiosSourceUrl =
    "https://github.com/Abdess/retrobios/tree/main/bios";

constexpr const char* kRetroBiosRawPrefix =
    "https://raw.githubusercontent.com/Abdess/retrobios/main/";

const std::vector<RomFetchEntry>& romFetchCatalog();

/// Lowercase hex SHA-256 of `n` bytes at `data`. Exposed so the fetch test
/// can prove the catalog's digests describe the dumps that ship in roms/.
std::string sha256Hex(const std::uint8_t* data, std::size_t n);

/// The exact argv (after argv[0]) POM2 hands `curl` for one download.
/// Exposed so the test can pin the hardening flags: a `-fsSL` that silently
/// followed an HTTPS→HTTP redirect, or a download with no size ceiling, is
/// not something a comment can keep true.
std::vector<std::string> curlDownloadArgs(const std::string& url,
                                          const std::string& outPath);

/// Largest total DECOMPRESSED size POM2 will unpack out of a fetched zip.
constexpr std::size_t kMaxUnpackedZipBytes = 64u * 1024u * 1024u;

/// Sum the uncompressed sizes in a zip's central directory. Returns false
/// (with `err` set) when the archive is malformed or would expand past
/// `kMaxUnpackedZipBytes` — the zip-bomb gate, applied BEFORE `unzip` runs,
/// because afterwards the disk is already full.
bool zipUnpackedSizeWithinCap(const std::vector<std::uint8_t>& zip,
                              std::uintmax_t& totalOut,
                              std::string& err);

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
