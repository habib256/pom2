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

// Header-only abstract interface so DiskIICard (and any other floppy
// peripheral) can notify a mechanical-sound consumer without dragging
// the miniaudio-based FloppySoundDevice TU into every test/headless
// build. Concrete implementation: FloppySoundDevice.

#ifndef POM2_FLOPPY_SOUND_SINK_H
#define POM2_FLOPPY_SOUND_SINK_H

#include <cstdint>

class FloppySoundSink
{
public:
    virtual ~FloppySoundSink() = default;
    /// Motor state changed. `withDisk`=true picks the loaded vs empty
    /// spin sample pair.
    virtual void motor(bool on, bool withDisk) = 0;
    /// Head moved one stepper pulse. `emuCycles` is the calling
    /// peripheral's emulated CPU cycle counter at the moment the head
    /// moved — used by the sound consumer to measure step cadence in
    /// **emulated** time, mirroring MAME's `machine().time()`-based
    /// rate in `floppy_sound_device::step` (floppy.cpp ~lines 1532-1620).
    /// Wall-clock audio frames are unsuitable: under POM2's disk turbo
    /// (~60× emulated speed) an entire phase sweep lands inside one
    /// audio buffer, so the audio thread sees gap=0 for every step
    /// after the first. `newTrack` is the destination quarter-track,
    /// kept for parity with MAME (per-track sample selection is unused
    /// for now since POM2 ships a single `step_1_1` sample).
    virtual void step(int newTrack, uint64_t emuCycles) = 0;
    /// One-shot mechanical click — used for disk insert / eject.
    virtual void click() = 0;

    /// Door latch. `inserting` is the lid closing; false is the lid opening.
    /// The MAME bank has a single click for both, which is what the default
    /// does. Virtual ][ ships two recordings and overrides this.
    virtual void latch(bool inserting) { (void)inserting; click(); }

    /// The drive itself reported a failed transfer (a SmartPort read or
    /// write that did not land). A Disk II never sees this: the guest
    /// checksums raw nibbles. Default is silence — only the Virtual ][
    /// bank has an "I-O Error" recording.
    virtual void ioError() {}
};

#endif // POM2_FLOPPY_SOUND_SINK_H
