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

// Port of MAME's Apple 3.5" Sony drive model (`applefdintf_device::add_35`
// + `floppy_image_device` SENSE / phase decoder). The "phases-as-command"
// protocol is documented in *Inside the Apple //gs* hardware reference
// chapter 6 ("Sony Drive"); MAME's authoritative source is in
// `src/devices/imagedev/mac_floppy.cpp::seek_phase_w` and
// `::wpt_r`. Where MAME would consult the loaded `floppy_image` for
// per-track state (write-protect tab, disk-in-drive switch, etc.), POM2
// queries the attached `Disk35Image`.
//
// Register table — MAME `mac_floppy_device` (imagedev/floppy.cpp), both
// directions indexed by { HDSEL, CA2, CA1, CA0 } (`m_reg = (phases & 7) |
// (m_actual_ss ? 8 : 0)` — bit 3 is the HEAD-SELECT line, NOT the IWM
// drive-select; an earlier POM2 revision wired SEL there and used a
// boot-tuned register layout that diverged from MAME on several entries):
//
//   addr  read SENSE (wpt_r)              write strobe (seek_phase_w)
//   ----  ------------------------------------------------------------
//   0x0   DIRTN (1 after DirPrev)         DirNext  (dir → cyl+1)
//   0x1   step done (always 1)            StepOn   (one step)
//   0x2   /MOTORON (0 = running)          MotorOn
//   0x3   disk changed since clear        EjectOff (no-op)
//   0x4   index pulse (MFM only → 0)      DirPrev  (dir → track 0)
//   0x5   superdrive capable (→ 0)        —
//   0x6   double-sided (800K → 1)         MotorOff
//   0x7   "drive exists" (→ 0)            EjectOn
//   0x8   disk present (1 = NO disk)      —
//   0x9   /WRTPRT (1 = not protected)     MFMModeOn  (GCR drive: no-op)
//   0xA   NOT track 0                     —
//   0xB   tachometer (unmodelled → 1)     —
//   0xC   index pulse (as 0x4)            DskchgClear
//   0xD   MFM mode active (→ 0)           GCRModeOn  (already GCR: no-op)
//   0xE   /READY (0 = ready)              —
//   0xF   1.4M "new interface" (→ 0)      —
//
// `senseR()` returns the raw line level exactly as MAME's wpt_r does.

#include "Sony35Drive.h"

#include "ByteIO.h"

#include "Sony35Gcr.h"
#include "CpuClock.h"
#include "Disk35Image.h"
#include "FloppySoundSink.h"
#include "Logger.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace pom2 {

