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
// (https://github.com/Abdess/retrobios, bios/Apple/Apple II plus MAME's
// a2mouse romset under bios/Arcade/MAME).
//
// Since RetroBIOS PR #75 (merged 2026-09-14) the collection covers every
// dump POM2 probes and can legally point a user at: the //c / //c+
// firmware, the Liron and Workstation Card images, the ThunderClock+,
// CFFA 65C02 and TransWarp EPROMs, and the European character
// generators. What it still cannot serve is alternate SPELLINGS of names
// that are served (`342-0135-b.64.rom` and `341-0265-a.chr.rom` behind
// the //e Unenhanced pair, ClockCard's markadev filename, the
// TransWarp's) plus one real gap: the FR-Canadian UNENHANCED character
// ROM, which has no upstream copy. Every destRel maps onto a name in
// SystemProfile / RomCatalog / CharRomCatalog, so a missing file here is
// a missing file the ROM Status panel already knows about. A file that is
// present AND is the expected dump is left alone. One that is present but
// is not — a damaged dump, or a variant POM2 has no digest for; it cannot
// tell them apart — is replaced, after the old file is copied to
// userDataDir()/roms-replaced/. If that copy fails, nothing is replaced.
//
// Host-side only. HTTPS goes through the system `curl` (and `unzip` /
// `tar` for the two MAME zips left) so POM2 does not grow a TLS
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
    /// Single zip member to extract. Null when `url` is already the dump —
    /// which, since RetroBIOS PR #75 published loose copies, is every entry
    /// but two: the mouse slot eprom and the unenhanced //e firmware live
    /// only inside MAME romsets (a2mouse.zip, apple2e.zip).
    const char* zipMember;
    /// Null-terminated extra members concatenated AFTER `zipMember`, in
    /// order. The unenhanced //e is the case: MAME keeps that firmware as
    /// two 8 KB chips and POM2 probes the single 16 KB image, which is
    /// 342-0135-B ($C000-$DFFF) followed by 342-0134-A ($E000-$FFFF).
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
    /// The II+ firmware is the case that needs it, now the other way round:
    /// RetroBIOS publishes the same 20 KB dump POM2 ships (4 KB pad + the
    /// $C000-$FFFF firmware), so that is what a fresh fetch must match — but
    /// a tree holding the 12 KB six-chip image POM2 used to assemble out of
    /// apple2p.zip has a legitimate II+ firmware too and must not be
    /// overwritten. Without this field the "present is not the same as
    /// CORRECT" re-check in romsToFetch() calls a working ROM missing on
    /// every launch, and "Download missing ROMs" replaces it (bug hunt #10).
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

/// Where a download goes: `userDataDir()/roms` whenever the data dir is
/// per-user (it is then the FIRST search root, so what lands there is what
/// the machine loads), else the first writable `roms/` among the search roots
/// (the working directory included), else `userDataDir()/roms`. Never
/// creates anything — `fetchMissingRoms` does, when it runs. An installed
/// bundle is never written.
std::filesystem::path writableRomsDir();

/// Entries whose destRel does not resolve through findResource() — the
/// same "missing" the ROM Status panel reports. `present` overrides the
/// probe for tests.
std::vector<const RomFetchEntry*> romsToFetch(
    const std::function<bool(const char* destRel)>& present);

std::vector<const RomFetchEntry*> romsToFetch();

/// Install downloaded bytes at `dest` the way a fetch does: the entry's
/// size / CRC32 / SHA-256 gates first, then — if `dest` already exists — a
/// copy of the old file into userDataDir()/roms-replaced/ (its path comes
/// back in `backupOut`), then the atomic replace. Any failure refuses and
/// leaves `dest` untouched; a failed backup means no replacement at all.
bool installFetchedRom(const std::filesystem::path& dest,
                       const RomFetchEntry& entry,
                       const std::vector<std::uint8_t>& bytes,
                       std::string& err, std::string& backupOut);

/// The planner's verdict on a dump already on disk: true when `have` is the
/// dump the entry describes, or the entry's legitimate alternate
/// (`altPresentSize`). Exposed so the test can check the rule on synthetic
/// bytes without depending on which search root findResource hits first.
bool localDumpAcceptable(const RomFetchEntry& e, const std::vector<std::uint8_t>& have);

struct RomFetchResult {
    int         saved   = 0;
    int         skipped = 0;
    int         failed  = 0;
    /// Of `saved`: how many replaced a present file that was not the
    /// expected dump. The old one is kept under userDataDir()/roms-replaced.
    int         replaced = 0;
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

/// Download every entry `romsToFetch()` lists into `destRoot` (typically
/// writableRomsDir()): missing ones, and present ones that are not the
/// expected dump. A present file is never destroyed — it is copied to
/// userDataDir()/roms-replaced/ first, and left alone if that fails.
/// Safe to call from a worker thread — no ImGui, no emulator lock.
RomFetchResult fetchMissingRoms(const std::filesystem::path& destRoot,
                                const RomFetchProgress& progress = {},
                                const RomFetchCancel& cancelled = {});

}  // namespace pom2

#endif  // POM2_ROM_FETCH_H
