// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A test's "the guest wrote this track", done so that every sector survives.
//
// The old idiom was `writeNibbleAt(track, n, cur ^ 1)` at an arbitrary index.
// Wherever that index fell inside an address or data field it made the sector
// undecodable — and the write-back then "saved" the track with that sector
// keeping the file's old bytes, which is what the tests using it were, without
// knowing, asserting. The save now refuses a track that is not all there
// (DiskImage_WriteBack.cpp, rule 2), so a test that only wants a DIRTY track
// writes where a real drive leaves slack: one sync byte in the middle of a gap.

#ifndef POM2_TESTS_SYNC_GAP_WRITE_H
#define POM2_TESTS_SYNC_GAP_WRITE_H

#include "DiskImage.h"

namespace pom2test {

/// Turn one $FF in the middle of a sync run into $FE, at or after `from`.
/// Returns the index written, or -1 when the track has no such gap. The track
/// is dirty afterwards and decodes to exactly the sectors it held.
inline int writeInSyncGap(DiskImage& img, int track, int from = 1)
{
    if (from < 1) from = 1;
    for (int i = from; i + 1 < DiskImage::kNibblesPerTrack; ++i) {
        if (img.nibbleAt(track, i - 1) == 0xFF && img.nibbleAt(track, i) == 0xFF &&
            img.nibbleAt(track, i + 1) == 0xFF) {
            img.writeNibbleAt(track, i, 0xFE);
            return i;
        }
    }
    return -1;
}

}  // namespace pom2test

#endif  // POM2_TESTS_SYNC_GAP_WRITE_H