namespace {

constexpr uint8_t kBitCA0   = 0x01;
constexpr uint8_t kBitCA1   = 0x02;
constexpr uint8_t kBitCA2   = 0x04;
constexpr uint8_t kBitLSTRB = 0x08;

// ── Sony zoned-recording geometry (MAME flopimg.cpp:2019-2027) ──────────
//
// Five concentric speed zones cover the 80 tracks. Each zone keeps the
// IWM bit-cell rate constant (~505 kHz) by spinning the platter slower
// at the outer zones. Cells per revolution × RPM × 60 / 1 µs ≈ constant.
//
// kCellsPerRev[zone] = 30318342 / RPM  (MAME's constant, ticks per cell)
// kRpm[zone]         = nominal spindle RPM at this zone
//
// The magic numerator is MAME's own `60.0 / 1.979e-6` (flopimg.cpp:2094) —
// one minute divided by the 1.979 µs bit cell. Written as the division
// rather than as literals because three of the five had drifted from it
// (zone 1 by 23 cells, zones 2 and 4 by one each), and `buildTrackBits`
// sizes the pregap as `cellsRev - 6208 * sectors`: a cells/rev that is not
// MAME's puts every sector of that zone at a different angle than the
// formatter MAME's own images came out of.
constexpr int kSonyCellNumerator = 30318342;
constexpr int kRpm[5]         = {   394,   429,   472,   525,   590 };
constexpr int kCellsPerRev[5] = {
    kSonyCellNumerator / kRpm[0],   // 76 950
    kSonyCellNumerator / kRpm[1],   // 70 672
    kSonyCellNumerator / kRpm[2],   // 64 233
    kSonyCellNumerator / kRpm[3],   // 57 749
    kSonyCellNumerator / kRpm[4],   // 51 387
};

// Per-zone CPU cycles per revolution. (60 / RPM) seconds × CPU clock.
// Pre-computed because the integer division is sensitive to ordering.
constexpr int64_t kCyclesPerRev[5] = {
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[0],   // 155 745
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[1],   // 143 038
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[2],   // 130 008
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[3],   // 116 883
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[4],   // 103 999
};

constexpr int sectorsForTrack35(int track) {
    return 12 - (track / 16);
}
constexpr int zoneForTrack(int track) {
    return std::min(track / 16, 4);
}

// MAME `flopimg.cpp:967-977 gcr6fw_tb` — 64-entry write-side GCR table.
// Maps 6-bit values to the 8-bit "disk bytes" the IWM writes (always
// has bit 7 set, no two adjacent zero bits — the recovery constraints
// the Disk II / IWM data separator needs).
//
// This file needs only the WRITE side. The read-side inverse table and the
// 4-disk-bytes → 3-raw-bytes decode that goes with it live in
// `Sony35Gcr.cpp` (`sony35::decodeSectors`), the only caller that ever
// wanted them. A second copy of both sat here entirely unused — a dead
// duplicate of a MAME-cited routine is worse than none, because it is the
// copy nobody remembers to correct when the original is.

constexpr uint8_t kGcr6fw[0x40] = {
    0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6,
    0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc,
    0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
    0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
    0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
    0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

// MAME `flopimg.cpp:512-520 gcr6_encode` — 3 raw bytes → 4 GCR bytes.
inline uint32_t gcr6Encode(uint8_t va, uint8_t vb, uint8_t vc)
{
    uint32_t r =
        (static_cast<uint32_t>(kGcr6fw[((va >> 2) & 0x30) |
                                       ((vb >> 4) & 0x0c) |
                                       ((vc >> 6) & 0x03)]) << 24) |
        (static_cast<uint32_t>(kGcr6fw[va & 0x3f]) << 16) |
        (static_cast<uint32_t>(kGcr6fw[vb & 0x3f]) <<  8) |
        (static_cast<uint32_t>(kGcr6fw[vc & 0x3f]));
    return r;
}

// MAME `flopimg.cpp:326 raw_w` — append `n` bit cells (MSB first) from
// `value` to the cell stream. For GCR every cell is the same period
// (the `size` parameter MAME passes is irrelevant on constant-rate
// zones), so we ignore it and just push 0/1 bits.
inline void rawW(std::vector<uint8_t>& cells, int n, uint32_t value)
{
    for (int i = n - 1; i >= 0; --i) {
        cells.push_back(static_cast<uint8_t>((value >> i) & 1));
    }
}

// Block index inside the `.po` payload for (track, head, logicalSec).
// MAME `apple_gcr_format::load` (ap_dsk35.cpp:366-386): blocks are
// laid out linearly by track, then by head, then by logical sector.
// On 800K the count per (track,head) is 12 - (track/16) blocks.
int blockIndexFor(int track, int head, int logicalSec)
{
    int idx = 0;
    for (int t = 0; t < track; ++t) {
        idx += 2 * sectorsForTrack35(t);
    }
    idx += head * sectorsForTrack35(track);
    return idx + logicalSec;
}

// Build the bit-cell stream for one (track, head) pair using the
// Sony 800K GCR layout. Verbatim port of MAME
// `flopimg.cpp:2017-2106 build_mac_track_gcr`. The output is one bit
// per cell, total = kCellsPerRev[zone].
void buildTrackBits(const Disk35Image& img,
                    int track, int head,
                    std::vector<uint8_t>& cells)
{
    const int zone     = zoneForTrack(track);
    const int sectors  = sectorsForTrack35(track);
    const int cellsRev = kCellsPerRev[zone];
    // Per MAME line 2034: pregap = cells_per_speed_zone - 6208 × sectors.
    const int pregap   = cellsRev - 6208 * sectors;

    cells.clear();
    cells.reserve(cellsRev);

    // Prepregap / pregap self-sync (MAME lines 2038-2049). The pattern
    // 0xff3fcf / 0xf3fcff is the two halves of the standard "10 cells
    // per $FF" self-sync run packed into 24-bit chunks.
    const int prepregap = pregap % 48;
    if (prepregap >= 24) {
        rawW(cells, prepregap - 24, 0xff3fcf);
        rawW(cells, 24,             0xf3fcff);
    } else {
        rawW(cells, prepregap,      0xf3fcff);
    }
    for (int i = 0; i < pregap / 48; ++i) {
        rawW(cells, 24, 0xff3fcf);
        rawW(cells, 24, 0xf3fcff);
    }

    // Per-sector loop. Sectors are laid out in physical order 0..ns-1
    // on disk; the address-field "sector number" carries the LOGICAL
    // sector that lives at this physical slot (MAME's 2:1 interleave
    // schedule, `apple_gcr_format::load` lines 372-382).
    auto physicalToLogical = [sectors](int phys) -> int {
        // Invert `si = (si + 2) % ns; if (si == 0) si++;` starting si=0.
        int si = 0;
        for (int logical = 0; logical < sectors; ++logical) {
            if (si == phys) return logical;
            si = (si + 2) % sectors;
            if (si == 0) si++;
        }
        return 0;  // unreachable on valid input
    };

    // Read all `sectors` blocks for this (track, head) into a flat
    // buffer up front — the encoder walks them in physical order so
    // grabbing them once avoids repeated random reads on the image.
    std::array<uint8_t, 12 * 512> sectorData{};
    for (int phys = 0; phys < sectors; ++phys) {
        const int logical = physicalToLogical(phys);
        const int blkIdx  = blockIndexFor(track, head, logical);
        img.readBlock(static_cast<uint32_t>(blkIdx),
                      sectorData.data() + phys * 512);
    }

    constexpr uint8_t kSonyFormatByte = 0x22;   // double-sided Mac/A2 800K
    for (int s = 0; s < sectors; ++s) {
        const int logicalSec = physicalToLogical(s);

        // 8× 48-cell self-sync gap before each sector (MAME 2052-2055).
        for (int i = 0; i < 8; ++i) {
            rawW(cells, 24, 0xff3fcf);
            rawW(cells, 24, 0xf3fcff);
        }

        // Address field (MAME 2057-2066).
        rawW(cells, 24, 0xd5aa96);
        const uint8_t tr  = static_cast<uint8_t>(track);
        const uint8_t sec = static_cast<uint8_t>(logicalSec);
        const uint8_t sid = static_cast<uint8_t>(
            ((tr & 0x40) ? 1 : 0) | (head ? 0x20 : 0));
        const uint8_t fmt = kSonyFormatByte;
        const uint8_t chk =
            static_cast<uint8_t>(tr ^ sec ^ sid ^ fmt);
        rawW(cells, 8, kGcr6fw[tr  & 0x3f]);
        rawW(cells, 8, kGcr6fw[sec & 0x3f]);
        rawW(cells, 8, kGcr6fw[sid & 0x3f]);
        rawW(cells, 8, kGcr6fw[fmt & 0x3f]);
        rawW(cells, 8, kGcr6fw[chk & 0x3f]);
        rawW(cells, 24, 0xdeaaff);

        // Inter-field self-sync (MAME 2068-2069).
        rawW(cells, 24, 0xff3fcf);
        rawW(cells, 24, 0xf3fcff);

        // Data field (MAME 2071-2103).
        rawW(cells, 24, 0xd5aaad);
        rawW(cells, 8,  kGcr6fw[sec & 0x3f]);

        const uint8_t* secBytes = sectorData.data() + s * 512;
        // MAME pre-pends 12 tag bytes (zero on Apple GCR loads — there
        // is no tag region in the .po format). Iterate 175 nibble
        // groups of 3 in 4-out; the first 4 groups consume the (zero)
        // tag bytes, the rest the 512-byte data payload.
        std::array<uint8_t, 12 + 512 + 1> dataWithTag{};
        std::memcpy(dataWithTag.data() + 12, secBytes, 512);
        // dataWithTag[524] is the implicit `vc = 0` for i==174.

        uint8_t ca = 0, cb = 0, cc = 0;
        for (int i = 0; i < 175; ++i) {
            const uint8_t va = dataWithTag[3 * i + 0];
            const uint8_t vb = dataWithTag[3 * i + 1];
            const uint8_t vc = (i != 174) ? dataWithTag[3 * i + 2] : 0;

            cc = static_cast<uint8_t>((cc << 1) | (cc >> 7));
            const uint16_t suma = static_cast<uint16_t>(ca + va + (cc & 1));
            ca = static_cast<uint8_t>(suma);
            const uint8_t vaX = static_cast<uint8_t>(va ^ cc);
            const uint16_t sumb = static_cast<uint16_t>(cb + vb + (suma >> 8));
            cb = static_cast<uint8_t>(sumb);
            const uint8_t vbX = static_cast<uint8_t>(vb ^ ca);
            if (i != 174) {
                cc = static_cast<uint8_t>(cc + vc + (sumb >> 8));
            }
            const uint8_t vcX = static_cast<uint8_t>(vc ^ cb);

            const uint32_t nb = (i != 174) ? 32u : 24u;
            const uint32_t enc = gcr6Encode(vaX, vbX, vcX);
            rawW(cells, static_cast<int>(nb), enc >> (32 - nb));
        }
        // Running checksum (3 bytes) packed as 4 GCR bytes.
        rawW(cells, 32, gcr6Encode(ca, cb, cc));
        // Data epilogue + pad (MAME 2102).
        rawW(cells, 32, 0xdeaaffff);
    }

    // The pregap was sized so the sum of pregap + N × 6208 equals
    // cellsRev exactly — but the prepregap rounding above can leave
    // the stream a few cells shy. Pad with self-sync if needed; trim
    // if a rounding accident overshot.
    while (static_cast<int>(cells.size()) < cellsRev) {
        cells.push_back(1);     // pad bit (single $FF cell)
    }
    if (static_cast<int>(cells.size()) > cellsRev) {
        cells.resize(static_cast<size_t>(cellsRev));
    }
}

}  // namespace

Sony35Drive::Sony35Drive()
{
    reset();
}

bool Sony35Drive::isInserted() const
{
    return image_ && image_->isLoaded();
}

int Sony35Drive::cellsPerRev() const
{
    return kCellsPerRev[zoneForTrack(track_)];
}

int64_t Sony35Drive::cyclesPerRev() const
{
    return kCyclesPerRev[zoneForTrack(track_)];
}

int64_t Sony35Drive::ticksPerRev() const
{
    return cyclesPerRev() * POM2_IWM_TICKS_PER_CPU_CYCLE;
}

void Sony35Drive::invalidateCache() const
{
    cachedTrack_ = -1;
    cachedHead_  = -1;
    cells_.clear();
    transitionCells_.clear();
    cachedCellsPerRev_   = 0;
    cachedCyclesPerRev_  = 0;
}

void Sony35Drive::rebuildTransitionsFromCells() const
{
    transitionCells_.clear();
    transitionCells_.reserve(cells_.size() / 4);
    for (int i = 0; i < static_cast<int>(cells_.size()); ++i) {
        if (cells_[i]) transitionCells_.push_back(i);
    }
}

std::vector<uint8_t> Sony35Drive::debugCellStream() const
{
    if (!isInserted() || track_ < 0 || track_ >= 80) return {};
    ensureCache();
    return cells_;
}

namespace {

}  // namespace

int Sony35Drive::decodeAndCommit(int track, int head) const
{
    // The GCR walk itself lives in `Sony35Gcr` — shared with the WOZ loader
    // in `Disk35Image`, which has to turn the same cells into the same
    // blocks when a flux image is mounted. What stays here is the part that
    // is about THIS drive: which track the head is on, and how a decoded
    // sector meets the mounted image.
    if (!image_ || !image_->isLoaded()) return 0;
    if (cells_.empty()) return 0;
    if (track < 0 || track >= 80) return 0;
    if (head  < 0 || head  >  1)  return 0;

    const auto nib = sony35::nibblesFromCells(cells_);

    int written = 0;
    sony35::decodeSectors(nib, /*expectTrack=*/track,
        [&](int tr, int sideByte, int sec, const uint8_t* data) {
            // Same argument as `expectTrack` (Sony35Gcr.h): the head is where
            // this drive physically IS, and a sector whose address field
            // claims the OTHER side cannot have been written by it. Without
            // this test a garbled side byte committed the far side's blocks —
            // 11 of them at track 20 — over data the guest could not reach,
            // while the side actually under the head kept its old contents.
            // `ensureCacheFor(track, head)` stamps every address field in
            // `cells_` with `head`, so for well-formed data this is a no-op,
            // exactly like the track filter.
            if (sideByte != head) return;
            const int blkIdx = sony35::blockIndexFor(tr, head, sec);
            if (blkIdx < 0 ||
                blkIdx >= static_cast<int>(Disk35Image::kBlockCount)) return;
            // Only write if the block actually differs — avoids dirtying
            // the image when the firmware just re-writes the same data.
            uint8_t existing[Disk35Image::kBlockBytes];
            if (image_->readBlock(blkIdx, existing) &&
                std::memcmp(existing, data, Disk35Image::kBlockBytes) == 0) {
                return;
            }
            if (image_->writeBlock(blkIdx, data)) ++written;
        });
    return written;
}

void Sony35Drive::writeStart()
{
    // MAME `floppy_image_device::write_start` (floppy.cpp:1261-1279), reached
    // from `iwm_device::write_clock_start` (iwm.cpp:335-343). Two things
    // happen there and both were missing here: the gate
    // `if(!m_image || m_mon) return;` (floppy.cpp:1263-1264) — no medium or
    // motor stopped means no write at all — and the LATCH of the destination
    // (`m_write_cyl = m_cyl; m_write_ss = m_ss;`, floppy.cpp:1273-1274) that
    // `write_do_flush` then commits to (floppy.cpp:1345). A phase strobe
    // reaches `seekPhaseW` without flushing the IWM, so without the latch a
    // head step between the last written flux and the flush spliced track 0's
    // bit stream into track 1's cells.
    writeCursorTick_ = INT64_MIN;   // a new arc: no cursor to continue
    if (!isInserted() || !motorOn_) { writeActive_ = false; return; }
    writeActive_ = true;
    writeTrack_  = track_;
    writeHead_   = side1_ ? 1 : 0;
}

void Sony35Drive::writeFlux(int64_t startTick, int64_t endTick,
                            const int64_t* fluxes, int count,
                            int64_t revStartTick, int64_t bitPeriodTicks)
{
    if (!isInserted()) return;
    // The destination is the (track, side) latched at write start, not the
    // one the head has reached by the time the controller flushes — see
    // `writeStart`. A caller that never armed one (a bench test splicing a
    // track by hand) gets the live head position.
    const int trk  = writeActive_ ? writeTrack_ : track_;
    const int head = writeActive_ ? writeHead_  : (side1_ ? 1 : 0);
    if (trk < 0 || trk >= 80) return;
    if (image_->isWriteProtected()) return;
    ensureCacheFor(trk, head);
    if (cells_.empty() || cachedCellsPerRev_ <= 0 ||
        cachedCyclesPerRev_ <= 0) {
        return;
    }
    if (endTick <= startTick) return;

    const int     n      = cachedCellsPerRev_;
    // Same timeline as the read side: IWM ticks (CpuClock.h).
    const int64_t period = static_cast<int64_t>(cachedCyclesPerRev_) *
                           POM2_IWM_TICKS_PER_CPU_CYCLE;
    // The controller's bit period. It is NOT the medium's cell period: the
    // IWM lays one bit every `2 × half_window_size()` ticks (iwm.cpp:303-313
    // → 14/16/28/32) while an outer-zone Sony cell is period/n = 14.17 ticks
    // wide. Rounding each flux stamp onto the cell grid therefore folded two
    // consecutive bits into one cell every ~84 bits — 1.2 % of every byte
    // ever written to a 3.5" disk silently deleted, and the sector it landed
    // in unreadable afterwards. A head lays bits down one after another; so
    // do we. Fall back to the cell period only for a caller with no
    // controller behind it.
    // A caller with no controller behind it (a bench harness splicing a
    // track it encoded itself) hands us stamps that ARE on the medium's cell
    // grid, and `period / n` truncates 14.17 to 14, which would drift a cell
    // every 84 bits — the very failure this argument exists to remove. So
    // that case keeps the exact grid mapping instead of counting bits.
    const bool    countBits = bitPeriodTicks > 0;
    const int64_t bitTicks  = countBits ? bitPeriodTicks
                                        : std::max<int64_t>(1, period / n);

    // Map tick → cell index inside one revolution (anchored on
    // `revStartTick`). Only the START of an arc uses this; every bit after
    // it is placed by counting, not by time.
    auto cycleToCell = [&](int64_t cy) -> int {
        int64_t rel = cy - revStartTick;
        rel %= period;
        if (rel < 0) rel += period;
        int cell = static_cast<int>(
            (rel * n + period / 2) / period);
        if (cell >= n) cell -= n;
        return cell;
    };

    // A continuous write reaches us as several windows (`IWMDevice::
    // flushWrite` sets `fluxWriteStart_ = when` and starts the next one
    // there). Re-anchoring each window on its start tick would make them
    // overlap, because bits advance at `bitTicks` and the anchor advances at
    // the cell period. Continue the arc instead whenever the window abuts
    // the previous one.
    const int cellStart =
        (writeCursorTick_ == startTick) ? writeCursorCell_
                                        : cycleToCell(startTick);

    // How many bit cells the controller laid down in this window. On the
    // grid-anchored path the window is measured on the medium's own grid,
    // so the span is the arc between its two cell indices.
    int64_t span;
    if (countBits) {
        span = (endTick - startTick + bitTicks / 2) / bitTicks;
    } else if (endTick - startTick >= period) {
        span = n;
    } else {
        const int cellEnd = cycleToCell(endTick);
        span = (cellEnd - cellStart + n) % n;
    }
    if (span < 0) span = 0;

    if (span >= n) {
        // A full revolution or more — clobber everything.
        std::fill(cells_.begin(), cells_.end(), 0);
    } else {
        for (int64_t k = 0; k < span; ++k)
            cells_[static_cast<std::size_t>((cellStart + k) % n)] = 0;
    }

    // Splice in the new flux transitions, one bit cell per controller bit.
    for (int i = 0; i < count; ++i) {
        if (!countBits) {
            // Grid-anchored: the stamp names its own cell exactly.
            cells_[static_cast<std::size_t>(cycleToCell(fluxes[i]))] = 1;
            continue;
        }
        int64_t k = (fluxes[i] - startTick + bitTicks / 2) / bitTicks;
        if (k < 0) k = 0;
        cells_[static_cast<std::size_t>((cellStart + k % n) % n)] = 1;
    }

    writeCursorTick_ = endTick;
    writeCursorCell_ = static_cast<int>((cellStart + span % n) % n);

    rebuildTransitionsFromCells();
    decodeAndCommit(trk, head);
}

void Sony35Drive::ensureCache() const
{
    ensureCacheFor(track_, side1_ ? 1 : 0);
}

void Sony35Drive::ensureCacheFor(int track, int head) const
{
    if (cachedTrack_ == track && cachedHead_ == head && !cells_.empty()) {
        return;
    }
    cells_.clear();
    transitionCells_.clear();
    cachedCellsPerRev_   = 0;
    cachedCyclesPerRev_  = 0;
    cachedTrack_ = track;
    cachedHead_  = head;
    if (!isInserted()) return;
    if (track < 0 || track >= 80) return;

    buildTrackBits(*image_, track, head, cells_);
    cachedCellsPerRev_  = static_cast<int>(cells_.size());
    cachedCyclesPerRev_ = kCyclesPerRev[zoneForTrack(track)];
    rebuildTransitionsFromCells();
}

int64_t Sony35Drive::nextTransition(int64_t fromTick,
                                    int64_t revStartTick) const
{
    ensureCache();
    if (transitionCells_.empty()) return INT64_MAX;

    // IWM ticks, seven per CPU cycle (CpuClock.h). At CPU-cycle resolution
    // this arithmetic quantised a 2.02-cycle cell to 2 or 3 whole cycles,
    // and the IWM's window walker lost the byte boundary within a sector.
    const int64_t period = static_cast<int64_t>(cachedCyclesPerRev_) *
                           POM2_IWM_TICKS_PER_CPU_CYCLE;
    const int     ncells = cachedCellsPerRev_;
    if (period <= 0 || ncells <= 0) return INT64_MAX;

    // Anchor the head: cell 0 was under the head at `revStartTick`. Find
    // the relative time inside the current revolution.
    int64_t rel = fromTick - revStartTick;
    int64_t revIdx = rel >= 0 ? (rel / period) : -((-rel + period - 1) / period);
    int64_t relInRev = rel - revIdx * period;
    if (relInRev < 0) { relInRev += period; --revIdx; }

    // Convert to cell space. We want the next transition STRICTLY
    // after `fromTick`, so look for the cell with `cellTime > relInRev`.
    auto cycleForCell = [period, ncells](int cellIdx) -> int64_t {
        // period × cellIdx stays inside 64-bit: period ≤ ~1.1 M ticks,
        // ncells ≤ ~77 k.
        return (static_cast<int64_t>(cellIdx) * period) / ncells;
    };

    // Binary-search the transition list for the first cell whose
    // cycle-time exceeds relInRev.
    int lo = 0, hi = static_cast<int>(transitionCells_.size());
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (cycleForCell(transitionCells_[mid]) > relInRev) hi = mid;
        else                                                lo = mid + 1;
    }

