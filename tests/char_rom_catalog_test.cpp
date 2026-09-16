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

// CharRomCatalog — the locale table, its settings keys, and the profile
// partition that decides which entries a machine may be offered.
//
// The 2026-08-28 coverage run put this file at 0 %: it is reachable only
// from three UI translation units, and nothing headless linked it. What it
// holds is not UI, though — it is a *persistence contract* (`state.cfg`
// stores a string key) and a *dispatch table* written three times over:
// once as the catalog vector, once as the enum→key switch, and once as the
// key→enum `if` chain. Three hand-synced copies of one mapping is the exact
// shape that drifts, and the drift is silent: a locale whose key is missing
// from `charRomLocaleFromKey` reads back as ProfileDefault, so the user's
// French //e ROM quietly becomes the stock US one on the next launch, with
// no error anywhere. That is what this file exists to make loud.
//
// The key strings themselves are asserted verbatim. They are on disk in
// every user's settings file; renaming one is not a refactor, it is a
// migration, and the test is where that has to be noticed.

#include "CharRomCatalog.h"

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

using pom2::CharRomLocale;
using pom2::SystemProfile;

// Every enum value, listed by hand on purpose: adding a locale must force a
// visit here, and the compiler cannot see an omission from this array the way
// `-Wswitch` sees one in `charRomLocaleKey`.
const std::vector<CharRomLocale> kAllLocales = {
    CharRomLocale::ProfileDefault,
    CharRomLocale::AppleIIClassic,
    CharRomLocale::VidexLowerCase,
    CharRomLocale::AppleIIeUS_Enhanced,
    CharRomLocale::AppleIIeUS_Unenhanced,
    CharRomLocale::AppleIIeFrench,
    CharRomLocale::AppleIIeFrenchCanadian,
    CharRomLocale::AppleIIeFrenchCanadianUnenhanced,
    CharRomLocale::AppleIIeUK_Enhanced,
    CharRomLocale::AppleIIeUK_Unenhanced,
    CharRomLocale::AppleIIeGerman,
    CharRomLocale::AppleIIeGermanImproved,
    CharRomLocale::AppleIIeFrench8k_FR,
    CharRomLocale::AppleIIeFrench8k_US,
    CharRomLocale::AppleIIeFrenchTouchBlock,
};

const std::vector<SystemProfile> kAllProfiles = {
    SystemProfile::AppleII,
    SystemProfile::AppleIIPlus,
    SystemProfile::AppleIIeUnenhanced,
    SystemProfile::AppleIIe,
    SystemProfile::AppleIIc,
    SystemProfile::AppleIIcPlus,
    SystemProfile::AppleIIeUnenhancedPAL,
    SystemProfile::AppleIIePAL,
    SystemProfile::AppleIIcPAL,
};

}  // namespace

