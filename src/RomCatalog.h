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

// RomCatalog — what every ROM POM2 looks for actually IS.
//
// The repo and release packages ship a full roms/ tree (see
// packaging/bundle.manifest); user overrides drop into the per-user data
// dir. A missing or mis-named dump there is still why "it doesn't boot"
// or "that card won't plug". The probe order
// for the MACHINE firmware + character generator already lives in
// `SystemProfile.h` (`romProbeOrder` / `charRomProbeOrder`) and the ROM
// Status panel reads it straight from there — one source of truth, so a new
// profile shows up without touching this file.
//
// What is NOT expressible there is the peripheral side: each card probes its
// own candidates at its own plug site in MainWindow.cpp / ClockCard.cpp. This
// table names those, in the same order the code tries them, with what the
// dump is and what happens when it is absent — that last column is the one
// that matters, because most card ROMs are optional and degrade rather than
// fail.
//
// `size` is the only hard check: a Disk II PROM is 256 bytes and a Grappler+
// EPROM is 4 KB, full stop, so a mismatch is a wrong file, not a variant.
// CRC32 is shown for identification but only *asserted* where POM2 has a
// documented reference dump to compare against (see `knownCrc`).

#ifndef POM2_ROM_CATALOG_H
#define POM2_ROM_CATALOG_H

#include <cstdint>
#include <vector>