    if (lo < static_cast<int>(transitionCells_.size())) {
        return revStartTick + revIdx * period + cycleForCell(transitionCells_[lo]);
    }
    // No transition left in this revolution — first event of the next.
    return revStartTick + (revIdx + 1) * period + cycleForCell(transitionCells_[0]);
}

void Sony35Drive::reset()
{
    motorOn_       = false;
    writeActive_     = false;
    writeCursorTick_ = INT64_MIN;
    side1_         = false;
    sel_           = false;
    directionIn_   = false;   // MAME floppy.cpp:290 `m_dir(0)`
    // MAME floppy.cpp:560 device reset: `m_dskchg = exists() ? 1 : 0` —
    // the DSKCHG latch is HIGH when a disk sits in place (or the host
    // cleared the latch), LOW when the drive is empty / just ejected.
    // Sense register 3 returns it NEGATED (mac wpt_r `!m_dskchg`).
    dskchg_        = image_ && image_->isLoaded();
    track_         = 0;
    phases_        = 0;
    prevPhases_    = 0;
}

void Sony35Drive::setImage(Disk35Image* image)
{
    // image_ is the drive *slot* — a stable Disk35Image instance owned
    // by EmulationController. Mounting / ejecting media is a separate
    // event signalled via `notifyMediaChange()`.
    image_ = image;
    writeActive_     = false;
    writeCursorTick_ = INT64_MIN;
    invalidateCache();
}

