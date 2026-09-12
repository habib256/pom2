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

// Disk II write-back framing test — DiskImage::writeFlux, non-WOZ store.
//
// A nibble-store image has no angular length, so the flux the head lays
// down has to be framed back into nibbles the way the read sequencer does
// it. Two properties must hold, and both were broken before 2026-07:
//
//   1. A data field written over an existing track lands EXACTLY, and
//      nothing outside the written window moves. The old re-pack aligned
//      the incoming cells to the nibble grid of the track that was already
//      there; the moment the new content padded its sync run differently
//      (which a real sector write always does) every nibble after the
//      first $FF was assembled from its neighbours' cells. DOS 3.3
//      answered I/O ERROR and the track stayed unreadable — CATALOG failed
//      too, and Print Shop hung forever retrying its setup save.
//
//   2. The cell grid comes from the WRITE clock, not the revolution
//      anchor. The head emits one cell every `cyc` LSS cycles from the
//      moment write mode came on, so a burst's transitions are exact
//      multiples of `cyc` apart. Quantising them against the revolution
//      phase instead put the grid at an arbitrary sub-cell offset:
//      adjacent transitions rounded into the same cell, one was dropped,
//      and the framing slipped a bit at a time.
//
// The write is flushed in ~30-transition chunks, which is what DiskIICard
// does — a nibble straddles a chunk boundary constantly, so this also
// pins the carry-over of the shift accumulator between flushes.

#include "DiskImage.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kSyncMinRun = 5;   // DiskImage's self-sync rule
constexpr int kCyc        = 8;   // LSS cycles per cell for a .dsk

// Padded cell width per nibble, same rule DiskImage expands with.
void cellWidths(const std::vector<uint8_t>& buf, std::vector<int>& w)
{
    const int n = static_cast<int>(buf.size());
    w.assign(static_cast<size_t>(n), 8);
    int i = 0;
    while (i < n) {
        if (buf[static_cast<size_t>(i)] != 0xFF) { ++i; continue; }
        int j = i;
        while (j < n && buf[static_cast<size_t>(j)] == 0xFF) ++j;
        int run = j - i;
        if (i == 0 && j == n) run = n;
        if (run >= kSyncMinRun)
            for (int k = i; k < j; ++k) w[static_cast<size_t>(k)] = 10;
        i = j;
    }
}

fs::path writeScratchDsk()
{
    const fs::path p =
        fs::temp_directory_path() / "pom2_writeflux_framing.dsk";
    // A DOS-ordered image with varied content — DiskImage nibblizes it on
    // load, giving a stock 16-sector track to write over.
    std::vector<uint8_t> bytes(DiskImage::kBytesPerImage);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return p;
}

// One DOS 3.3 data field: sync, prologue, payload, epilogue.
std::vector<uint8_t> dataField()
{
    std::vector<uint8_t> v;
    for (int i = 0; i < 5; ++i) v.push_back(0xFF);
    v.push_back(0xD5); v.push_back(0xAA); v.push_back(0xAD);
    for (int i = 0; i < 342; ++i)
        v.push_back(static_cast<uint8_t>(0x96 + (i * 7 % 0x40)));
    v.push_back(0xDE); v.push_back(0xAA); v.push_back(0xEB);
    return v;
}

