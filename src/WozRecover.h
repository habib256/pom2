// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WozRecover — turn a damaged WOZ file into a healthy one, and say which
// sectors of it can no longer be trusted.
//
// POM2 mounts a damaged WOZ read-only (DiskImage::integrityWarning): a wrong
// CRC or a track record pointing outside the file means a save would paper
// over the damage. That protects the file; it does not give the user a disk
// they can write to again, nor tell them what was lost. This does both.
//
// What "recover" means here, exactly:
//
//   * the CONTAINER is repaired in place — nothing is moved, re-encoded or
//     re-timed. A track whose record is usable keeps its bit stream bit for
//     bit; a track whose record points outside the file is unmapped (its
//     TMAP entries become $FF and its record zeros), because its bits are
//     gone and a mapped-but-missing track reads as something worse than a
//     blank one. A file cut short is padded back to its declared length.
//     Then the CRC is recomputed. The result loads clean.
//   * the CONTENT is not repaired — nothing here invents data. Each track's
//     bit stream is read as the drive would read it and every standard
//     sector's two checksums are verified; the report lists the sectors that
//     are missing or fail. A bit flipped inside a sector cannot be put back,
//     but it can be FOUND, which is the difference between "the disk was
//     recovered" and "the disk was recovered, and sector 9 of track 17 is
//     not what it was".
//
// "Does not verify" is not "is damaged". A copy-protected track is non-
// standard on purpose, and the report says "no standard sector" for it, not
// "bad". The report is evidence; the user decides.
//
// [B] WOZ 2.1 reference (applesaucefdc.com/woz/reference2): the header and
//     its CRC ("CRC32 of all remaining data in the file"), the chunk
//     structure, "the primary data chunks are all located in fixed
//     positions" (INFO data at 20, TMAP at 88, TRKS at 256, track bits from
//     1536), the TRK record (starting block u16, block count u16, bit count
//     u32), and for flux tracks "the Bit Count is actually a byte count".
//     WOZ 1 reference for the 6656-byte track entries (6646 bytes of bits,
//     bytes used u16, bit count u16).
// [B] Beneath Apple DOS, ch. 3: the address field (D5 AA 96, volume / track /
//     sector / checksum in 4-and-4, checksum = XOR of the three) and the
//     data field (D5 AA AD, 342 six-and-two nibbles and a checksum nibble —
//     each stored value is XORed with the previous, so all 343 XOR to zero).
//     DOS 3.2's D5 AA B5 address and 410 + 1 five-and-three nibbles likewise.
//
// Pure: bytes in, bytes and a report out. `woz_recover` tests it, including
// against POM2's own WOZ writer as an independent encoder.

#ifndef POM2_WOZ_RECOVER_H
#define POM2_WOZ_RECOVER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {
namespace woz {

struct TrackReport {
    int      trksIndex = 0;           ///< the TRK record
    /// Where it sits: the whole-track TMAP entry mapping it (track * 4) when
    /// there is one, else the lowest entry (a track recorded between tracks).
    int      firstQuarterTrack = 0;
    bool     kept = true;             ///< false: its record was unusable, unmapped
    uint32_t bitCount = 0;
    bool     thirteenSector = false;  ///< DOS 3.2 address prologue seen
    int      addressFields = 0;       ///< with a good checksum
    uint32_t sectorsGood = 0;         ///< bit n: sector n, address AND data verify
    uint32_t sectorsBad  = 0;         ///< bit n: address verifies, data missing or bad
    bool     wrongTrackNumber = false;///< address fields name another track
    std::string note;                 ///< why it was dropped, when it was
    int expectedSectors() const { return thirteenSector ? 13 : 16; }
    /// Every standard sector is there and verifies.
    bool verifies() const;
};

struct Report {
    bool        isWoz = false;
    int         version = 0;          ///< 1 or 2
    int         diskType = 0;         ///< INFO: 1 = 5.25", 2 = 3.5"
    uint32_t    crcStated = 0;
    uint32_t    crcComputed = 0;
    bool        crcWasWrong = false;  ///< stated non-zero and different
    std::vector<std::string> repairs; ///< what recover() changed, in order
    std::vector<TrackReport> tracks;
    int tracksDropped() const;
    int tracksNotVerifying() const;   ///< kept, analysed, and not fully standard
    /// Nothing was repaired and every analysed track verifies.
    bool clean() const { return repairs.empty() && tracksNotVerifying() == 0; }
};

/// Read-only look: the same report recover() gives, with `repairs` listing
/// what it WOULD change.
Report inspect(const std::vector<uint8_t>& file);

/// `healthy` receives the repaired file. False (with `error`, `healthy`
/// untouched) only when there is nothing to work from: not a WOZ, or no
/// INFO / TMAP / TRKS to be found even at the fixed positions.
bool recover(const std::vector<uint8_t>& damaged, std::vector<uint8_t>& healthy,
             Report& report, std::string& error);

/// The report as text, one line per track that is not simply fine.
std::string formatReport(const Report& report);

/// CRC-32 as the WOZ reference's Appendix A defines it (the common
/// reflected 0xEDB88320 one), initial value 0.
uint32_t crc32(const uint8_t* data, std::size_t size);

}  // namespace woz
}  // namespace pom2

#endif  // POM2_WOZ_RECOVER_H