void Sony35Drive::notifyMediaChange()
{
    // MAME: load() sets m_dskchg = 1 (floppy.cpp:672-673), unload()
    // sets it 0 (floppy.cpp:723). The old code set a "switched" flag on
    // BOTH events and returned it un-negated from sense reg 3, so an
    // EMPTY external drive read as "disk in place" — the //c+ firmware's
    // boot drive-scan then walked into the read-a-disk path of a drive
    // with no disk and hung the whole cold boot at $F0FC (no banner).
    dskchg_       = image_ && image_->isLoaded();
    // The medium under the head changed: any write arc in flight is void.
    writeActive_     = false;
    writeCursorTick_ = INT64_MIN;
    invalidateCache();
    pom2::log().info(
        "Sony35",
        std::string("media change ") +
            ((image_ && image_->isLoaded()) ? image_->path() : "(empty)"));
}

void Sony35Drive::monW(bool motorOffHigh)
{
    // MAME: m_floppy->mon_w(true) = motor STOP. The IWM calls this when it
    // leaves MODE_ACTIVE.
    //
    // Upstream would have this be a NO-OP for a 3.5" — `mac_floppy_device::
    // mon_w(int) { /* Motor control is through commands */ }`
    // (floppy.cpp:2835-2838) — because a Sony's motor answers only its own
    // MotorOn (0x2) / MotorOff (0x6) register strobes, not the controller's
    // enable line. **POM2 deliberately keeps the enable line wired**, and
    // bug hunt #4 tried removing it and put it back: `liron_boot35` fails
    // with "the drive's motor never ran" because POM2's Liron and //c+ paths
    // reach the mechanism through `IWMDevice`'s enable rather than through a
    // separately modelled MIG/strobe sequencer. Aligning with upstream here
    // needs that sequencer first; until then this line is what spins the
    // disk, and the divergence is recorded in TODO's parity dashboard.
    motorOn_ = !motorOffHigh;
}

