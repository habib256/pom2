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

#include "DiskIICard.h"

#include <cmath>
#include "FloppySoundSink.h"

// Where the platter of `drive` stands, kept for when its spindle turns again
// — a spindle resumes where it stopped, it does not snap back to index.
// [D] MAME floppy.cpp:871-876 @ b2a182f0b7: mon_w(true) saves
// m_stopped_position on EVERY on→off, and wozfdc.cpp:269-290 calls it on the
// old drive of a $C0nA/$C0nB swap. The swap used to skip it, so a two-drive
// copy re-entered each drive at a stale angle (index, if it had never
// coasted to a stop) — bug hunt 2026-11-07. A drive whose anchor is already
// cleared keeps what it had.
void DiskIICard::rememberStoppedPosition(int drive)
{
    if (revolutionStartLssCycle[drive] == kNeverRev) return;
    const int period = images[drive].trackPeriod(headQuarterTrack[drive]);
    stoppedPosition[drive] = period > 0
        ? static_cast<int64_t>((lssCycle - static_cast<uint64_t>(
              revolutionStartLssCycle[drive])) % static_cast<uint64_t>(period))
        : 0;
}

void DiskIICard::advanceCycles(int cycles)
{
    // A caller's batch can straddle the spindle-stop deadline. Catch up
    // only to that deadline while the motor is still running, then advance
    // the remaining CPU time with the motor stopped. Otherwise the LSS
    // processes extra flux/PROM states, making later reads depend on the
    // caller's batch size (even after lssStart resets the read register).
    // The deadline in CPU cycles: motorOffDelay is stock time (setCpuClock).
    // The ratio is bus ticks per CPU cycle: below 1 under an accelerator,
    // ABOVE 1 under a FASTChip set below 1 MHz (0.2 MHz: 5). Only ratio 1
    // means "the same count"; it used to be `ratio >= 1`, which put a slow
    // CPU's deadline up to 5x late, and the batch ran the disk past the stop
    // (bug hunt 2026-10-31).
    const double ratio = motorClock_.ratio();
    if (motorOffDelay > 0) {
        const int toDeadline = ratio == 1.0 ? motorOffDelay
            : static_cast<int>(std::ceil(motorOffDelay / ratio));
        if (cycles > toDeadline) {
            const int remaining = cycles - toDeadline;
            advanceCycles(toDeadline);
            advanceCycles(remaining);
            return;
        }
    }

    // The LSS, the spindle and the stepper run on the card's 2 MHz Q3 and
    // the disk's own rotation — wall-clock, not the 6502's. An accelerator
    // only shortens the CPU cycle, so the timeline below is in STOCK bus
    // cycles (MAME's wozfdc runs on machine time). The ratio follows each
    // chunk's speed, slow windows included (applyAcceleratorClock).
    const uint64_t stock = motorClock_.busTicks(static_cast<uint64_t>(cycles));
    cpuCycleTotal += stock;

    // Idle drive on the bit-level path (motor off AND spin-down elapsed):
    // nothing below would do anything — lssSync() returns on MODE_IDLE, the
    // spin-down countdown is over — so skip straight out. A plugged Disk II
    // is the common case and its drive is idle most of the time, yet this is
    // called once per emulated instruction through the slot fan-out.
    if (useBitLss && active == MODE_IDLE && motorOffDelay <= 0) return;

    // The DATA path (nibble/LSS) only runs with media present. The motor
    // spin-down delay, however, must tick REGARDLESS of media: a motor-off on
    // an empty selected drive still has to time out and stop, otherwise
    // motorOn / active stay stuck on forever (drive LED never clears, the
    // spin-down sound never fires). The early `return` here used to skip the
    // countdown below whenever the active drive had no disk.
    if (images[activeDrive].isLoaded()) {
        if (!useBitLss) {
            if (motorOn) legacyAdvance(static_cast<int>(stock));
        } else {
            // MAME wozfdc: catch up the LSS to the new CPU time. lss_sync
            // skips when active == MODE_IDLE so a stopped drive is cheap.
            lssSync();
        }
    }

    // Tick the spin-down delay. When it expires the motor truly stops
    // (active drops to MODE_IDLE).
    if (motorOffDelay > 0) {
        motorOffDelay -= static_cast<int>(stock);
        if (motorOffDelay <= 0) stopSpindle();
    }
}