// Write `payload` starting at nibble `startNib` of `track`, flushing every
// `chunk` transitions the way DiskIICard does. `anchor` is the revolution
// anchor handed to writeFlux — in the live controller it is latched at
// motor-on (cpuCycleTotal*2, arbitrary mod 8) while the burst origin is
// latched at Q7-on, so anchor ≢ origin (mod cyc) is the NORMAL case. The
// hold-back seam test used to run on the anchor grid instead of the
// write-clock grid, and any anchor not congruent to the origin dropped one
// bit per flush seam (~345 of 353 data-field nibbles corrupted). Returns 0
// on success.
int writeAndCheck(const fs::path& img, int track, int startNib, int chunk,
                  int64_t anchor)
{
    DiskImage disk;
    if (!disk.loadFile(img.string())) {
        std::fprintf(stderr, "framing: load failed: %s\n",
                     disk.getLastError().c_str());
        return 1;
    }
    disk.setWriteBackEnabled(true);
    const int qt = track * 4;

    std::vector<uint8_t> before(DiskImage::kNibblesPerTrack);
    for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i)
        before[static_cast<size_t>(i)] =
            disk.nibbleAt(track, i);

    // Angular start of `startNib` on the padded timeline.
    std::vector<int> w;
    cellWidths(before, w);
    int64_t cell0 = 0;
    for (int i = 0; i < startNib; ++i) cell0 += w[static_cast<size_t>(i)];

    // The head lays the payload down with its OWN sync padding: a $FF in a
    // sync run takes 10 cells (DOS's 40-cycle write loop), everything else
    // 8. One transition per 1 bit, on the write clock.
    const std::vector<uint8_t> payload = dataField();
    std::vector<int> pw;
    cellWidths(payload, pw);
    std::vector<int64_t> tr;
    int64_t cell = cell0;
    for (size_t n = 0; n < payload.size(); ++n) {
        for (int b = 0; b < 8; ++b)
            if (payload[n] & (0x80 >> b)) tr.push_back((cell + b) * kCyc);
        cell += pw[n];
    }
    const int64_t startCycle = cell0 * kCyc;
    const int64_t endCycle   = cell * kCyc;

    int64_t winStart = startCycle;
    size_t i = 0;
    while (i < tr.size()) {
        const size_t end = std::min(i + static_cast<size_t>(chunk), tr.size());
        const int64_t winEnd = (end < tr.size()) ? tr[end] : endCycle;
        disk.writeFlux(qt, winStart, winEnd,
                       static_cast<int>(end - i), tr.data() + i, anchor);
        winStart = winEnd;
        i = end;
    }

    int rc = 0;
    for (size_t n = 0; n < payload.size(); ++n) {
        const int idx = (startNib + static_cast<int>(n))
                        % DiskImage::kNibblesPerTrack;
        const uint8_t got = disk.nibbleAt(track, idx);
        if (got != payload[n]) {
            std::fprintf(stderr,
                "framing FAIL (start %d, chunk %d, anchor %lld): nibble %zu "
                "of the data field read back $%02X, wrote $%02X\n",
                startNib, chunk, static_cast<long long>(anchor), n, got,
                payload[n]);
            rc = 2;
            break;
        }
    }
    for (int n = 0; n < DiskImage::kNibblesPerTrack && rc == 0; ++n) {
        const int rel = (n - startNib + DiskImage::kNibblesPerTrack)
                        % DiskImage::kNibblesPerTrack;
        if (rel < static_cast<int>(payload.size())) continue;
        if (disk.nibbleAt(track, n) != before[static_cast<size_t>(n)]) {
            std::fprintf(stderr,
                "framing FAIL (start %d, chunk %d, anchor %lld): nibble %d "
                "OUTSIDE the written window changed ($%02X → $%02X) — a "
                "sector write must not touch its neighbours\n",
                startNib, chunk, static_cast<long long>(anchor), n,
                before[static_cast<size_t>(n)], disk.nibbleAt(track, n));
            rc = 3;
        }
    }
    return rc;
}