void Sony35Drive::ssW(bool side1)
{
    if (side1_ != side1) {
        side1_ = side1;
        invalidateCache();
    }
}

void Sony35Drive::setSel(bool sel)
{
    sel_ = sel;
}

uint8_t Sony35Drive::regSelect() const
{
    // MAME: `m_reg = (phases & 7) | (m_actual_ss ? 8 : 0)` — bit 3 is
    // the HEAD-SELECT line (ssW / MIG HDSEL), NOT the IWM drive-select.
    // Wiring SEL here meant the head-1 register bank (disk present, WP,
    // track 0…) was only addressable while switching the active drive.
    uint8_t r = (phases_ & (kBitCA2 | kBitCA1 | kBitCA0));
    if (side1_) r |= 0x08;
    return r;
}

void Sony35Drive::seekPhaseW(uint8_t phases, uint64_t emuCycles)
{
    // MAME `mac_floppy.cpp::seek_phase_w`: latch the new phase bits,
    // then if LSTRB transitioned 0→1 fire `strobeWriteRegister` with
    // the current `regSelect()` address. The IWM is free to change
    // CA0/CA1/CA2 while LSTRB is held — MAME only fires the strobe on
    // the rising edge.
    prevPhases_ = phases_;
    phases_     = static_cast<uint8_t>(phases & 0x0F);
    lastStrobeCycle_ = emuCycles;
    const bool lstrbWasLow = !(prevPhases_ & kBitLSTRB);
    const bool lstrbNowHi  =  (phases_     & kBitLSTRB);
    if (lstrbWasLow && lstrbNowHi) {
        strobeWriteRegister(regSelect());
    }
}