void DiskIICard::stopSpindle()
{
    motorOffDelay = 0;
    motorOn       = false;
    if (cardTrace()) {                   // the spindle really stops (DiskIICard.h)
        flushSeekRun();
        traceLine("DISK S" + std::to_string(busSlot()) + ",D" +
                  std::to_string(activeDrive + 1) + " motor stopped");
    }
    if (active == MODE_DELAY) {
        active = MODE_IDLE;
        // Commit any in-flight write splice BEFORE invalidating
        // the revolution anchor: the transitions in writeBuffer
        // were captured under THIS spin's timeline, so they must
        // be reduced with THIS spin's anchor (MAME mon_w(true)
        // keeps writes and m_revolution_start_time coherent).
        // Deferring to the eventual Q7L flush would pair the
        // old-timeline buffer with kNeverRev — the exact
        // mixed-anchor corruption the writeFlux anchor exists
        // to eliminate.
        DiskImage& img = images[activeDrive];
        if (writeMode && writeBackEnabled && img.isLoaded()
            && writePosition > 0 && !writingInhibited()) {
            img.writeFlux(headQuarterTrack[activeDrive],
                          writeStartTime,
                          static_cast<int64_t>(lssCycle),
                          writePosition, writeBuffer,
                          revolutionStartLssCycle[activeDrive]);
            ++writeFlushCount;
        }
        // Whether or not it was spliced: a burst the gate refused
        // (notch, phase 1, write-back off) is timed against the spin
        // that just ended, and a later Q7L would splice it against
        // the cleared anchor — onto a disk un-notched meanwhile.
        writePosition  = 0;
        writeStartTime = static_cast<int64_t>(lssCycle);
        // MAME `floppy_image_device::mon_w(true)`: when the
        // controller actually stops driving the spindle, the
        // drive's revolution timestamp becomes `attotime::never`.
        // POM2's equivalent: clear the anchor — after remembering
        // WHERE the platter stopped ([D] MAME `bc9aec5a`, 2026-10-03):
        // a spindle resumes where it stopped, it does not snap back
        // to index. `docs/audits/parite-floppy-2026-11-01.md`.
        rememberStoppedPosition(activeDrive);
        revolutionStartLssCycle[activeDrive] = kNeverRev;
    }
    if (sound_) sound_->motor(false, images[activeDrive].isLoaded());
}

