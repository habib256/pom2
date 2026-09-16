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

#include "CharRomCatalog.h"
#include "ResourcePaths.h"

#include <filesystem>

namespace pom2 {

namespace {

const std::vector<CharRomEntry>& catalogStorage()
{
    // Order matters: the dropdown renders in this order so locales of
    // related origin (FR / FR-CA, UK enh / unenh, DE / DE improved)
    // group together visually.
    static const std::vector<CharRomEntry> all = {
        { CharRomLocale::ProfileDefault,
          "Default (per profile)",                 "",                                  /*isIIeClass=*/true },
        { CharRomLocale::ProfileDefault,
          "Default (per profile)",                 "",                                  /*isIIeClass=*/false },
        // The two ProfileDefault entries are functionally identical but
        // we duplicate them so the dropdown always offers at least one
        // entry for any profile — see charRomFitsProfile. They share a
        // tag so the persistence round-trip stays simple.

        // ── II / II+ (2 KB, uppercase only) ───────────────────────────
        { CharRomLocale::AppleIIClassic,
          "Apple ][/][+ — Classic (US, 2 KB)",     "roms/apple2_char.rom",              false },
        { CharRomLocale::VidexLowerCase,
          "Apple ][/][+ — Videx LOWER CASE CHIP",  "roms/Videx Lower Case Chip ROM.bin", false },

        // ── IIe-class (4 KB, with lowercase + mousetext when Enhanced)
        { CharRomLocale::AppleIIeUS_Enhanced,
          "//e/c — US Enhanced (342-0265-A)",      "roms/apple2e_char_us.rom",          true },
        { CharRomLocale::AppleIIeUS_Unenhanced,
          "//e — US Unenhanced (342-0133-A)",      "roms/apple2e_char_us_unenh.rom",    true },
        { CharRomLocale::AppleIIeFrench,
          "//e/c — Français (342-0274-A)",         "roms/apple2e_char_fr.rom",          true },
        { CharRomLocale::AppleIIeFrenchCanadian,
          "//e/c — FR Canadien Enhanced",          "roms/apple2e_char_frca.rom",        true },
        { CharRomLocale::AppleIIeFrenchCanadianUnenhanced,
          "//e — FR Canadien (341-0168-A)",        "roms/apple2e_char_frca_unenh.rom",  true },
        { CharRomLocale::AppleIIeUK_Enhanced,
          "//e/c — UK Enhanced (342-0273-A)",      "roms/apple2e_char_uk.rom",          true },
        { CharRomLocale::AppleIIeUK_Unenhanced,
          "//e — UK Unenhanced (341-0160-A)",      "roms/apple2e_char_uk_unenh.rom",    true },
        { CharRomLocale::AppleIIeGerman,
          "//e/c — Deutsch (341-0161-A)",          "roms/apple2e_char_de.rom",          true },
        { CharRomLocale::AppleIIeGermanImproved,
          "//e/c — Deutsch Improved",              "roms/apple2e_char_de_improved.rom", true },

        // The genuine 342-0274-A: one 8 KB part holding BOTH sets, as fitted
        // to the French //e (MAME `apple2eefr` gfx1). Offered as two entries
        // because POM2 selects a bank at load time instead of modelling the
        // machine's charset switch. Note the older "Français" entry above is
        // a 4 KB dump that is byte-identical to the FR-CA unenhanced one and
        // is NOT 342-0274-A, despite how it used to be labelled.
        { CharRomLocale::AppleIIeFrench8k_FR,
          "//e — Français 342-0274-A (banque FR)", "roms/342-0274-a.e9",                true, 0, 8192 },
        { CharRomLocale::AppleIIeFrench8k_US,
          "//e — Français 342-0274-A (banque US)", "roms/342-0274-a.e9",                true, 1, 8192 },
        { CharRomLocale::AppleIIeFrenchTouchBlock,
          "//e — French Touch (Block ASCII custom)", "roms/apple2e_char_ft_blockascii.rom", true, -1, 8192 },
    };
    return all;
}

}  // namespace

const std::vector<CharRomEntry>& charRomCatalog()
{
    return catalogStorage();
}

bool charRomFitsProfile(const CharRomEntry& e, SystemProfile p)
{
    // The profile drives the char ROM layout: II / II+ load a 2 KB file
    // (Memory::loadCharRom accepts either size, but IIe paging on a 2 KB
    // ROM gives no mousetext — and vice-versa the IIe 4 KB ROMs render
    // garbage on a II+ because the upper 2 KB are unused). So we strictly
    // partition the dropdown by profile family.
    const bool iieProfile =
        (p == SystemProfile::AppleIIe              ||
         p == SystemProfile::AppleIIeUnenhanced    ||
         p == SystemProfile::AppleIIc              ||
         p == SystemProfile::AppleIIcPlus          ||
         p == SystemProfile::AppleIIePAL           ||
         p == SystemProfile::AppleIIeUnenhancedPAL ||
         p == SystemProfile::AppleIIcPAL);

    if (e.locale == CharRomLocale::ProfileDefault) {
        // Show exactly one "Default" entry, whichever side matches.
        return e.isIIeClass == iieProfile;
    }
    return e.isIIeClass == iieProfile;
}

const CharRomEntry& charRomEntry(CharRomLocale l)
{
    for (const auto& e : catalogStorage()) {
        if (e.locale == l) return e;
    }
    // Defensive: stale enum value → first ProfileDefault entry.
    return catalogStorage().front();
}

const char* charRomLocaleKey(CharRomLocale l)
{
    switch (l) {
        case CharRomLocale::ProfileDefault:                      return "default";
        case CharRomLocale::AppleIIClassic:                      return "ii_classic";
        case CharRomLocale::VidexLowerCase:                      return "videx_lc";
        case CharRomLocale::AppleIIeUS_Enhanced:                 return "iie_us";
        case CharRomLocale::AppleIIeUS_Unenhanced:               return "iie_us_unenh";
        case CharRomLocale::AppleIIeFrench:                      return "iie_fr";
        case CharRomLocale::AppleIIeFrenchCanadian:              return "iie_frca";
        case CharRomLocale::AppleIIeFrenchCanadianUnenhanced:    return "iie_frca_unenh";
        case CharRomLocale::AppleIIeUK_Enhanced:                 return "iie_uk";
        case CharRomLocale::AppleIIeUK_Unenhanced:               return "iie_uk_unenh";
        case CharRomLocale::AppleIIeGerman:                      return "iie_de";
        case CharRomLocale::AppleIIeGermanImproved:              return "iie_de_improved";
        case CharRomLocale::AppleIIeFrench8k_FR:                 return "iie_fr8k_fr";
        case CharRomLocale::AppleIIeFrench8k_US:                 return "iie_fr8k_us";
        case CharRomLocale::AppleIIeFrenchTouchBlock:            return "iie_ft_block";
    }
    return "default";
}

int charRomBank(CharRomLocale l)
{
    for (const auto& e : charRomCatalog())
        if (e.locale == l) return e.bank;
    return 0;
}

std::string resolveCharRomPath(const std::string& catalogPath)
{
    if (catalogPath.empty()) return {};
    // The catalog stores paths as `roms/foo.rom`. findResource probes the
    // CWD, the build/-relative `../` `../../` roots (dev), and the
    // executable-relative / FHS-install roots (portable bundle, AppImage,
    // /usr/bin) — so the catalog resolves identically on the MainWindow
    // restore path and the live toolbar swap. See ResourcePaths.h.
    return findResource(catalogPath);
}

std::string resolveCharRomPath(CharRomLocale l)
{
    if (l == CharRomLocale::ProfileDefault) return {};
    return resolveCharRomPath(std::string(charRomEntry(l).path));
}

CharRomLocale charRomLocaleFromKey(const std::string& key)
{
    if (key == "ii_classic")        return CharRomLocale::AppleIIClassic;
    if (key == "videx_lc")          return CharRomLocale::VidexLowerCase;
    if (key == "iie_us")            return CharRomLocale::AppleIIeUS_Enhanced;
    if (key == "iie_us_unenh")      return CharRomLocale::AppleIIeUS_Unenhanced;
    if (key == "iie_fr")            return CharRomLocale::AppleIIeFrench;
    if (key == "iie_frca")          return CharRomLocale::AppleIIeFrenchCanadian;
    if (key == "iie_frca_unenh")    return CharRomLocale::AppleIIeFrenchCanadianUnenhanced;
    if (key == "iie_uk")            return CharRomLocale::AppleIIeUK_Enhanced;
    if (key == "iie_uk_unenh")      return CharRomLocale::AppleIIeUK_Unenhanced;
    if (key == "iie_de")            return CharRomLocale::AppleIIeGerman;
    if (key == "iie_de_improved")   return CharRomLocale::AppleIIeGermanImproved;
    if (key == "iie_fr8k_fr")       return CharRomLocale::AppleIIeFrench8k_FR;
    if (key == "iie_fr8k_us")       return CharRomLocale::AppleIIeFrench8k_US;
    if (key == "iie_ft_block")      return CharRomLocale::AppleIIeFrenchTouchBlock;
    return CharRomLocale::ProfileDefault;
}

}  // namespace pom2
