// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A2rFlux — an Applesauce A2R flux capture of a 5.25" disk turned into a WOZ
// 2.1 image with FLUX tracks, which DiskImage already plays at the true
// drive clock (DiskImageWozFlux.cpp). A2R is the preservation capture: the
// raw flux of each quarter track, usually more than one revolution, with no
// idea of where a revolution ends. A WOZ flux track is ONE revolution,
// looped. The work here is choosing that revolution.
//
// [B] A2R 2.x reference (applesaucefdc.com/a2r2-reference/, v2.0.1,
//     fetched 2026-11-24): "A2R2" header, INFO (disk type 1 = 5.25"), STRM
//     captures (location in quarter tracks, type 1 timing / 2 bits /
//     3 xtiming, an estimated loop point), 125 ns ticks, a byte per
//     transition with 255 continuing the count.
// [B] A2R 3.x reference (applesaucefdc.com/a2r/, v3.0, fetched 2026-11-24):
//     "A2R3", INFO drive type 1 = 5.25" SS at 0.25 step, RWCP captures with
//     a resolution in picoseconds a tick (62.5 ns by default) and the index
//     signals of the capture, and SLVD: an already "solved" single
//     revolution a track.
// [B] WOZ 2.1 reference (applesaucefdc.com/woz/reference2/): INFO v3
//     flux_block / largest_flux_track, the FLUX map (TMAP's shape), TRKS
//     entries whose bit count is a byte count for flux, 125 ns ticks.
//
// Choosing a revolution: SLVD when the file has one. Otherwise a first
// guess (the capture's index signal, A2R 2's estimated loop point, or a
// nominal 200 ms at 300 RPM), refined to the point within ±4 % where the
// flux repeats best, so the loop joins without a seam.
// [F] The refinement (a 512-transition window compared by absolute
//     difference) is POM2's; Applesauce's own solver is not documented.
//     Settled by: comparing a converted capture's revolution against the
//     SLVD Applesauce wrote for the same track.

#ifndef POM2_A2R_FLUX_H
#define POM2_A2R_FLUX_H

#include <cstdint>
#include <string>
#include <vector>

namespace pom2::a2r {

bool looksLikeA2r(const std::vector<uint8_t>& bytes);

/// Convert. `note` says what came out (version, tracks, how each loop was
/// found). False with the reason: not an A2R, a 3.5" or PC capture, no
/// timing capture, a damaged chunk.
bool toWoz(const std::vector<uint8_t>& a2r, std::vector<uint8_t>& woz,
           std::string& note, std::string& error);

} // namespace pom2::a2r

#endif // POM2_A2R_FLUX_H
