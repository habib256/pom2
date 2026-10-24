// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// STORAGE-013: which whole track a .dsk serves at a quarter-track position.
//
// A sector image carries whole tracks only. At quarter-track 4k+3 the head
// is three quarters of the way to track k+1, and AppleWin's PhaseToTrack
// (`ceil(phase) >> 1` on half-track phases) serves k+1 there; 4k+1 and 4k+2
// stay on k. POM2 rounded every off-track position DOWN, so a write at
// 4k+3 put fields naming k+1 into track k's buffer, which the sector save
// then refused to decode. Pinned for reads (the bit stream at each position
// is the expected track's) and for writes (a write at 4k+3 lands on k+1).

#include "DiskImage.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
namespace fs = std::filesystem;

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

bool sameTrack(const DiskImage& img, int qtA, int qtB)
{
    const int n = img.trackBitLength(qtA);
    if (n <= 0 || n != img.trackBitLength(qtB)) return false;
    for (int i = 0; i < n; ++i)
        if (img.bitAt(qtA, i) != img.bitAt(qtB, i)) return false;
    return true;
}

}  // namespace

int main()
{
    const auto path = fs::temp_directory_path() / "pom2_qt_alias.dsk";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        const std::vector<char> zeros(143360, 0);
        f.write(zeros.data(), std::streamsize(zeros.size()));
    }
    DiskImage img;
    expect(img.loadFile(path.string()), "load blank .dsk");
    img.setWriteBackEnabled(true);

    // Address fields carry the track number, so tracks 1 and 2 differ.
    expect(!sameTrack(img, 4, 8), "tracks 1 and 2 are distinguishable");
    expect(sameTrack(img, 5, 4), "qt 5 (4k+1) reads track 1");
    expect(sameTrack(img, 6, 4), "qt 6 (4k+2) reads track 1");
    expect(sameTrack(img, 7, 8), "qt 7 (4k+3) reads track 2");
    expect(DiskImage::wholeTrackOf(7) == 2 && DiskImage::wholeTrackOf(6) == 1 &&
           DiskImage::wholeTrackOf(0) == 0 && DiskImage::wholeTrackOf(139) == 35,
           "wholeTrackOf follows AppleWin's PhaseToTrack");
    expect(img.trackBitLength(159) >= 0, "the last quarter-track stays in range");

    // A write at qt 7 lands on track 2: compare against an untouched copy.
    DiskImage before;
    expect(before.loadFile(path.string()), "reload reference");
    // Half a revolution of $FF nibbles: a transition in every cell.
    const int period = img.trackPeriod(7);
    std::vector<int64_t> pulses;
    for (int64_t t = 4; t < period / 2; t += 8) pulses.push_back(t);
    img.writeFlux(7, 0, period / 2, int(pulses.size()), pulses.data());
    bool track2Changed = false, track1Changed = false;
    for (int i = 0; i < img.trackBitLength(8) && !track2Changed; ++i)
        track2Changed = img.bitAt(8, i) != before.bitAt(8, i);
    for (int i = 0; i < img.trackBitLength(4) && !track1Changed; ++i)
        track1Changed = img.bitAt(4, i) != before.bitAt(4, i);
    expect(track2Changed, "a write at qt 7 changes track 2");
    expect(!track1Changed, "a write at qt 7 leaves track 1 alone");

    std::error_code ec;
    fs::remove(path, ec);
    std::printf("disk_quarter_track_alias: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
