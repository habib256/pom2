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

// RomErrata — known defects of a ROM DUMP, put right as the dump is mapped.
//
// The dumps are SHA-256-pinned (RomFetch, tools/test_media.sha256, the
// archive.org items), so a defect cannot be fixed in the file without every
// user's copy failing its check. It is fixed in memory instead, and only when
// the loaded bytes are EXACTLY the known-bad dump: anything else — the right
// ROM, a user's own dump, a hack — is left alone.
//
// ─── The ][+ IRQ/BRK vector ($FFFE) ─────────────────────────────────────
// roms/apple2p.rom (sha256 92c4bef6…, RetroBIOS / pom2_firm) is Apple's ][+
// set page for page except two bytes: its $FFFE/$FFFF read $59 $FF, where the
// Autostart Monitor 341-0020-00 has $40 $FA.
//   [B] Apple's own copy of that ROM: "Confidential Service Disk No.1",
//       file ASF8.DATA — the reference its dealer ROM test compares against
//       — ends $40 $FA (found 2026-11-07, see CHANGELOG).
//   [D] MAME apple2.cpp @ b2a182f0b7: ROM_LOAD "341-0020-00.f8"
//       CRC 079589c4 — the page with $40 $FA; the dump's page is ecffd453.
//   [D] AppleWin resource/Apple2_Plus.rom (master, 12 KB): $40 $FA, page CRC
//       079589c4 — AppleWin's OWN earlier ][+ image was the $FF59 one.
// Where the $FF59 came from is not known [F]. comp.sys.apple2, "Ultima &
// Mockingboards on each of the Apples" (Dec 2005): AppleWin's ][+ ROM and
// MESS's a2p both had it, MESS having "most likely just taken the ROMs from
// AppleWin"; nobody traced it to a chip, and "a hacked ROM designed for
// cracking" was a guess. Patching it back to $FA40 is what fixed Ultima
// III's Mockingboard music there, and what a2emu-vera's README does to the
// Asimov "apple_ii+_rom.zip" image today. The pinned dump's 20 KB layout (a
// $C000-$CFFF copy at $B000) suggests the old AppleWin APPLE2.ROM — inferred
// from the thread, not checked against that file.
// $FA40 is the Monitor's IRQ entry: it saves A, tells BRK from IRQ and goes
// through $03F0 (BRK) or $03FE (IRQ). $FF59 is OLDRST, the Monitor's reset
// entry. So on the dump every IRQ and every BRK landed in the Monitor's
// "*" prompt: an interrupt-driven program on a ][+ (a Mockingboard player
// hooking $03FE) died on its first interrupt, and BRK showed no registers.

#ifndef POM2_ROM_ERRATA_H
#define POM2_ROM_ERRATA_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

/// CRC-32 (IEEE, reflected) — the figure MAME's ROM_LOAD lines carry.
inline std::uint32_t romCrc32(const std::uint8_t* p, std::size_t n)
{
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/// Apply the errata to one $F800 page (2 KB, contiguous). Returns a one-line
/// note per correction made, empty when none applied.
inline std::string applyRomErrataF8(std::uint8_t* f8)
{
    std::string note;
    constexpr std::uint32_t kDumpF8   = 0xECFFD453u;   // apple2p.rom's $F800 page
    constexpr std::uint32_t kAppleF8  = 0x079589C4u;   // 341-0020-00.f8
    if (f8[0x7FE] == 0x59 && f8[0x7FF] == 0xFF &&
        romCrc32(f8, 0x800) == kDumpF8) {
        f8[0x7FE] = 0x40;
        f8[0x7FF] = 0xFA;
        if (romCrc32(f8, 0x800) == kAppleF8) {
            note = "IRQ/BRK vector $FFFE put back to $FA40 (341-0020-00): "
                   "this dump carries $FF59 (RomErrata.h)";
        } else {   // cannot happen for the pinned dump; never half-apply
            f8[0x7FE] = 0x59;
            f8[0x7FF] = 0xFF;
        }
    }
    return note;
}

/// Apply the errata to a mapped 64 KB image (`mem[0x0000..0xFFFF]`).
inline std::string applyRomErrata(std::uint8_t* mem) { return applyRomErrataF8(mem + 0xF800); }

/// The same errata on a snapshot's or a session's ENV section, which stores
/// $D000-$FFFF as (RAM flag, byte) pairs from byte 8
/// (Memory::appendSnapshotEnvironment). Without it every ][+ .pom2snap or
/// session saved before the erratum was refused as "firmware differs" — the
/// live ROM had changed under it — and a session reconstructed from its own
/// ENV brought $FF59 back (bug hunt 2026-11-07). Both sides are normalised:
/// MachineSnapshot compares the corrected ENV, installSessionEnvironment
/// installs the corrected page. A page holding RAM is never touched.
inline void applyRomErrataToEnvironment(std::vector<std::uint8_t>& env)
{
    constexpr std::size_t base = 8 + (0xF800 - 0xD000) * 2;
    if (env.empty() || env[0] != 1 || env.size() < base + 0x800 * 2) return;
    std::uint8_t page[0x800];
    for (std::size_t i = 0; i < 0x800; ++i) {
        if (env[base + 2 * i] != 0) return;          // RAM there: not a ROM page
        page[i] = env[base + 2 * i + 1];
    }
    if (applyRomErrataF8(page).empty()) return;
    for (std::size_t i = 0; i < 0x800; ++i) env[base + 2 * i + 1] = page[i];
}

}  // namespace pom2

#endif  // POM2_ROM_ERRATA_H
