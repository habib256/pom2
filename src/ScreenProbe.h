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

// ScreenProbe — what is at a point of the screen: the question the Screen
// Inspector answers under the mouse. Pure arithmetic on a line's video
// switches, so it is tested without a window (`beam_view`).
//
// [B] The layouts are the Apple II's own: text and lo-res rows at
// $0400 + ((row & 7) << 7) + (row >> 3) * 40, hi-res lines at the offset
// Memory::hiResLineOffset gives, 7 pixels a byte, and in 80 columns / double
// hi-res the AUX byte left of the main byte (Apple IIe Technical Reference).

#ifndef POM2_SCREEN_PROBE_H
#define POM2_SCREEN_PROBE_H

#include <cstdint>

namespace pom2 {

struct ScreenProbe {
    const char* mode = "";   ///< "TEXT", "TEXT80", "LORES", "DLORES", "HIRES", "DHGR"
    bool     page2 = false;  ///< the page the scanner reads (80STORE pins page 1)
    int      x = 0;          ///< position in the line's own grid
    int      xMax = 0;       ///< last position of that grid (39, 79, 279, 559)
    int      row = 0;        ///< text / lo-res row (0-23), or the line for hi-res
    int      byteColumn = 0; ///< 0-39
    int      bit = -1;       ///< bit in the byte, -1 where a byte is one cell
    uint16_t address = 0;    ///< address of the byte
    bool     aux = false;    ///< the AUX half (80 columns, double modes)
};

/// Probe line `y` (0-191) at horizontal fraction `fx` (0 <= fx < 1) of the
/// picture, for a line displayed with these switches.
ScreenProbe probeScreen(bool textMode, bool mixedMode, bool hiRes, bool page2,
                        bool eightyCol, bool dhgr, bool eightyStore,
                        int y, double fx);

} // namespace pom2

#endif // POM2_SCREEN_PROBE_H
