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

// CharRomCatalog — list of character-generator ROMs the user can switch
// between at runtime via the toolbar dropdown.
//
// Apple shipped localised char ROMs for each export market (French, UK,
// German, French Canadian, …). On a Euro IIe the locale was selected by
// a physical switch under the keyboard that paged between two banks of
// the dual 8 KB EPROM; POM2 treats each locale as an independent 4 KB
// file (the "Single/" community split of the Dual-Euro dumps from
// downloads.reactivemicro.com — same content as the half of the 2764
// the switch selects).
//
// Entries with kProfileMaskII* control whether the choice appears in the
// dropdown for the active profile: a French //e ROM has no business
// being offered on a II+ profile (2 KB layout, no mousetext / lowercase).
// "Default" entries point to the profile's stock probe order so the user
// can always revert to "whatever the profile would have loaded on its
// own".

#ifndef POM2_CHAR_ROM_CATALOG_H
#define POM2_CHAR_ROM_CATALOG_H

#include <cstddef>

#include "SystemProfile.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

enum class CharRomLocale : uint8_t {
    ProfileDefault = 0,    // honour the profile's charRomProbeOrder

    // 2 KB classic II / II+ font (uppercase only, no lowercase, no
    // mousetext). Kept distinct from the IIe entries because loading
    // it on a IIe profile silently disables MouseText.
    AppleIIClassic,

    // Videx LOWER CASE CHIP (1980) — also 2 KB, also a II / II+ part, but
    // it DOES carry lowercase: it replaced the motherboard's character
    // generator so a II/II+ could display lowercase at all. The renderer
    // no longer folds a-z to A-Z on ROM size for exactly this reason; see
    // `Memory::charRomHasLowercase()`.
    VidexLowerCase,

    // 4 KB IIe-class ROMs (lowercase + mousetext when Enhanced).
    AppleIIeUS_Enhanced,
    AppleIIeUS_Unenhanced,
    AppleIIeFrench,
    AppleIIeFrenchCanadian,
    AppleIIeFrenchCanadianUnenhanced,
    AppleIIeUK_Enhanced,
    AppleIIeUK_Unenhanced,
    AppleIIeGerman,
    AppleIIeGermanImproved,

    // 8 KB INTERNATIONAL //e video ROM (342-0274-A), which carries TWO 4 KB
    // sets in one part — the real French //e switches between them. POM2
    // offers each bank as its own entry rather than modelling the switch:
    // bank 0 is the FR-CA set, bank 1 the US set (verified by CRC against
    // POM2's standalone 4 KB dumps; pinned by `char_rom_8k_bank`).
    AppleIIeFrench8k_FR,
    AppleIIeFrench8k_US,

    // French Touch custom character generator (8 KB EPROM, `eprom2164.bin`,
    // shipped with the "Block ASCII Anthology" demo). The Unenhanced //e the
    // French Touch corpus targets has no MouseText, so demos that draw block
    // art bring their OWN char set to burn into the video ROM socket. Bank 0
    // holds the block glyphs. Without it, the demo's block art renders as the
    // stock font's letters/brackets — the "garbled" screen users see.
    AppleIIeFrenchTouchBlock,
};

struct CharRomEntry {
    CharRomLocale locale;
    const char*   displayName;
    const char*   path;             // empty when locale == ProfileDefault
    bool          isIIeClass;       // hides II/II+ entries on IIe profiles
                                    // and vice-versa (a "ProfileDefault"
                                    // entry is shown for every profile)
    int           bank = 0;         // 4 KB bank inside an 8 KB two-set dump;
                                    // ignored for 2 KB / 4 KB parts
    /// The part's file size when it is not the class default (2 KB for a
    /// II/II+, 4 KB for a IIe-class). The 8 KB two-set dumps say so here, or
    /// the ROM Status panel paints a correct file red as "wrong size — a
    /// different file, not a variant" (bug hunt 2026-09-16). 0 = default.
    std::size_t   size = 0;
};

/// All entries the dropdown could ever display, in stable order.
const std::vector<CharRomEntry>& charRomCatalog();

/// The 4 KB bank to select inside an 8 KB two-set dump for this locale
/// (0 for every 2 KB / 4 KB part, and for ProfileDefault). Callers must pass
/// it to `Memory::loadCharRom`, or both banks of a two-set ROM would load as
/// bank 0 and the picker's two entries would draw the same glyphs.
int charRomBank(CharRomLocale l);

/// True when this entry is meaningful to offer for the active profile.
/// `ProfileDefault` is universal; II / II+ get only the classic 2 KB
/// option; everything else only sees the 4 KB IIe-class entries.
bool charRomFitsProfile(const CharRomEntry& e, SystemProfile p);

/// Resolve an enum tag to its catalog entry; falls back to
/// ProfileDefault when the tag is out of range (defensive against
/// stale settings files written by a future build).
const CharRomEntry& charRomEntry(CharRomLocale l);

/// String key used in settings.cfg so the choice survives restarts.
const char* charRomLocaleKey(CharRomLocale l);
CharRomLocale charRomLocaleFromKey(const std::string& key);

/// Resolve a catalog entry's file under the same path-prefix probe the
/// rest of the boot path uses (`roms/X`, `../roms/X`, `../../roms/X`)
/// — POM2 is normally launched from `build/`, where the bare `roms/...`
/// path the catalog stores doesn't exist. Returns the first existing
/// candidate, or an empty string when the file is missing entirely.
/// Pass `ProfileDefault` to get an empty string (caller should fall
/// back to the profile probe order).
std::string resolveCharRomPath(CharRomLocale l);
std::string resolveCharRomPath(const std::string& catalogPath);

}  // namespace pom2

#endif // POM2_CHAR_ROM_CATALOG_H