void Sony35Drive::emitInsertClick()
{
    if (sound_) sound_->click();
}

void Sony35Drive::completeEject()
{
    if (!image_) return;
    image_->eject();
    dskchg_ = false;   // MAME unload(): m_dskchg = 0
    writeActive_     = false;
    writeCursorTick_ = INT64_MIN;
    if (sound_) sound_->click();
    pom2::log().info("Sony35", "eject requested by host");
}

void Sony35Drive::strobeWriteRegister(uint8_t reg)
{
    static const bool trace = std::getenv("POM2_TRACE_IWM_SENSE") != nullptr;
    if (trace)
        std::fprintf(stderr, "[STROBE] drv=%p reg=%X motor=%d trk=%d\n",
                     static_cast<const void*>(this), reg, motorOn_ ? 1 : 0,
                     track_);
    // Decode per the MAME `mac_floppy_device::seek_phase_w` table at the
    // top of this file. An earlier "boot-tuned" mapping put MotorOff on
    // reg 0x3 (MAME: EjectOff, a no-op — real MotorOff is 0x6) and used
    // strobes 0x8/0x9 as head select (the head is the ssW LINE, not a
    // register; MAME 0x9 is MFMModeOn). Firmware strobing the real
    // registers was silently ignored — or worse, killed the motor.
    switch (reg) {
        case 0x0: directionIn_ = false; break;       // DirNext: step toward cyl+1 (outward)
        case 0x1: {                                   // StepOn
            bool moved = false;
            if (directionIn_ && track_ > 0)  {
                --track_; invalidateCache(); moved = true;
            }
            if (!directionIn_ && track_ < 79) {
                ++track_; invalidateCache(); moved = true;
            }
            // Fire one mechanical-step sound per *actual* track motion
            // — head bumps at track 0 or 79 don't click on real
            // hardware. Stamp with the IWM strobe cycle so the
            // FloppySoundDevice can measure step cadence in emulated
            // time (parity with DiskIICard::seekPhaseW).
            if (moved && sound_) sound_->step(track_, lastStrobeCycle_);
            break;
        }
        case 0x2:                                     // MotorOn
            if (!motorOn_ && sound_) {
                sound_->motor(true, image_ && image_->isLoaded());
            }
            motorOn_ = true;
            break;
        case 0x3: break;                              // EjectOff — no-op (MAME)
        case 0x4: directionIn_ = true;  break;        // DirPrev: step toward cyl-1 (track 0)
        case 0x6:                                     // MotorOff
            if (motorOn_ && sound_) {
                sound_->motor(false, image_ && image_->isLoaded());
            }
            motorOn_ = false;
            break;
        case 0x7:                                      // EjectOn
            if (image_ && image_->isLoaded() && !ejectPending_) {
                // Flush guest write-back blocks before dropping the image —
                // Disk35Image::eject() clears blocks_ + dirty_ with no file
                // write, so without this a firmware-issued eject silently
                // loses everything written since the last save (the UI eject
                // path in EmulationController::eject35 already does this).
                //
                // Two-phase, because THIS path runs on the CPU worker inside
                // `stateMutex` (the IWM strobed it) and that lock must never
                // be held across file I/O — 800 KB and two fsyncs here froze
                // the machine and the window together. Phase 1 is a memcpy;
                // the host's sink commits it with the lock released. A drive
                // with no sink (headless, tests) writes inline, which is
                // correct there: nothing else is waiting on a lock.
                Disk35Image::PendingWriteBack pending = image_->takeWriteBack();
                if (pending.valid && writeBackSink_) {
                    // Queued, NOT saved. The medium stays in the bay until
                    // the sink reports: the pre-fix code ejected right here,
                    // so a commit that failed (disk full, read-only parent)
                    // only logged a line — the disk was already gone and its
                    // writes with it, while the sinkless branch below has
                    // always refused the eject for exactly that case.
                    ejectPending_ = true;
                    const std::string path = pending.path;
                    std::weak_ptr<int> alive = aliveToken_;
                    writeBackSink_->submit(
                        std::move(pending),
                        [this, alive, path](bool ok, const std::string& err) {
                            // The completion arrives under the machine lock
                            // but possibly a slot rebuild later: an expired
                            // token means this drive died with its card.
                            if (alive.expired()) return;
                            ejectPending_ = false;
                            // Only if the SAME medium is still in the bay —
                            // a mount that landed while the commit ran owns
                            // the drive now (same rule as
                            // EmulationController::eject35).
                            if (!image_ || !image_->isLoaded() ||
                                image_->path() != path)
                                return;
                            if (!ok) {
                                image_->restoreDirty();
                                pom2::log().warn(
                                    "Sony35", "eject refused: " + err);
                                return;
                            }
                            completeEject();
                        });
                    break;
                }
                if (pending.valid) {
                    std::string err;
                    if (!Disk35Image::commitWriteBack(std::move(pending),
                                                      err)) {
                        image_->restoreDirty();
                        pom2::log().warn("Sony35", "eject refused: " + err);
                        break;
                    }
                }
                completeEject();
            }
            break;
        case 0x9: break;                              // MFMModeOn — GCR-only drive
        case 0xC: dskchg_ = true; break;   // DskchgClear (MAME: m_dskchg = 1)
        case 0xD: break;                              // GCRModeOn — already GCR
        default:
            // Unmapped register — MAME logs but does nothing.
            break;
    }
}