namespace pom2 {

/// One catalogued ROM: what it is, where POM2 looks, how big it must be.
struct RomCatalogEntry {
    const char* group;        ///< Section heading in the panel.
    const char* name;         ///< Human name of the part.
    /// Candidate paths, in the order the code probes them. The first that
    /// resolves through ResourcePaths is the one actually used.
    std::vector<const char*> candidates;
    /// Required byte size, or 0 when several are legitimate.
    std::size_t size;
    /// CRC32 of a dump POM2 can vouch for, or 0 when there is no reference
    /// to check against — in that case the panel reports the checksum as
    /// identification only and passes no judgement on it.
    std::uint32_t knownCrc;
    const char* knownCrcLabel; ///< What that CRC identifies. May be empty.
    /// What POM2 does when none of the candidates resolve.
    const char* whenMissing;
};

/// The peripheral-side catalogue. Machine + character ROMs come from
/// `profileConfig()`, not from here.
inline const std::vector<RomCatalogEntry>& romCatalog()
{
    static const std::vector<RomCatalogEntry> kEntries = {
        // ─── Disk II ──────────────────────────────────────────────────────
        { "Disk II (5.25\")", "Boot PROM, 16-sector (Apple 341-0027-A)",
          { "roms/disk2.rom" }, 256, 0, "",
          "Falls back to the embedded 341-0027-A — the card still boots." },
        { "Disk II (5.25\")", "P6 sequencer PROM, 16-sector (Apple 341-0028-A)",
          { "roms/diskii_p6.rom" }, 256, 0, "",
          "Falls back to the legacy 32-cycle nibble gate. WOZ images force "
          "the bit-level LSS path anyway, using the embedded default." },
        { "Disk II (5.25\")", "Boot PROM, 13-sector (Apple 341-0009)",
          { "roms/disk2_13.rom" }, 256, 0, "",
          "13-sector (DOS 3.2 era) disks mount but cannot boot." },
        { "Disk II (5.25\")", "P6 sequencer PROM, 13-sector (Apple 341-0010)",
          { "roms/diskii_p6_13.rom" }, 256, 0, "",
          "Same: the card never switches to the 13-sector pair." },

        // ─── Storage cards ────────────────────────────────────────────────
        { "Storage cards", "Liron / SmartPort controller (4 KB)",
          { "roms/liron.rom" }, 4096, 0, "",
          "SmartPortCard presents a synthetic identity instead of the real "
          "firmware. Blocks still work; the $Cn0D dispatch does not." },
        { "Storage cards",
          "Apple II 3.5\" Disk Controller / SuperDrive (341-0438-A)",
          { "roms/341-0438-a.bin" }, 32768, 0xC73FF25Bu,
          "MAME a2superdrive (CRC c73ff25b)",
          "Not used by POM2 yet — kept for the MAME apple2eefr oracle "
          "(-slN superdrive) and a future SuperDrive card port." },
        { "Reference dumps (oracle only)",
          "//e international keyboard decode ROM (342-0326-A, FR)",
          { "roms/342-0326-a.f12" }, 2048, 0xF04970A9u,
          "MAME apple2eefr (BAD_DUMP upstream: FR half + QWERTY UK half)",
          "Nothing — POM2 maps host keys directly and has no keyboard-decode "
          "ROM. Present only so the MAME PAL //e oracle romset is complete." },
        { "Storage cards", "CFFA 2.0 firmware, 65C02 build",
          { "roms/cffa20eec02.bin" }, 4096, 0xFB3726F8u,
          "dreher.net Run6_CDROM.zip",
          "The CFFA card refuses to plug — it is a ROM-driven card." },
        { "Storage cards", "CFFA 2.0 firmware, 6502 build",
          { "roms/cffa20ee02.bin" }, 4096, 0x3ECAFCE5u,
          "dreher.net Run6_CDROM.zip",
          "Same — one of the two CFFA dumps must be present." },

        // ─── Printer / input / clock ──────────────────────────────────────
        { "Other cards", "Grappler+ parallel printer EPROM (Orange Micro)",
          { "roms/grappler_plus.bin", "roms/grappler+.bin",
            "roms/grappler.bin" }, 4096, 0, "",
          "The card plugs with a synthetic stub ROM: PR#n still prints, but "
          "software that looks for the real firmware (AppleWorks' "
          "\"Printer = Grappler+\") will not find it." },
        { "Other cards", "TransWarp accelerator ROM v1.4 (Applied Engineering)",
          { "roms/ae_transwarp_1.4.bin",
            "roms/ae transwarp rom v1.4.bin" }, 4096, 0xAFE37F55u, "MAME warprom",
          "The card still accelerates without it — the ROM only supplies AE's "
          "speed-corrected Monitor, which it overlays on $F000-$FFFF until "
          "software writes $C072. Without it the stock F8 ROM's 1 MHz delay "
          "loops (WAIT, the beep) come out 3.5x short." },
        { "Other cards", "Mouse card slot EPROM (Apple 341-0270-C)",
          { "roms/mouse_341-0270-c.bin" }, 2048, 0, "",
          "Neither mouse card can be plugged (both variants need it)." },
        { "Other cards", "Mouse card 6805 MCU ROM (Apple 341-0269)",
          { "roms/mouse_341-0269.bin" }, 2048, 0, "",
          "The MAME-faithful mouse card refuses to plug; the AppleWin HLE "
          "variant, which only needs the slot EPROM, still works." },
        { "Reference dumps (oracle only)",
          "Apple II Workstation Card firmware (341-0358-A, 64 KB)",
          { "roms/341-0358-A.bin", "roms/341-0358-a.bin" }, 65536, 0x63819DCBu,
          "sha1 59c8e8c88bac5c31ada1306b412edfcf5912a720",
          "Nothing — no card reads it yet. Catalogued because the dump is "
          "the analysed one (docs/printer_plan_2.md \u00a7 5.1): the Apple II "
          "side of the firmware is at file offsets 0xC400 ($Cn00 page) and "
          "0xC800-0xCFFF (expansion ROM), and the card's own 65C02 image is "
          "the upper 32 KB." },

        { "Other cards", "Videx LOWER CASE CHIP character generator (1980)",
          { "roms/Videx Lower Case Chip ROM.bin" }, 2048, 0x00F68076u,
          "sha1 447874fe0850c8add3fd5b13fa98f6648fe6f999",
          "The Videx entry disappears from the character-set picker; the "
          "II/II+ keeps its stock uppercase-only generator, which is what "
          "the machine shipped with." },

        { "Other cards", "ThunderClock+ slot ROM (Thunderware)",
          { "roms/thunderclock_u9_v1.3.bin", "roms/thunderclock_u9.bin",
            "roms/thunderclock.rom",
            // ClockCard::tryLoadDump probes this fourth name too — the
            // upstream markadev filename. Missing here, the panel called a
            // dump the card was actually USING "missing", which is the one
            // thing this table exists not to do.
            "roms/Thunderware_REV_1.3_ROM_U9.bin" }, 0, 0, "",
          "The clock card runs its synthetic ROM — ProDOS still reads the "
          "date, but the real firmware entry points are absent." },
    };
    return kEntries;
}

}  // namespace pom2

#endif  // POM2_ROM_CATALOG_H
