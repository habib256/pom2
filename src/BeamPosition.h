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

// BeamPosition — where the video scanner is at a given beam-clock cycle, for
// the debugger's H/V readout (French Touch's request, 2026-11-25: "the beam's
// position on the page at instant T, like AppleWin's H and V").
//
// It is NOT a second video model. It is the derivation POM2's beam racing
// already stamps every soft-switch event with (Memory::pushVideoEventLocked,
// Apple2Display::frameCycleToPos): a 65-cycle line, 192 visible lines then
// vertical blanking up to 262 (NTSC) or 312 (PAL), the 40-byte window opening
// at horizontal cycle 25. Reading it off the same arithmetic is the point: the
// readout says where POM2 believes the beam is, which is what a developer
// racing it inside POM2 needs. Where POM2's beam stands against a real
// machine's is graded in Apple2Display.cpp (the -24 column phase, French
// Touch's MAD EFFECT) and not re-asserted here.
//
// H and V are the raw counters of that model: V = line in the frame (0-191
// visible), H = cycle in the line (0-64, the 40 bytes from 25).
// [D] AppleWin @ 7fd7fd7c numbers its debugger's "v:" / "h:" the same way by
// default (NTSC.cpp: VIDEO_SCANNER_HORZ_START 25, VIDEO_SCANNER_Y_DISPLAY
// 192), in hex unless set to decimal; its optional "real" H is this H - 13
// (Debugger_Display.cpp, DrawVideoScannerInfo). So a figure read in one
// debugger means the same in the other. Pinned by `disasm_navigation`.

#ifndef POM2_BEAM_POSITION_H
#define POM2_BEAM_POSITION_H

#include "CpuClock.h"

#include <cstdint>

namespace pom2 {

struct BeamPosition {
    int      line   = 0;     ///< V: 0 .. scanlinesPerFrame-1
    int      h      = 0;     ///< H: cycle within the line, 0..64
    int      column = -1;    ///< byte 0..39 being fetched, -1 in HBL or VBL
    bool     vbl    = false; ///< line >= 192
    bool     hbl    = false; ///< h < 25
    uint32_t frameCycle = 0; ///< cycle since the top of the frame
};

/// Horizontal blanking: the first 25 cycles of each 65-cycle line.
inline constexpr int kBeamHblCycles = 25;

inline BeamPosition beamPositionAt(uint64_t videoCycle, VideoStandard standard)
{
    const VideoTiming& t = pom2VideoTiming(standard);
    const uint64_t perLine  = static_cast<uint64_t>(t.cyclesPerScanline);
    const uint64_t perFrame = perLine * static_cast<uint64_t>(t.scanlinesPerFrame);
    BeamPosition b;
    b.frameCycle = static_cast<uint32_t>(videoCycle % perFrame);
    b.line   = static_cast<int>(b.frameCycle / perLine);
    b.h      = static_cast<int>(b.frameCycle % perLine);
    b.vbl    = b.line >= t.visibleScanlines;
    b.hbl    = b.h < kBeamHblCycles;
    b.column = (b.vbl || b.hbl) ? -1 : b.h - kBeamHblCycles;
    return b;
}

} // namespace pom2

#endif // POM2_BEAM_POSITION_H