bool Sony35Drive::senseR(uint64_t nowCycles) const
{
    const uint8_t reg = regSelect();
    const bool    v   = senseValue(reg, nowCycles);
    // Diagnostic: POM2_TRACE_IWM_SENSE=1 logs each (register, value)
    // CHANGE — the firmware polls sense in tight loops, so unconditional
    // logging would melt the console.
    static const bool trace = std::getenv("POM2_TRACE_IWM_SENSE") != nullptr;
    if (trace) {
        static const void* lastDrive = nullptr;
        static int lastReg = -1, lastV = -1;
        if (this != lastDrive || reg != lastReg || static_cast<int>(v) != lastV) {
            std::fprintf(stderr,
                         "[SENSE] drv=%p reg=%X v=%d motor=%d disk=%d trk=%d\n",
                         static_cast<const void*>(this), reg, v ? 1 : 0,
                         motorOn_ ? 1 : 0,
                         (image_ && image_->isLoaded()) ? 1 : 0, track_);
            lastDrive = this; lastReg = reg; lastV = v;
        }
    }
    return v;
}

bool Sony35Drive::senseValue(uint8_t reg, uint64_t nowCycles) const
{
    // MAME `mac_floppy_device::wpt_r` — raw line level per register (see
    // the table at the top of this file). Notable deltas from the old
    // boot-tuned map: DIRTN polarity is `m_dir` (1 after DirPrev), the
    // disk-change latch lives at 0x3 and is cleared by the DskchgClear
    // STROBE (0xC) — not by reading it — and a write-protect sense (0x9)
    // exists at all (it used to be missing entirely, so a WP image was
    // invisible to firmware and writes were silently dropped).
    switch (reg) {
        case 0x0:                                       // DIRTN
            return directionIn_;                        // 1 after DirPrev
        case 0x1:                                       // step done
            return true;
        case 0x2:                                       // /MOTORON — 0 = running
            return !motorOn_;
        case 0x3:                                       // disk-change / empty
            // MAME mac wpt_r: `return !m_dskchg;` — HIGH means "empty or
            // ejected since the last DskchgClear".
            return !dskchg_;
        case 0x4:                                       // index pulse (MFM-only)
        case 0xC:
            return false;
        case 0x5:                                       // superdrive capable
            return false;                               // 800K GCR drive
        case 0x6:                                       // double-sided
            return true;                                // 800K = 2 heads
        case 0x7:                                       // "drive exists" (MAME: false)
            return false;
        case 0x8:                                       // disk present — 1 = NO disk
            return !(image_ && image_->isLoaded());
        case 0x9:                                       // /WRTPRT — 1 = not protected
            return !(image_ && image_->isWriteProtected());
        case 0xA:                                       // NOT track 0
            return track_ != 0;
        case 0xB: {                                     // tachometer
            // MAME floppy.cpp:2711-2719: 120 inversions per rotation while a
            // disk spins, and FALSE with no medium or the motor stopped —
            // POM2 answered a hard-wired 1 in every state, i.e. "the platter
            // is turning" to anything that polls the tach on an empty bay.
            // 120 inversions = 60 full cycles per revolution, so one half
            // period is `cyclesPerRev() / 120`.
            if (!(image_ && image_->isLoaded()) || !motorOn_) return false;
            const int64_t rev = cyclesPerRev();
            const int64_t seg = rev / 60;
            if (seg <= 0) return false;
            return static_cast<int64_t>(nowCycles % static_cast<uint64_t>(rev))
                       % seg < seg / 2;
        }
        case 0xD:                                       // MFM mode active
            return false;                               // GCR only
        case 0xE:                                       // /READY — 0 = ready
            // A medium in the bay is READY. It does NOT also require the
            // spindle to be turning: the //c+ firmware's own probe strobes
            // MotorOff (register 6) and then waits on /READY before it will
            // re-enable the drive, so gating this on `motorOn_` deadlocked
            // that wait — the machine could not boot a 3.5" whose medium is
            // not write-protected, because the write-protected branch of the
            // ROM's $E974 gate skips the whole transfer, and every boot that
            // worked only worked through that branch. Ticking "write-back"
            // (what makes the medium writable) blanked the screen for good.
            // Deliberate divergence from MAME, whose m_ready follows mon_w
            // (bug hunt #9).
            return !(image_ && image_->isLoaded());
        case 0xF:                                       // 1.4M "new interface"
            return false;                               // 800K drive
        default:
            return true;
    }
}

