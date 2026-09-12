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

// DiskImage::writeFlux — revolution-anchor + padded-timeline re-pack test.
//
// Pins two write-path bugs against the MAME reference semantics
// (`imagedev/floppy.cpp` write_flux ~:1050-1095 / find_position
// ~:1100-1125):
//
//   1. ANCHOR: MAME's write_flux maps start / end / every transition
//      through `find_position(base, when)`, anchored on
//      `m_revolution_start_time` — the SAME anchor get_next_transition
//      uses on the read side. POM2's writeFlux used a raw
//      `startLssCycle % period` reduction, so a drive whose revolution
//      anchor wasn't a multiple of the track period spliced its writes
//      at an angular offset the read path never reported. writeFlux now
//      takes the `revolutionStart` anchor (same convention as
//      getNextTransition: < 0 = unanchored).
//
//   2. RE-PACK: the non-WOZ cell→nibble re-pack assumed 8 cells per
//      nibble, but the cell timeline `expandTrackBits` builds (and that
//      trackPeriod / fluxEvents / getNextTransition expose) pads +2 zero
//      cells per sync $FF (runs ≥ 5). On a stock 16-sector .dsk track
//      every sector's gaps carry ~19 sync $FFs, so `cell / 8` drifted
//      ~4.75 nibbles per sector and a sector rewrite corrupted the
//      neighbouring address field. The re-pack must walk the padded
//      timeline of the existing track.
//
// Method: build a zero-filled 16-sector .dsk, locate physical sector 5's
// data-field payload on track 0, splice a full 343-nibble replacement
// through writeFlux at a NON-ZERO revolution anchor (several revolutions
// in), and assert (a) exactly the payload nibbles changed, (b) every
// other nibble of the track — address fields, prologues, epilogues,
// gaps — is bit-identical to the pre-write track.

#include "DiskImage.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

constexpr int kNib = DiskImage::kNibblesPerTrack;

std::string writeZeroDsk()
{
    const auto p = fs::temp_directory_path() / "pom2_writeflux_anchor.dsk";
    std::vector<uint8_t> img(DiskImage::kBytesPerImage, 0x00);
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp .dsk");
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return p.string();
}

uint8_t decode44(uint8_t a, uint8_t b)
{
    return static_cast<uint8_t>(((a << 1) | 1) & b);
}

// Snapshot of one whole track's nibbles via the public nibbleAt API.
std::vector<uint8_t> snapshotTrack(const DiskImage& img, int track)
{
    std::vector<uint8_t> out(kNib);
    for (int i = 0; i < kNib; ++i)
        out[i] = img.nibbleAt(track, i);
    return out;
}

// Per-nibble cell-start offsets in the PADDED cell timeline — an
// independent re-implementation of the documented expansion rule
// (8 data cells per nibble; +2 trailing zero cells for each $FF inside
// a run of >= 5 contiguous $FFs). Pins DiskImage's own walk.
std::vector<int> cellStarts(const std::vector<uint8_t>& trk)
{
    auto isSync = [&](int i) {
        if (trk[i] != 0xFF) return false;
        int run = 1;
        for (int j = 1; j < 5; ++j) {
            if (trk[(i - j + kNib) % kNib] != 0xFF) break;
            ++run;
        }
        for (int j = 1; j < 5; ++j) {
            if (trk[(i + j) % kNib] != 0xFF) break;
            ++run;
        }
        return run >= 5;
    };
    std::vector<int> starts(kNib + 1, 0);
    int c = 0;
    for (int i = 0; i < kNib; ++i) {
        starts[i] = c;
        c += isSync(i) ? 10 : 8;
    }
    starts[kNib] = c;
    return starts;
}

