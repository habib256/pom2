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
#include "FloppySoundSink.h"

void DiskIICard::advanceCycles(int cycles)
{
    // A caller's batch can straddle the spindle-stop deadline. Catch up
    // only to that deadline while the motor is still running, then advance
    // the remaining CPU time with the motor stopped. Otherwise the LSS
    // processes extra flux/PROM states, making later reads depend on the
    // caller's batch size (even after lssStart resets the read register).
    if (motorOffDelay > 0 && cycles > motorOffDelay) {
        const int remaining = cycles - motorOffDelay;
        advanceCycles(motorOffDelay);
        advanceCycles(remaining);
        return;
    }

    cpuCycleTotal += static_cast<uint64_t>(cycles);

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
            if (motorOn) legacyAdvance(cycles);
        } else {
            // MAME wozfdc: catch up the LSS to the new CPU time. lss_sync
            // skips when active == MODE_IDLE so a stopped drive is cheap.
            lssSync();
        }
    }

    // Tick the spin-down delay. When it expires the motor truly stops
    // (active drops to MODE_IDLE).
    if (motorOffDelay > 0) {
        motorOffDelay -= cycles;
        if (motorOffDelay <= 0) {
            motorOffDelay = 0;
            motorOn       = false;
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
                // POM2's equivalent: clear the anchor so a subsequent
                // motor-on event re-zeros it from the new spin-up.
                revolutionStartLssCycle[activeDrive] = kNeverRev;
            }
            if (sound_) sound_->motor(false, images[activeDrive].isLoaded());
        }
    }
}
