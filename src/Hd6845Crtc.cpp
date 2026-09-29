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

#include "Hd6845Crtc.h"

namespace pom2 {

uint8_t Hd6845Crtc::writeMask(int n)
{
    // MAME mc6845.cpp:237-252.
    switch (n) {
        case 0: case 1: case 2: case 3: case 8:  return 0xFF;
        case 4: case 6: case 7:                  return 0x7F;
        case 5: case 9: case 11:                 return 0x1F;
        case 10:                                 return 0x7F;
        case 12: case 14:                        return 0x3F;   // (d & 0x3f) << 8
        case 13: case 15:                        return 0xFF;
        default:                                 return 0x00;   // R16/R17 read-only
    }
}

void Hd6845Crtc::reset()
{
    // MAME mc6845.cpp:1036-1054 (device_reset): "internal registers other
    // than status remain unchanged" — except the cursor address, the address
    // latch and the light-pen address, which it zeroes.
    r_[14] = 0;
    r_[15] = 0;
    r_[16] = 0;
    r_[17] = 0;
    latch_ = 0;
}

uint8_t Hd6845Crtc::registerRead() const
{
    // MAME mc6845.cpp:192-212. The HD6845S reads back the display start
    // address (`m_supports_disp_start_addr_r = true`, :1214); every register
    // below R12 is write-only and reads 0. Reading R16/R17 would clear the
    // light-pen strobe flag, which nothing here ever sets.
    switch (latch_) {
        case 12: case 13: case 14: case 15: case 16: case 17:
            return r_[latch_];
        default:
            return 0;
    }
}

void Hd6845Crtc::registerWrite(uint8_t v)
{
    // MAME mc6845.cpp:215-270. R16/R17 are read-only, R18/R19 (transparent
    // update address) do not exist on the HD6845S, R20-R31 decode nothing.
    if (latch_ >= kRegisters) return;
    const uint8_t mask = writeMask(latch_);
    if (!mask) return;
    r_[latch_] = static_cast<uint8_t>(v & mask);
}

double Hd6845Crtc::frameRateHz(double charClockHz) const
{
    // One frame = (R0+1) characters per line × ((R4+1) rows × (R9+1)
    // rasters + R5 adjust) lines; the HD6845S has no interlace adjust in
    // non-interlaced mode beyond the +1 (mc6845.cpp:1210-1222).
    const double lines =
        static_cast<double>((vertTotal() + 1) * (maxRaster() + 1) + vertAdjust());
    const double chars = static_cast<double>(horizTotal() + 1);
    if (horizTotal() == 0 || vertTotal() == 0 || lines <= 0.0 || chars <= 0.0)
        return 0.0;
    return charClockHz / (chars * lines);
}

bool Hd6845Crtc::cursorBlinkOn(uint64_t frameIndex) const
{
    switch (cursorMode()) {
        case 0:  return true;                              // steady
        case 1:  return false;                             // off
        case 2:  return ((frameIndex >> 4) & 1u) != 0;     // 1/16 field rate
        default: return ((frameIndex >> 5) & 1u) != 0;     // 1/32 field rate
    }
}

bool Hd6845Crtc::cursorVisible(int ra, uint16_t lineAddr, bool blinkOn,
                               int& cursorX) const
{
    // HD6845S check_cursor_visible, mc6845.cpp:546-569.
    if (!blinkOn) return false;
    const int cursor = cursorAddress();
    if (cursor < lineAddr || cursor >= lineAddr + horizDisplayed())
        return false;
    const int start = cursorStart();
    const int maxRas = maxRaster();   // + noninterlace adjust (1) - 1
    if (start > maxRas || start > cursorEnd()) return false;
    if (ra < start || ra > cursorEnd()) return false;
    cursorX = cursor - lineAddr;      // mc6845.cpp:826
    return true;
}

void Hd6845Crtc::saveState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), r_.begin(), r_.end());
    out.push_back(latch_);
}

bool Hd6845Crtc::loadState(const uint8_t* p, std::size_t len)
{
    if (!p || len < kStateBytes) return false;
    for (int n = 0; n < kRegisters; ++n)
        if (p[n] & static_cast<uint8_t>(~writeMask(n))) return false;
    if (p[kRegisters] > 0x1F) return false;
    for (int n = 0; n < kRegisters; ++n) r_[static_cast<std::size_t>(n)] = p[n];
    latch_ = p[kRegisters];
    return true;
}

} // namespace pom2