// ── The angle survives a head step ──────────────────────────────────────
// A disk turns as one piece: when the head moves to another track, the same
// RADIUS of the platter is under it, so the head is the same FRACTION of a
// revolution past the index on the new track as it was on the old one.
//
// [B] WOZ 2.1 reference, "Cross-Track Synchronization"
//     (applesaucefdc.com/woz/reference2/, fetched 2026-11-05) — "you need to
//     start the new bitstream at the same relative bitstream position …
//     account for the fact that track lengths are inconsistent on a disk due
//     to fluctuations in drive speed … new_position = current_position *
//     new_track_length / current_track_length. Remember to maintain the bit
//     position even when on an empty track (TMAP value of 0xFF) … using a
//     fake length of 51,200 bits (6400 bytes) works very well."
// [D] MAME imagedev/floppy.cpp find_position @ mame0287 — gets the same
//     result for free: every track is stretched to one fixed `m_rev_time`,
//     so an offset in TIME is already a fraction of a turn.
//
// POM2 is not MAME here. It gives each bit cell a fixed duration, so a
// track's period is its cell count times that duration and the tracks of one
// WOZ do NOT share a revolution time. The angle was kept as
// `(now - anchor) mod period`, an offset in cycles, and the anchor was left
// alone by a step — so the offset carried over, not the fraction. From 90 %
// of a 50,000-cell track the head arrived at 86.5 % of its 52,000-cell
// neighbour: 3.5 % of a turn, some 220 nibbles, in the wrong place. Software
// that times one track against another read a different sector than on the
// machine. Until 2026-11-05 the comment at `getNextTransition` claimed
// parity with `find_position`; the arithmetic matched, the premise did not.
//
// The anchor (or, with the spindle stopped, the remembered position) is
// re-based so the fraction holds. Rounded to nearest, not truncated: a seek
// is up to 160 steps and a floor would walk the platter backwards by half a
// cycle each. No state is added, so snapshots are unchanged. Two tracks of
// one length are left alone, which is every .dsk/.do/.po as loaded; once the
// guest writes to such an image its tracks drift apart by a few cells (the
// period is the PADDED cell count, and a write lays its own sync runs down),
// and the carry then applies there too, by ratios within 0.1 % of 1.
void DiskIICard::carryAngleAcrossStep(int drive, int fromQt, int toQt)
{
    if (!validDrive(drive)) return;
    const DiskImage& img = images[drive];
    if (!img.isLoaded()) return;
    const auto periodOf = [&img](int qt) -> int64_t {
        const int p = img.trackPeriod(qt);
        return p > 0 ? p : kEmptyTrackPeriodLss;
    };
    const int64_t from = periodOf(fromQt), to = periodOf(toQt);
    if (from == to) return;
    const auto rescale = [from, to](int64_t pos) {
        pos %= from;
        if (pos < 0) pos += from;
        return ((pos * to + from / 2) / from) % to;
    };
    int64_t& anchor = revolutionStartLssCycle[drive];
    if (anchor == kNeverRev) {
        stoppedPosition[drive] = rescale(stoppedPosition[drive]);
        return;
    }
    const int64_t now = static_cast<int64_t>(lssCycle);
    if (now < anchor) return;
    const int64_t pos = rescale(now - anchor);
    // In the first revolution after power-on `now` can be smaller than the
    // new position, and the anchor it would take is negative — which reads as
    // `kNeverRev`, the same limit the spin-up resume has. The angle is left
    // as it was for that one step; nothing moves the head off track 0 that
    // early (the boot's recalibration pushes it against the stop).
    if (now >= pos) anchor = now - pos;
}

// D2S1 v6: the write framing of each captured drive. A rewind restores the
// nibbles and the controller's burst (v4) but left `writeFraming` holding
// the abandoned future: the next flush took the window for a new burst,
// threw the half-framed nibble away and re-framed mid-nibble, so the rest
// of the data field was garbage (bug hunt 2026-11-07).
void DiskIICard::appendFramingTail(std::vector<uint8_t>& out) const
{
    for (int d = 0; d < kDriveCount; ++d) {
        // The media capture's predicate (appendSnapshotState).
        const bool cap = images[d].isLoaded() &&
                         !images[d].isFileWriteProtected() && !images[d].isWoz();
        out.push_back(cap ? 1 : 0);
        if (cap) images[d].appendWriteFraming(out);
    }
}

bool DiskIICard::framingTailFits(const uint8_t* data, std::size_t len)
{
    std::size_t q = 0;
    for (int d = 0; d < kDriveCount; ++d) {
        if (q >= len || data[q] > 1) return false;
        if (data[q++]) {
            if (DiskImage::kWriteFramingBytes > len - q) return false;
            q += DiskImage::kWriteFramingBytes;
        }
    }
    return true;
}

void DiskIICard::loadFramingTail(const uint8_t* data, std::size_t len,
                                 const bool mediaApplied[])
{
    std::size_t q = 0;
    for (int d = 0; d < kDriveCount; ++d) {
        const bool has = data && q < len && data[q++] != 0;
        if (has && mediaApplied[d] &&
            images[d].loadWriteFraming(data + q, DiskImage::kWriteFramingBytes)) {
            q += DiskImage::kWriteFramingBytes;
            continue;
        }
        if (has) q += DiskImage::kWriteFramingBytes;
        if (mediaApplied[d]) images[d].resetWriteFraming();
    }
}