int main()
{
    const auto& cat = pom2::charRomCatalog();

    // ── The settings round-trip ──────────────────────────────────────────
    // The failure this catches: a locale added to the catalog and to the
    // enum→key switch, but not to the key→enum chain. Everything compiles,
    // the dropdown works for the whole session, and the choice is lost on
    // restart.
    for (CharRomLocale l : kAllLocales) {
        const char* key = pom2::charRomLocaleKey(l);
        expect(key != nullptr && key[0] != '\0',
               "every locale has a non-empty settings key");
        expect(pom2::charRomLocaleFromKey(key) == l,
               std::string("key round-trip for '") + key + "'");
    }

    // Keys are unique — two locales sharing one key makes the round-trip
    // above pass for one of them and silently alias the other.
    {
        std::set<std::string> seen;
        for (CharRomLocale l : kAllLocales) {
            const std::string key = pom2::charRomLocaleKey(l);
            expect(seen.insert(key).second,
                   "settings key '" + key + "' is used by exactly one locale");
        }
    }

    // The keys are on disk in every user's state.cfg. Pinned verbatim so a
    // rename is a decision, not a diff nobody reads.
    {
        const std::vector<std::pair<CharRomLocale, const char*>> kKeys = {
            { CharRomLocale::ProfileDefault,                   "default" },
            { CharRomLocale::AppleIIClassic,                   "ii_classic" },
            { CharRomLocale::VidexLowerCase,                   "videx_lc" },
            { CharRomLocale::AppleIIeUS_Enhanced,              "iie_us" },
            { CharRomLocale::AppleIIeUS_Unenhanced,            "iie_us_unenh" },
            { CharRomLocale::AppleIIeFrench,                   "iie_fr" },
            { CharRomLocale::AppleIIeFrenchCanadian,           "iie_frca" },
            { CharRomLocale::AppleIIeFrenchCanadianUnenhanced, "iie_frca_unenh" },
            { CharRomLocale::AppleIIeUK_Enhanced,              "iie_uk" },
            { CharRomLocale::AppleIIeUK_Unenhanced,            "iie_uk_unenh" },
            { CharRomLocale::AppleIIeGerman,                   "iie_de" },
            { CharRomLocale::AppleIIeGermanImproved,           "iie_de_improved" },
            { CharRomLocale::AppleIIeFrench8k_FR,              "iie_fr8k_fr" },
            { CharRomLocale::AppleIIeFrench8k_US,              "iie_fr8k_us" },
            { CharRomLocale::AppleIIeFrenchTouchBlock,         "iie_ft_block" },
        };
        expect(kKeys.size() == kAllLocales.size(),
               "the pinned key table covers every locale");
        for (const auto& [l, key] : kKeys)
            expect(std::string(pom2::charRomLocaleKey(l)) == key,
                   std::string("key is still '") + key + "'");
    }

    // An unknown key — a settings file written by a future build, or one a
    // user edited — must fall back, not crash or alias.
    expect(pom2::charRomLocaleFromKey("") == CharRomLocale::ProfileDefault,
           "empty key falls back to ProfileDefault");
    expect(pom2::charRomLocaleFromKey("iie_es") == CharRomLocale::ProfileDefault,
           "unknown key falls back to ProfileDefault");

    // ── Catalog completeness and lookup ──────────────────────────────────
    for (CharRomLocale l : kAllLocales) {
        bool found = false;
        for (const auto& e : cat) if (e.locale == l) { found = true; break; }
        expect(found, std::string("locale '") + pom2::charRomLocaleKey(l) +
                      "' has a catalog entry");
        expect(pom2::charRomEntry(l).locale == l,
               std::string("charRomEntry resolves '") +
               pom2::charRomLocaleKey(l) + "'");
    }

    // Defensive path documented in the header: a tag out of range must land
    // on ProfileDefault rather than read past the vector.
    expect(pom2::charRomEntry(static_cast<CharRomLocale>(200)).locale ==
               CharRomLocale::ProfileDefault,
           "an out-of-range tag falls back to ProfileDefault");

    for (const auto& e : cat) {
        expect(e.displayName != nullptr && e.displayName[0] != '\0',
               "every entry has a display name");
        if (e.locale == CharRomLocale::ProfileDefault) {
            expect(std::string(e.path).empty(),
                   "ProfileDefault carries no path");
        } else {
            const std::string p = e.path;
            expect(!p.empty(), "a real locale carries a path");
            expect(p.rfind("roms/", 0) == 0,
                   "catalog path '" + p + "' is stored roms/-relative "
                   "(resolveCharRomPath probes from there)");
        }
    }

    // ── The 8 KB two-set part ────────────────────────────────────────────
    // 342-0274-A holds both charsets in one dump; POM2 offers each bank as
    // its own entry. Both entries pointing at bank 0 would draw identical
    // glyphs in the picker — the failure the header warns about.
    {
        const auto& fr = pom2::charRomEntry(CharRomLocale::AppleIIeFrench8k_FR);
        const auto& us = pom2::charRomEntry(CharRomLocale::AppleIIeFrench8k_US);
        expect(std::string(fr.path) == std::string(us.path),
               "both 342-0274-A entries name the same dump");
        expect(pom2::charRomBank(CharRomLocale::AppleIIeFrench8k_FR) == 0 &&
               pom2::charRomBank(CharRomLocale::AppleIIeFrench8k_US) == 1,
               "the two 342-0274-A entries select different banks");
    }
    // The French Touch "Block ASCII" 8 KB part is DUAL-BANK: it carries a
    // sentinel bank of -1 so Memory::loadCharRom keeps both 4 KB sets and lets
    // annunciator 2 ($C05C/$C05D) pick the live one at render time. (Block
    // ASCII Anthology switches its whole font that way — normal-text intro vs
    // block-glyph art.)
    expect(pom2::charRomBank(CharRomLocale::AppleIIeFrenchTouchBlock) == -1,
           "the Block ASCII 8 KB part is dual-bank (AN2-switchable, bank -1)");
    for (CharRomLocale l : kAllLocales) {
        if (l == CharRomLocale::AppleIIeFrench8k_US) continue;
        if (l == CharRomLocale::AppleIIeFrenchTouchBlock) continue;  // dual-bank
        expect(pom2::charRomBank(l) == 0,
               std::string("2 KB / 4 KB part '") + pom2::charRomLocaleKey(l) +
               "' selects bank 0");
    }
    expect(pom2::charRomBank(static_cast<CharRomLocale>(200)) == 0,
           "an out-of-range tag banks to 0");

    // ── The profile partition ────────────────────────────────────────────
    // A 4 KB IIe ROM on a II+ renders garbage and a 2 KB ROM on a //e kills
    // MouseText, so the dropdown is strictly partitioned. Two properties
    // matter: no profile is ever offered an empty list (the user could not
    // get back to "Default"), and exactly one Default is offered — the
    // catalog holds two, one per family.
    for (SystemProfile p : kAllProfiles) {
        int offered = 0, defaults = 0;
        for (const auto& e : cat)
            if (pom2::charRomFitsProfile(e, p)) {
                ++offered;
                if (e.locale == CharRomLocale::ProfileDefault) ++defaults;
            }
        expect(offered > 1, "a profile is offered more than just a default");
        expect(defaults == 1, "exactly one 'Default (per profile)' per profile");
    }

    // No orphan entries: everything in the catalog is reachable from at
    // least one profile, or it is a row nobody can ever pick.
    for (const auto& e : cat) {
        bool reachable = false;
        for (SystemProfile p : kAllProfiles)
            if (pom2::charRomFitsProfile(e, p)) { reachable = true; break; }
        expect(reachable, std::string("entry '") + e.displayName +
                          "' is offered by some profile");
    }

    // And the partition is the one documented: II / II+ see the two 2 KB
    // parts and no IIe ROM; every IIe-class profile sees the reverse.
    {
        const auto& classic = pom2::charRomEntry(CharRomLocale::AppleIIClassic);
        const auto& videx   = pom2::charRomEntry(CharRomLocale::VidexLowerCase);
        const auto& iieFr   = pom2::charRomEntry(CharRomLocale::AppleIIeFrench);
        for (SystemProfile p : { SystemProfile::AppleII, SystemProfile::AppleIIPlus }) {
            expect(pom2::charRomFitsProfile(classic, p),
                   "II/II+ is offered the classic 2 KB generator");
            expect(pom2::charRomFitsProfile(videx, p),
                   "II/II+ is offered the Videx LOWER CASE CHIP");
            expect(!pom2::charRomFitsProfile(iieFr, p),
                   "II/II+ is NOT offered a 4 KB IIe ROM");
        }
        for (SystemProfile p : { SystemProfile::AppleIIe,
                                 SystemProfile::AppleIIeUnenhanced,
                                 SystemProfile::AppleIIc,
                                 SystemProfile::AppleIIcPlus,
                                 SystemProfile::AppleIIeUnenhancedPAL,
                                 SystemProfile::AppleIIePAL,
                                 SystemProfile::AppleIIcPAL }) {
            expect(pom2::charRomFitsProfile(iieFr, p),
                   "a IIe-class profile is offered the IIe ROMs");
            expect(!pom2::charRomFitsProfile(classic, p),
                   "a IIe-class profile is NOT offered the 2 KB classic part");
            expect(!pom2::charRomFitsProfile(videx, p),
                   "a IIe-class profile is NOT offered the Videx chip");
        }
    }

    // ── Path resolution ──────────────────────────────────────────────────
    // ProfileDefault must resolve to nothing at all: the caller's contract is
    // to fall back to the profile's own probe order, and a non-empty string
    // here would load some arbitrary file instead.
    expect(pom2::resolveCharRomPath(CharRomLocale::ProfileDefault).empty(),
           "ProfileDefault resolves to an empty path");
    expect(pom2::resolveCharRomPath(std::string()).empty(),
           "an empty catalog path resolves to an empty path");
    expect(pom2::resolveCharRomPath(std::string("roms/no_such_char_rom.bin")).empty(),
           "a missing dump resolves to an empty path, not a bogus one");

    // Whatever the probe does return must exist — the caller opens it
    // without checking. Dumps are optional, so a locale that resolves to
    // nothing is not a failure here.
    int resolved = 0;
    for (const auto& e : cat) {
        if (std::string(e.path).empty()) continue;
        const std::string got = pom2::resolveCharRomPath(e.locale);
        if (got.empty()) continue;
        ++resolved;
        expect(std::filesystem::exists(got),
               "resolved path '" + got + "' exists on disk");
        // The ROM Status panel judges a present dump by the size the entry
        // implies (RomStatus_ImGui: `size`, else 4 KB IIe-class / 2 KB).
        // The 8 KB two-set parts used to be painted red as "a different
        // file, not a variant" (bug hunt 2026-09-16).
        std::error_code sec;
        const auto have = std::filesystem::file_size(got, sec);
        const std::size_t want = e.size ? e.size : (e.isIIeClass ? 4096u : 2048u);
        expect(!sec && have == want,
               std::string(e.displayName) + ": the shipped dump is " +
               std::to_string(have) + " bytes, the panel expects " +
               std::to_string(want));
    }
    std::printf("char_rom_catalog: %d/%zu dumps present\n",
                resolved, cat.size());

    if (failures == 0) std::printf("char_rom_catalog: OK\n");
    return failures == 0 ? 0 : 1;
}
