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

// HostFileTag — the ONE convention by which a host file name carries an
// Apple II file's type and aux value, so a file can leave a guest volume
// and come back with its type, and a BLOAD needs no `,A` override.
//
//     NAME[#TTAAAA]        TT = ProDOS file type, AAAA = aux / load address
//
// CiderPress / SD-CARD-OS metadata tag; POM2's HGR Paint editor saves pages
// as `PIC#062000` (BIN at $2000). The tag is used only when an extension
// cannot carry the same (type, aux) pair back — `hostNameForType` decides.
//
// **This convention outlives every other choice here**: files already
// exported must keep importing faithfully, so it does not change.
//
// It lives in its own file because three POM2 paths exchange files with the
// host and all three must spell a type the same way — the ProDOS host folder
// (`ProDOSVolume.cpp`, which owned this code privately until 2026-11-01),
// `pom2_dos33` and `pom2_cpm`. The hazard is the one `SettingRebuild.h`
// records: policy that lives at one call site drifts at the others. The
// lesson the ProDOS decode learned the hard way is pinned in
// `hostNameForType`'s comment — composing an extension regardless made every
// untyped type `.bin` and every aux 0, and a host `PIC#062000` came back as
// a SECOND file `PIC.bin` on every flush.

#ifndef POM2_HOST_FILE_TAG_H
#define POM2_HOST_FILE_TAG_H

#include <cstdint>
#include <string>

namespace pom2 {

// ---------------------------------------------------------------- the tag

/// `#TTAAAA` for (type, aux) — always 7 characters.
std::string hostFileTag(std::uint8_t type, std::uint16_t aux);

/// Strips a `#TTAAAA` tag off `name`, filling `type` / `aux`. False and both
/// untouched when the name carries none.
bool stripHostFileTag(std::string& name, std::uint8_t& type, std::uint16_t& aux);

// ------------------------------------------------- extensions and types

/// ProDOS file type an extension names: `.bas` $FC, `.bin` $06, `.sys` $FF,
/// `.txt` $04, `.int` $FA. An extensionless name is typeless ($00) so the
/// decode adds NO extension and the name round-trips verbatim; any other
/// extension stays in the Apple name, so its type is informational → BIN.
std::uint8_t prodosTypeFromExtension(const std::string& ext);

/// Inverse, for the types an extension can name. `""` for typeless ($00),
/// nullptr when no extension names the type (so the tag must carry it).
const char* extensionForProdosType(std::uint8_t type);

/// Length of the well-known extension `name` ends in, 0 when none. These are
/// stripped off a host name so they don't eat into the Apple name's budget.
std::size_t strippedExtensionLength(const std::string& name);

/// The host file name for an Apple file called `name` of (type, aux):
/// its extension when the parse above would derive exactly this pair back
/// from it, else the `#TTAAAA` tag. Lossless in both directions.
std::string hostNameForType(const std::string& name, std::uint8_t type,
                            std::uint16_t aux);

// --------------------------------------------------------- DOS 3.3 types

/// ProDOS file type for a DOS 3.3 catalog type (lock bit already stripped).
std::uint8_t prodosTypeFromDos33(std::uint8_t dosType);

/// Inverse. False when no DOS 3.3 type maps to `prodosType`.
bool dos33TypeFromProdos(std::uint8_t prodosType, std::uint8_t& dosType);

} // namespace pom2

#endif // POM2_HOST_FILE_TAG_H
