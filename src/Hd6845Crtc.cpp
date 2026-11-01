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
    // [B] Hitachi HD6845S, U70 data book (1985) p. 451, "Reset (RES)": "All
    //     the counters in the CRTC are cleared ... 3) Control registers in
    //     the CRTC are not affected and remain unchanged"; p. 463: "internal
    //     registers shown in Table 1 (R0-R17) are not affected by RES and
    //     remain unchanged". So the cursor address R14/R15 SURVIVES (and
    //     R16/R17, never strobed here). This class has no counters to clear.
    // [D] MAME mc6845.cpp:1265-1291 @ mame0287 zeroes the cursor, latch and
    //     light pen under its own "TODO: none of these should reset unless
    //     otherwise proven" — contradicted by the datasheet, not followed.
    // [F] The address register (AR) is not in R0-R17 and the datasheet does
    //     not say what RES does to it; it is cleared as MAME does. Nothing
    //     observes it: both Videx firmwares write AR before every access.
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

double Hd6845Crtc::fieldRateHz(double charClockHz) const
{
    // [B] Hitachi HD6845S, U70 p. 456-457 (Fig. 8, Table 9) — in interlace
    //     sync & video mode the even field outputs the even raster addresses
    //     and the odd field the odd ones, so a field carries rasterCount()/2
    //     rasters of each character row; p. 460, Fig. 13 — each field has its
    //     own R5 adjust rasters, and an interlaced frame its half (dummy)
    //     raster. Interlace sync alone (R8 = x1, V = 0) scans every raster in
    //     each field. A field is therefore, in lines:
    //       non-interlace           (R4+1)·RN     + R5
    //       interlace sync          (R4+1)·RN     + R5 + 1/2
    //       interlace sync & video  (R4+1)·RN / 2 + R5 + 1/2
    //     The UltraTerm's 80x48 table (R0=122, R4=57, R5=1, R9=7, R8=3) gives
    //     262.5 lines, 59.98 Hz — the NTSC field rate a monitor locks to —
    //     where frameRateHz() gives MAME's 30.1 Hz two-field frame.
    // [D] MAME mc6845.cpp:674-776 @ mame0287 calls update_cursor_state once
    //     per vertical cycle, i.e. once per FRAME in this mode; its opt-in
    //     m_interlace_fields (0b45ad90c8, mos8563 only) is the field count.
    const bool interlaced = (r_[8] & 0x01) != 0;
    const double rows = static_cast<double>((vertTotal() + 1) * rasterCount());
    const double lines = (interlaceAndVideo() ? rows / 2.0 : rows)
                       + static_cast<double>(vertAdjust())
                       + (interlaced ? 0.5 : 0.0);
    const double chars = static_cast<double>(horizTotal() + 1);
    if (horizTotal() == 0 || vertTotal() == 0 || lines <= 0.0 || chars <= 0.0)
        return 0.0;
    return charClockHz / (chars * lines);
}

double Hd6845Crtc::frameRateHz(double charClockHz) const
{
    // One frame = (R0+1) characters per line × ((R4+1) rows × rasterCount()
    // + R5 adjust) lines. rasterCount() carries the interlace adjust: +1
    // non-interlaced, +2 in interlace-and-video (mc6845.cpp:311,1278-1280).
    // Without it the UltraTerm's 80x48 mode reported 33.9 Hz against MAME's
    // 30.1, skewing the cursor blink period by ~12 %.
    const double lines =
        static_cast<double>((vertTotal() + 1) * rasterCount() + vertAdjust());
    const double chars = static_cast<double>(horizTotal() + 1);
    if (horizTotal() == 0 || vertTotal() == 0 || lines <= 0.0 || chars <= 0.0)
        return 0.0;
    return charClockHz / (chars * lines);
}

bool Hd6845Crtc::cursorBlinkOn(uint64_t fieldIndex) const
{
    // [B] Hitachi HD6845S, U70 p. 454, Table 7 and its figure: "Blink Period"
    //     = light + dark under ONE brace, "16 or 32 Field Period"; the same
    //     drawing in HLN014 (HD46505R) Table 6-1 note 5 and Table 6-3. So a
    //     16-field blink is 8 fields lit and 8 dark: bit 3 (16) or bit 4 (32)
    //     of the FIELD counter.
    // [D] MAME mc6845.cpp:885-913 @ mame0287 toggles every 16/32 frames
    //     (the Motorola MC6845 sheet's Fig. 15 puts "16 or 32 Times Field
    //     Period" on the lit phase alone) — a different chip's drawing.
    // [F] Hitachi's own Fig. 15 note (p. 461), "changed into display or
    //     non-display mode when field period is 16 or 32-time period", can be
    //     read either way, and no HD6845S has been measured. The phase at
    //     field 0 (dark first, as MAME starts) is unknown and unobservable.
    switch (cursorMode()) {
        case 0:  return true;                              // steady
        case 1:  return false;                             // off
        case 2:  return ((fieldIndex >> 3) & 1u) != 0;     // 16-field period
        default: return ((fieldIndex >> 4) & 1u) != 0;     // 32-field period
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
    const int maxRas = rasterCount() - 1;   // mc6845.cpp:584, adjust included
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
