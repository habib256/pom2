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

#include "ScreenProbe.h"

#include <algorithm>

namespace pom2 {

namespace {
constexpr uint16_t hiResOffset(int y)
{
    return static_cast<uint16_t>(((y & 7) << 10) | (((y >> 3) & 7) << 7) | ((y >> 6) * 40));
}
}  // namespace

ScreenProbe probeScreen(bool textMode, bool mixedMode, bool hiRes, bool page2,
                        bool eightyCol, bool dhgr, bool eightyStore,
                        int y, double fx)
{
    y  = std::clamp(y, 0, 191);
    fx = std::clamp(fx, 0.0, 0.999999);
    ScreenProbe p;
    p.page2 = page2 && !eightyStore;
    const bool text   = textMode || (mixedMode && y >= 160);
    const bool doubled = eightyCol && (text || dhgr);

    if (text || !hiRes) {
        const int row = y >> 3;
        p.row  = row;
        p.mode = text ? (doubled ? "TEXT80" : "TEXT") : (doubled ? "DLORES" : "LORES");
        p.xMax = doubled ? 79 : 39;
        p.x    = static_cast<int>(fx * (p.xMax + 1));
        p.byteColumn = doubled ? p.x / 2 : p.x;
        p.aux  = doubled && (p.x % 2 == 0);
        p.address = static_cast<uint16_t>((p.page2 ? 0x0800 : 0x0400) +
                                          ((row & 7) << 7) + (row >> 3) * 40 + p.byteColumn);
        return p;
    }
    p.row  = y;
    p.mode = doubled ? "DHGR" : "HIRES";
    p.xMax = doubled ? 559 : 279;
    p.x    = static_cast<int>(fx * (p.xMax + 1));
    const int byteIndex = p.x / 7;
    p.bit  = p.x % 7;
    p.byteColumn = doubled ? byteIndex / 2 : byteIndex;
    p.aux  = doubled && (byteIndex % 2 == 0);
    p.address = static_cast<uint16_t>((p.page2 ? 0x4000 : 0x2000) + hiResOffset(y) + p.byteColumn);
    return p;
}

} // namespace pom2