// Bug hunt #7: a burst that re-arms a couple of cells after the last one
// ended is the same pass of the head (DOS 3.3's format loop drops Q7 for ~50
// CPU cycles between a sector's address field and its data field). The old
// code re-derived its angle from the revolution anchor and the cell-width map
// the first burst had just rewritten: the anchor sits `revs` revolutions back,
// the first burst changed the track's padded period, and `(t - anchor) mod
// period` moved by revs x dP -- so the second burst landed tens of nibbles
// away from where the head was and erased the field before it. Returns 0 when
// the second burst's first nibble lands exactly after the first burst's last.
int resumeAndCheck(const fs::path& img, int track, int startNib, int revs)
{
    DiskImage disk;
    if (!disk.loadFile(img.string())) {
        std::fprintf(stderr, "resume: load failed: %s\n",
                     disk.getLastError().c_str());
        return 1;
    }
    disk.setWriteBackEnabled(true);
    const int qt = track * 4;

    std::vector<uint8_t> before(DiskImage::kNibblesPerTrack);
    for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i)
        before[static_cast<size_t>(i)] = disk.nibbleAt(track, i);
    std::vector<int> w;
    cellWidths(before, w);
    int64_t cell0 = 0;
    for (int i = 0; i < startNib; ++i) cell0 += w[static_cast<size_t>(i)];
    const int64_t periodBefore = disk.trackPeriod(qt);

    auto burst = [&](const std::vector<uint8_t>& payload, int64_t startCycle,
                     int64_t& endCycle) {
        std::vector<int> pw;
        cellWidths(payload, pw);
        std::vector<int64_t> tr;
        int64_t cell = 0;
        for (size_t n = 0; n < payload.size(); ++n) {
            for (int b = 0; b < 8; ++b)
                if (payload[n] & (0x80 >> b))
                    tr.push_back(startCycle + (cell + b) * kCyc);
            cell += pw[n];
        }
        endCycle = startCycle + cell * kCyc;
        disk.writeFlux(qt, startCycle, endCycle, static_cast<int>(tr.size()),
                       tr.data(), /*anchor*/ 0);
    };

    // Burst 1: an address-field-sized stretch, `revs` revolutions after the
    // anchor. Burst 2: a data field, re-armed two cells after burst 1 ended.
    std::vector<uint8_t> first;
    for (int i = 0; i < 8; ++i) first.push_back(0xFF);
    first.push_back(0xD5); first.push_back(0xAA); first.push_back(0x96);
    for (int i = 0; i < 8; ++i) first.push_back(static_cast<uint8_t>(0xAA + 2 * i));
    first.push_back(0xDE); first.push_back(0xAA); first.push_back(0xEB);
    const std::vector<uint8_t> second = dataField();

    int64_t end1 = 0, end2 = 0;
    burst(first, revs * periodBefore + cell0 * kCyc, end1);
    const int64_t periodAfter = disk.trackPeriod(qt);
    burst(second, end1 + 2 * kCyc, end2);

    const int n1 = static_cast<int>(first.size());
    for (size_t n = 0; n < second.size(); ++n) {
        const int idx = (startNib + n1 + static_cast<int>(n))
                        % DiskImage::kNibblesPerTrack;
        const uint8_t got = disk.nibbleAt(track, idx);
        if (got != second[n]) {
            std::fprintf(stderr,
                "resume FAIL (start %d, %d revs, period %lld -> %lld): nibble "
                "%zu of the resumed burst read back $%02X at slot %d, wrote "
                "$%02X -- the burst was re-anchored instead of carried on\n",
                startNib, revs, static_cast<long long>(periodBefore),
                static_cast<long long>(periodAfter), n, got, idx, second[n]);
            return 4;
        }
    }
    for (int n = 0; n < n1; ++n) {
        const int idx = (startNib + n) % DiskImage::kNibblesPerTrack;
        if (disk.nibbleAt(track, idx) != first[static_cast<size_t>(n)]) {
            std::fprintf(stderr,
                "resume FAIL (start %d, %d revs): the resumed burst erased "
                "nibble %d of the field written just before it\n",
                startNib, revs, n);
            return 5;
        }
    }
    return 0;
}

}  // namespace

int main()
{
    const fs::path img = writeScratchDsk();
    int rc = 0;

    for (int startNib : {400, 1200, 3000}) {
        for (int revs : {0, 3, 20}) {
            if (const int r = resumeAndCheck(img, 17, startNib, revs); r != 0) {
                rc = r;
                goto done;
            }
        }
    }

    // Several start positions (inside a gap, inside a data field, and one
    // that wraps the end of the track buffer), both a chunked and a
    // single-shot flush, and revolution anchors that do / do not share the
    // write-clock grid's phase mod 8 (anchor 0 is the lucky congruent case
    // the old code passed; 1/3/5 are the normal live-controller case it
    // corrupted).
    for (int startNib : {400, 1200, 3000, 6500}) {
        for (int chunk : {30, 4, 100000}) {
            for (int64_t anchor : {int64_t{0}, int64_t{1}, int64_t{3},
                                   int64_t{5}}) {
                if (const int r = writeAndCheck(img, 17, startNib, chunk,
                                                anchor); r != 0) {
                    rc = r;
                    goto done;
                }
            }
        }
    }

done:
    std::error_code ec;
    fs::remove(img, ec);
    if (rc == 0) std::printf("disk_writeflux_framing OK\n");
    return rc;
}