// ─── Snapshot / rewind ────────────────────────────────────────────────────
// See the header for the contract (mechanism only, media excluded).

namespace {
constexpr uint32_t kSonySnapMagic   = 0x594E4F53u;   // 'SONY'
constexpr uint16_t kSonySnapVersion = 1;
/// Highest version this reader understands. The reader is TOLERANT — it
/// accepts anything in [1, kSonySnapVersionMax] and gates every field added
/// after v1 on `r.has()` — because a strict `!= kSonySnapVersion` makes the
/// NEXT bump reject every .pom2snap written by the shipped build, and the
/// user sees "MEX truncated" on a file that is not truncated. The cards
/// already read this way; the two device sections did not.
constexpr uint16_t kSonySnapVersionMax = kSonySnapVersion;
}  // namespace

void Sony35Drive::appendSnapshotState(std::vector<uint8_t>& out) const
{
    byteio::putU32(out, kSonySnapMagic);
    byteio::putU16(out, kSonySnapVersion);
    uint8_t flags = 0;
    if (motorOn_)      flags |= 0x01;
    // Bit 1 held a `writeProtect_` cache that no longer exists (it is asked
    // of the medium now — see the header). The bit still travels, carrying
    // the derived value, so v1 blobs stay byte-compatible in both directions.
    if (isWriteProtected()) flags |= 0x02;
    if (side1_)        flags |= 0x04;
    if (sel_)          flags |= 0x08;
    if (directionIn_)  flags |= 0x10;
    if (dskchg_)       flags |= 0x20;
    out.push_back(flags);
    out.push_back(static_cast<uint8_t>(track_ & 0xFF));
    out.push_back(phases_);
    out.push_back(prevPhases_);
    byteio::putU64(out, lastStrobeCycle_);
}

bool Sony35Drive::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    byteio::Reader r(data, len);
    if (!r.has(4 + 2 + 4 + 8)) return false;
    if (r.u32() != kSonySnapMagic) return false;
    const uint16_t version = r.u16();
    if (version == 0 || version > kSonySnapVersionMax) return false;

    const uint8_t flags = r.u8();
    motorOn_      = (flags & 0x01) != 0;
    // flags & 0x02 was the write-protect cache; the medium answers now.
    side1_        = (flags & 0x04) != 0;
    sel_          = (flags & 0x08) != 0;
    directionIn_  = (flags & 0x10) != 0;
    dskchg_       = (flags & 0x20) != 0;
    // 80 cylinders. Every cell-stream helper already bails on an
    // out-of-range track, but a restored 200 would silently make the drive
    // read nothing for the rest of the session.
    const int t = static_cast<int>(r.u8());
    track_       = (t >= 0 && t < 80) ? t : 0;
    phases_      = r.u8();
    prevPhases_  = r.u8();
    lastStrobeCycle_ = r.u64();
    // A restored machine is not mid-write: the arc that was open when the
    // snapshot was taken died with the controller state around it.
    writeActive_     = false;
    writeCursorTick_ = INT64_MIN;
    // The bit-cell cache is keyed on (track, side) and is a pure function of
    // the mounted image, so drop it rather than serialise ~100 KB per frame.
    invalidateCache();
    return true;
}

}  // namespace pom2
