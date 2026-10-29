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

// RomImport — install a ROM the user HAS, under the name POM2 probes.
//
// The probe lists are exact filenames (`roms/apple2c-32Kv0.rom`,
// `roms/mouse_341-0270-c.bin`, …) and a dump found elsewhere almost never
// carries that name: MAME calls the same chip `341-0270-c.4b`, a forum calls
// it `AppleMouse.rom`, and a romset is a .zip. Renaming by hand is the step
// users got wrong, and dropping such a file on the window was refused as
// "not a disk image".
//
// So identify by CONTENT instead: every entry of `romFetchCatalog()` carries
// the SHA-256 of the dump POM2 vouches for, and a byte-identical file is that
// dump whatever it is called. A loose file is hashed as is; a zip is opened
// in-process (`ZipMember.h`) and every member hashed, plus the multi-chip
// images the catalogue assembles (the unenhanced //e's two 8 KB chips). A
// match goes through the same install gate as a download
// (`installFetchedRom`: size / CRC32 / SHA-256, the replaced file kept in
// roms-replaced/), into `writableRomsDir()`. A file that matches nothing is
// left alone and reported — POM2 never installs a dump it cannot vouch for.

#ifndef POM2_ROM_IMPORT_H
#define POM2_ROM_IMPORT_H

#include "RomFetch.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pom2 {

/// Largest loose file worth hashing as a ROM candidate. The biggest dump in
/// the catalogue is the 64 KB Workstation Card image; a disk image or a
/// photo dropped by mistake is not read at all.
constexpr std::size_t kMaxRomImportFileBytes = 256u * 1024u;

/// Every catalogue entry whose SHA-256 these bytes have — empty when none.
/// Usually one, but the catalogue names some dumps twice (the US //e
/// character generator is both `apple2e_char.rom` and `apple2e_char_us.rom`),
/// and an import installs each name the machine probes.
std::vector<const RomFetchEntry*> identifyRom(const std::vector<std::uint8_t>& bytes);

/// One dump found in the dropped files.
struct RomImportItem {
    std::string source;     ///< "file" or "archive.zip:member" it came from.
    std::string destRel;    ///< As probed: "roms/apple2e.rom".
    std::string label;      ///< Catalogue label.
    std::string installedAt;///< Absolute path written, empty when skipped.
    std::string backup;     ///< Where the replaced file went, if any.
    bool        alreadyPresent = false; ///< The probe already resolves to these bytes.
    std::string error;      ///< Non-empty when the install was refused.
};

struct RomImportResult {
    std::vector<RomImportItem> items;
    /// Inputs that held no catalogued dump (with the reason when unreadable).
    std::vector<std::string>   unrecognised;
    int installed = 0;
    int alreadyPresent = 0;
    int failed = 0;
    std::string summary;    ///< One line for a status bar / the panel.
};

/// True when `path` is worth handing to `importRoms`: a .zip, or a regular
/// file no larger than `kMaxRomImportFileBytes`. Cheap — one stat.
bool romImportCandidate(const std::filesystem::path& path);

/// Identify every input and install each match into `destRoot` (normally
/// `writableRomsDir()`). The same dump found twice is installed once. Safe
/// off the UI thread; touches no emulator state.
RomImportResult importRoms(const std::vector<std::filesystem::path>& inputs,
                           const std::filesystem::path& destRoot);

}  // namespace pom2

#endif  // POM2_ROM_IMPORT_H