// One run of the splice. `revAnchor` is the revolution anchor handed to
// writeFlux AND baked into the absolute timestamps (`revAnchor +
// revolutions*period + angular`); `revAnchor < 0` exercises the
// unanchored fallback (timestamps are then plain angular offsets).
// `chunkSize > 0` replays DiskIICard::lssSync's flush cadence: the
// transition list is committed in windows of ~chunkSize transitions,
// each window starting where the previous ended — so nearly every
// boundary lands mid-nibble (and mid-cell). Pins the straddling-nibble
// MERGE rule: a nibble cut by a window edge must combine the previous
// chunk's cells with this one's instead of being dropped (the drop left
// one stale nibble per flush mid-data-field → GCR checksum error).
bool runSplice(int64_t revAnchor, int revolutions, const char* label,
               int chunkSize = 0)
{
    DiskImage img;
    const std::string dsk = writeZeroDsk();
    if (!img.loadFile(dsk)) {
        std::printf("FAIL[%s]: loadFile: %s\n", label,
                    img.getLastError().c_str());
        return false;
    }

    const auto before = snapshotTrack(img, 0);

    // Locate physical sector 5's address field, then its data field.
    int addrIdx = -1;
    for (int i = 0; i < kNib; ++i) {
        if (before[i] == 0xD5 && before[(i + 1) % kNib] == 0xAA &&
            before[(i + 2) % kNib] == 0x96) {
            const uint8_t sec = decode44(before[(i + 7) % kNib],
                                         before[(i + 8) % kNib]);
            if (sec == 5) { addrIdx = i; break; }
        }
    }
    if (addrIdx < 0) {
        std::printf("FAIL[%s]: no address field for sector 5\n", label);
        return false;
    }
    int dataIdx = -1;
    for (int i = addrIdx; i < addrIdx + 64; ++i) {
        if (before[i % kNib] == 0xD5 && before[(i + 1) % kNib] == 0xAA &&
            before[(i + 2) % kNib] == 0xAD) {
            dataIdx = i % kNib;
            break;
        }
    }
    if (dataIdx < 0) {
        std::printf("FAIL[%s]: no data prologue after sector 5\n", label);
        return false;
    }
    const int payloadIdx = dataIdx + 3;          // 343 payload nibbles
    constexpr int kPayload = 343;
    assert(payloadIdx + kPayload < kNib && "payload wraps; unexpected layout");

    // Replacement payload: valid-looking nibbles (bit 7 set), no $FFs so
    // the splice cannot create new sync runs.
    std::vector<uint8_t> pattern(kPayload);
    for (int i = 0; i < kPayload; ++i)
        pattern[i] = static_cast<uint8_t>(0x80 | ((i * 7 + 0x25) & 0x7E));

    const auto starts = cellStarts(before);
    const int period = img.trackPeriod(0);
    if (period != starts[kNib] * 8) {
        std::printf("FAIL[%s]: trackPeriod %d != padded timeline %d×8 — "
                    "test's sync rule diverged from DiskImage's\n",
                    label, period, starts[kNib]);
        return false;
    }

    // Absolute LSS-cycle base for angular position 0 of revolution
    // `revolutions` past the anchor.
    const int64_t origin = (revAnchor >= 0) ? revAnchor : 0;
    const int64_t base   = origin +
                           static_cast<int64_t>(revolutions) * period;

    const int firstCell = starts[payloadIdx];
    const int lastCell  = starts[payloadIdx + kPayload];   // exclusive
    std::vector<int64_t> transitions;
    for (int i = 0; i < kPayload; ++i) {
        const int cs = starts[payloadIdx + i];
        for (int b = 0; b < 8; ++b) {
            if (pattern[i] & (0x80 >> b)) {
                transitions.push_back(base +
                    static_cast<int64_t>(cs + b) * 8 + 4);  // cell centre
            }
        }
    }
    if (chunkSize <= 0) {
        img.writeFlux(0,
                      base + static_cast<int64_t>(firstCell) * 8,
                      base + static_cast<int64_t>(lastCell) * 8,
                      static_cast<int>(transitions.size()),
                      transitions.data(), revAnchor);
    } else {
        // DiskIICard cadence: window N+1 starts where window N ended;
        // the end of an intermediate window is just past its last
        // transition (mid-nibble in general).
        int64_t wstart = base + static_cast<int64_t>(firstCell) * 8;
        size_t  i      = 0;
        while (i < transitions.size()) {
            const size_t j = std::min(i + static_cast<size_t>(chunkSize),
                                      transitions.size());
            const int64_t wend = (j < transitions.size())
                ? transitions[j - 1] + 1
                : base + static_cast<int64_t>(lastCell) * 8;
            img.writeFlux(0, wstart, wend, static_cast<int>(j - i),
                          &transitions[i], revAnchor);
            wstart = wend;
            i = j;
        }
    }

    if (!img.hasUnsavedChanges()) {
        std::printf("FAIL[%s]: writeFlux changed nothing\n", label);
        return false;
    }

    // (a) payload updated in place; (b) everything else untouched.
    const auto after = snapshotTrack(img, 0);
    for (int i = 0; i < kPayload; ++i) {
        if (after[payloadIdx + i] != pattern[i]) {
            std::printf("FAIL[%s]: payload nibble %d = %02X, want %02X "
                        "(splice landed at the wrong angle / drifted)\n",
                        label, i, after[payloadIdx + i], pattern[i]);
            return false;
        }
    }
    for (int i = 0; i < kNib; ++i) {
        if (i >= payloadIdx && i < payloadIdx + kPayload) continue;
        if (after[i] != before[i]) {
            std::printf("FAIL[%s]: nibble %d corrupted: %02X → %02X "
                        "(adjacent field / gap clobbered — payload at "
                        "%d..%d)\n",
                        label, i, before[i], after[i],
                        payloadIdx, payloadIdx + kPayload - 1);
            return false;
        }
    }
    std::printf("[ OK ] %s: sector-5 data field rewritten in place "
                "(payload @%d, %zu transitions, anchor %lld, rev %d)\n",
                label, payloadIdx, transitions.size(),
                static_cast<long long>(revAnchor), revolutions);
    return true;
}

}  // namespace

int main()
{
    bool ok = true;
    // Anchored, several revolutions past a non-period-multiple anchor —
    // the raw `start % period` reduction would land 1234567 % period
    // cells away and both assertions blow up.
    ok &= runSplice(/*revAnchor=*/1'234'567, /*revolutions=*/3, "anchored");
    // Anchor that IS angularly aligned with zero: isolates the padded
    // re-pack from the anchor math.
    ok &= runSplice(/*revAnchor=*/0, /*revolutions=*/2, "anchor-zero");
    // Unanchored fallback (revolutionStart = -1, angular timestamps):
    // the legacy convention must keep working for the IWM shadow path.
    ok &= runSplice(/*revAnchor=*/-1, /*revolutions=*/0, "unanchored");
    // DiskIICard flush cadence: ~30-transition chunks, boundaries
    // mid-nibble — pins the straddling-nibble merge (one stale nibble
    // per flush before the fix).
    ok &= runSplice(/*revAnchor=*/1'234'567, /*revolutions=*/3,
                    "anchored-chunked", /*chunkSize=*/30);
    if (ok) {
        std::printf("disk_writeflux_anchor OK\n");
        return 0;
    }
    return 1;
}
