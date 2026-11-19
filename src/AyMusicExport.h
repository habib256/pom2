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

// AyMusicExport — a Mockingboard's AY-3-8910 register writes, captured with
// their bus-cycle stamps, written out as VGM or YM (AUDIO-013).
//
// The capture is EXACT by construction: the card already stamps every AY
// register store with the bus cycle it happened on (the emuCycles rule, the
// same stamp the audio thread replays), so the file holds the writes the
// program made, where it made them — not a resampling of the sound.
//
// VGM ([B] VGM file format specification 1.71, vgmrips.net "VGM
// Specification"): 44 100 Hz sample positions, command $A0 aa dd writes AY
// register aa (bit 7 of aa = the second chip), the header's AY8910 clock at
// $74 carries bit 30 for a dual-chip file. Both of a Mockingboard's AYs fit.
// A cycle lands on sample floor((cycle - start) * 44100 / busHz).
//
// YM ([B] Leonard/Oxygene's YM format, ST-Sound "YM5!" layout): ONE chip,
// sixteen registers per video frame, interleaved, uncompressed (ST-Sound's
// loader takes a raw file as well as an LHA one). Inherently lossy: the
// state at each frame's end, and R13 = $FF in a frame that did not write it
// (the format's "do not retrigger the envelope"). Use VGM for exact timing.
//
// An AY /RESET is recorded as `kResetReg` and written out as fourteen zero
// stores, which is what the reset does to registers 0-13 ([D] MAME
// ay8910.cpp ay8910_reset_ym @ mame0287, Ay3_8910.h).

#ifndef POM2_AY_MUSIC_EXPORT_H
#define POM2_AY_MUSIC_EXPORT_H

#include <cstdint>
#include <string>
#include <vector>

namespace pom2::aymusic {

/// A recorded /RESET strobe in place of a register number.
constexpr uint8_t kResetReg = 0xFF;
/// VGM's fixed sample clock.
constexpr uint32_t kVgmRate = 44100;

struct Write {
    uint64_t cycle;   // bus cycle, the card's `busNow_`
    uint8_t  chip;    // 0 or 1
    uint8_t  reg;     // 0-15, or kResetReg
    uint8_t  val;
};

struct Capture {
    double   busHz          = 1022727.0;   // the AYs' pin-22 clock = the bus clock
    int      cyclesPerFrame = 17030;       // video frame, for YM
    int      chips          = 2;
    uint64_t startCycle     = 0;
    uint64_t endCycle       = 0;
    uint8_t  initial[2][16] = {};          // each chip's registers at the start
    std::vector<Write> writes;             // in cycle order
};

/// The 44.1 kHz sample a cycle lands on.
uint64_t sampleAt(const Capture& c, uint64_t cycle);

/// A complete VGM 1.71 file: the initial registers at sample 0, every write
/// at its sample, the length to `endCycle`.
std::vector<uint8_t> encodeVgm(const Capture& c);

/// A complete uncompressed YM5 file of one chip (0 or 1).
std::vector<uint8_t> encodeYm5(const Capture& c, int chip,
                               const std::string& title = "POM2 capture");

} // namespace pom2::aymusic

#endif // POM2_AY_MUSIC_EXPORT_H
