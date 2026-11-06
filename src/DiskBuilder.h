// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DiskBuilder — 5.25" disk images that carry NO Apple code:
//
//   * blankDos33()   a formatted, EMPTY DOS 3.3 data disk (VTOC, catalog,
//                    bit map), with no DOS image on tracks 0-2;
//   * directBoot()   a disk that loads a user's binary straight off the
//                    boot PROM, with no operating system at all;
//   * notBootableSector()  the 256-byte boot sector both of them build on:
//                    booted, it says "NOT A STARTUP DISK" and drops into the
//                    Monitor instead of executing zeros.
//
// Why no Apple code: POM2 ships none (CLAUDE.md, "No copyrighted firmware").
// A DOS 3.3 image is Apple's; ProDOS's block 0 is Apple's; so is the Disk II
// PROM. Every 6502 byte here is hand-assembled and listed next to its array in
// DiskBuilder.cpp, and the one routine taken from elsewhere — the direct-boot
// loader — is Casso's (MIT), re-verified instruction by instruction.
//
// The boot code works with BOTH boot PROMs POM2 can run — Apple's
// 341-0027-A (roms/disk2.rom) and POM2's own (DiskIIBootRom.h) — and with the
// //c's built-in one, because it relies only on what the three share:
//
//   $0800      the PROM's terminating sector count (byte 0 of the sector);
//   $0801      where the PROM jumps once it has read that many;
//   $2B        slot*16;
//   $Cs5C      the read entry: reads physical sector $3D, of the track whose
//              address fields say $41, into the page at ($26) with $26 = 0,
//              then INC $27, INC $3D, and loops while $3D < ($0800);
//   the head   on track 0 (phase 0's position) when $0801 first runs.
//
// The sources for each are cited at the code that leans on them.
//
// Byte order. Every image here is a DOS-ordered sector buffer — the byte
// order of a .dsk / .do file: track t's DOS-logical sector s at offset
// (t*16 + s)*256. dosOrderToProdosOrder() gives the .po order.

#ifndef POM2_DISK_BUILDER_H
#define POM2_DISK_BUILDER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2::diskbuild {

inline constexpr int         kTracks          = 35;
inline constexpr int         kSectorsPerTrack = 16;
inline constexpr std::size_t kSectorBytes     = 256;
inline constexpr std::size_t kImageBytes      = 143360;   // 35 tracks x 16 sectors x 256
static_assert(kImageBytes == std::size_t(kTracks) * std::size_t(kSectorsPerTrack) * kSectorBytes,
              "a 140 KB 5.25\" image");

// ── The "not a startup disk" sector ────────────────────────────────────────

/// What the sector prints (in high-bit ASCII on the screen).
inline constexpr char kNotStartupMessage[] = "NOT A STARTUP DISK";

/// The boot sector of a disk that has nothing to start, 256 bytes: motor
/// off, PR#0 / IN#0, text screen cleared, the message through COUT, then the
/// Monitor's `*` prompt. Byte 0 is 1 (the PROM reads this sector only).
/// For a ProDOS volume, the caller places it in the first half of block 0:
/// block 0 is DOS-ordered sectors 0 and 14 (physical 0 and 2) of track 0, and
/// the PROM loads physical sector 0 only.
std::vector<uint8_t> notBootableSector();

// ── Blank DOS 3.3 data disk ────────────────────────────────────────────────

/// Free sectors on blankDos33(): 560 - track 0 - track 17 = 528, where a
/// DOS-INITed disk has 496 (tracks 0-2 hold its DOS image). See the .cpp.
inline constexpr int kBlankDos33FreeSectors = 528;

/// A formatted, EMPTY DOS 3.3 data disk as a DOS-ordered sector buffer:
/// VTOC at 17/0, catalog chain 17/15 down to 17/1, bit map with tracks 0 and
/// 17 in use and every other track free, notBootableSector() at 0/0.
/// `volume` must be 1-254 (Beneath Apple DOS ch. 4, VTOC byte $06); 0 or
/// 255 returns an EMPTY vector rather than a disk DOS would not accept.
std::vector<uint8_t> blankDos33(uint8_t volume = 254);

// ── Direct boot ────────────────────────────────────────────────────────────

struct DirectBootSpec {
    uint16_t loadAddress  = 0x0900;
    uint16_t entryAddress = 0x0900;
};

/// The window a payload may occupy: page $08 is the loader's while it runs
/// and $0200-$03FF the PROM's workspace; $C000 up is not RAM.
inline constexpr uint16_t kDirectBootLowest  = 0x0900;
inline constexpr uint32_t kDirectBootCeiling = 0xC000;   // first address NOT loadable
/// Track 0 holds only the loader; the payload starts on track 1.
inline constexpr int      kDirectBootFirstTrack = 1;
/// The loader asks a track for every 2nd sector (see directBootReadOrder).
inline constexpr int      kDirectBootReadStep = 2;

/// Where the loader sector keeps what a build fills in (offsets in the
/// sector, i.e. address - $0800). Everything else is the loader verbatim.
inline constexpr std::size_t kLoaderEntryLo     = 0x54;   // JMP operand
inline constexpr std::size_t kLoaderEntryHi     = 0x55;
inline constexpr std::size_t kLoaderReadOrder   = 0xE0;   // 16 physical sector numbers
inline constexpr std::size_t kLoaderSectorsLeft = 0xF0;
inline constexpr std::size_t kLoaderLoadPage    = 0xF1;
inline constexpr std::size_t kLoaderTrack       = 0xF2;
inline constexpr std::size_t kLoaderPhase       = 0xF3;
inline constexpr std::size_t kLoaderIndex       = 0xF4;
/// The loader's code ends here; $0880-$08DF stay zero.
inline constexpr std::size_t kLoaderCodeBytes   = 0x80;

/// The order the loader requests a track's physical sectors in, which is
/// also the order the payload's pages are laid down in: 0 2 4 … 14 1 3 … 15.
std::array<uint8_t, 16> directBootReadOrder();

/// Largest payload for that load address ($0900..$BFFF window), 0 outside it.
std::size_t directBootCapacity(uint16_t loadAddress);
/// Sectors a payload of that size occupies, counting the zero lead-in that
/// pads a load address that is not page-aligned. 0 for an empty payload.
std::size_t directBootSectors(uint16_t loadAddress, std::size_t payloadBytes);

/// The complete DOS-ordered sector buffer, or false with ONE reason in
/// `refusal` (empty payload; load address outside the window; too large,
/// stating the capacity; entry outside the payload). All-or-nothing:
/// `sectors` is assigned only on success, and `refusal` cleared then.
bool directBoot(const std::vector<uint8_t>& payload, const DirectBootSpec& spec,
                std::vector<uint8_t>& sectors, std::string& refusal);

// ── Sector orders ──────────────────────────────────────────────────────────

/// DOS-ordered (.dsk/.do) <-> ProDOS-ordered (.po) byte order of a
/// 143360-byte image. Any other size returns an empty vector.
std::vector<uint8_t> dosOrderToProdosOrder(const std::vector<uint8_t>& dosOrdered);
std::vector<uint8_t> prodosOrderToDosOrder(const std::vector<uint8_t>& prodosOrdered);

/// For a caller that lays bytes down by PHYSICAL sector (the number in the
/// address field): the index of that sector within its track in a
/// DOS-ordered buffer. -1 outside 0..15.
int dosFileIndexForPhysicalSector(int physicalSector);
/// The same, in a ProDOS-ordered buffer.
int prodosFileIndexForPhysicalSector(int physicalSector);

} // namespace pom2::diskbuild

#endif // POM2_DISK_BUILDER_H
