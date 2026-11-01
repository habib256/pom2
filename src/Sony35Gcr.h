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

// Sony35Gcr — the Apple 800K 3.5" GCR *read* path, as one pure module.
//
// Two things in POM2 have to turn Sony GCR cells back into 512-byte ProDOS
// blocks, and they arrive at the cells from opposite directions:
//
//   * `Sony35Drive` — the guest wrote them, a track at a time, through the
//     IWM; `decodeAndCommit` folds them back into the mounted image.
//   * `Disk35Image` — a WOZ flux dump on disk holds them already, and a
//     `.woz` has to become blocks at LOAD time (POM2 stores 3.5" media as a
//     flat block array; it has no GCR encoder, so there is nothing to mount
//     a flux image *as*).
//
// The decode is identical either way and is a port of MAME
// `flopimg.cpp:2107 extract_sectors_from_track_mac_gcr6` (address field
// D5 AA 96 + track/sector/side/format/checksum, data field D5 AA AD + 175
// 4-in-3 GCR groups + the three-way running checksum + DE AA). Keeping one
// copy is the point of this file: a second transcription of the tables and
// the checksum walk is a second thing to get subtly wrong.

#ifndef POM2_SONY35_GCR_H
#define POM2_SONY35_GCR_H

#include <cstdint>
#include <functional>
#include <vector>

namespace pom2 {
namespace sony35 {

/// Sectors on 3.5" track `t` (0..79) — the 5 zones, 12/11/10/9/8.
/// [D] MAME ap_dsk35.cpp:368 @ mame0287 — `int ns = 12 - (track/16);`
/// (the `apple35_sectors_per_track[]` table went in dc57db5ceb, 2022-09-25).
constexpr int sectorsForTrack(int track) { return 12 - (track / 16); }

/// Block index of (track, head, logical sector) in the flat 1600-block
/// ProDOS payload. [D] MAME `apple_gcr_format::load` (ap_dsk35.cpp:366-386
/// @ mame0287):
/// linear by track, then head, then sector.
int blockIndexFor(int track, int head, int sector);

/// One byte per bit-cell (0/1) → the self-synced disk-byte stream, as the
/// IWM's shifter would see it. MAME `flopimg.cpp:1535-1551` alignment.
std::vector<uint8_t> nibblesFromCells(const std::vector<uint8_t>& cells);

/// Expand WOZ-style PACKED bits (MSB first) into the one-byte-per-cell form
/// `nibblesFromCells` takes. `bitCount` may be shorter than 8*len.
std::vector<uint8_t> cellsFromPackedBits(const uint8_t* data, std::size_t len,
                                         uint32_t bitCount);

/// A decoded sector: `data` is the 512-byte block (the 12 tag bytes ahead of
/// it are dropped, as the ProDOS payload has no room for them).
using SectorSink = std::function<void(int track, int head, int sector,
                                      const uint8_t* data)>;

/// Walk `nib` and hand every sector that passes BOTH checksums to `sink`.
/// Returns the number of sectors delivered.
///
/// `expectTrack >= 0` filters to that track — what `Sony35Drive` wants,
/// since it knows where its head is and a stray address field from a
/// neighbouring track would commit blocks the guest never wrote. Pass -1 to
/// accept whatever the address fields say, which is what a file loader
/// wants (it is reading the tracks in order and cross-checks itself).
int decodeSectors(const std::vector<uint8_t>& nib, int expectTrack,
                  const SectorSink& sink);

}  // namespace sony35
}  // namespace pom2

#endif  // POM2_SONY35_GCR_H
