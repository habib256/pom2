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

#include "VidexVideotermCard.h"

#include <algorithm>
#include <cmath>

namespace {
// Snapshot blob: magic, version, bank quarter, CRTC state, VRAM. The ROMs
// never travel (SlotPeripheral.h contract).
constexpr uint8_t kMagic[4] = { 'V', 'D', 'X', 'T' };
constexpr uint8_t kVersion  = 1;
constexpr std::size_t kHeader = 4 + 1 + 1;
constexpr std::size_t kBlobBytes =
    kHeader + pom2::Hd6845Crtc::kStateBytes + VidexVideotermCard::kVramBytes;
} // namespace

VidexVideotermCard::VidexVideotermCard(int slot) : slot_(slot)
{
    // MAME device_start (:359-365): VRAM zeroed. The EPROM region is
    // ROMREGION_ERASEFF (:45) until a dump is loaded.
    rom_.fill(0xFF);
    for (std::size_t i = 0; i < kCharSetBytes; ++i) charAlt_[i] = 0xFF;
}

bool VidexVideotermCard::loadFirmware(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != kRomBytes && bytes.size() != kRomBytes / 2) return false;
    rom_.fill(0xFF);
    std::copy(bytes.begin(), bytes.end(), rom_.begin());
    firmwareLoaded_ = true;
    return true;
}

bool VidexVideotermCard::loadCharRoms(const std::vector<uint8_t>& normal,
                                      const std::vector<uint8_t>& alternate)
{
    if (normal.size() != kCharSetBytes) return false;
    if (!alternate.empty() && alternate.size() != kCharSetBytes) return false;
    std::copy(normal.begin(), normal.end(), charNormal_.begin());
    if (alternate.empty()) {
        // 4.ic4.bin == ~normal.bin on rasters 0-8 of every glyph (checked
        // against the MAME set), so the inverse is an exact stand-in for
        // what the stock card displays.
        for (std::size_t i = 0; i < kCharSetBytes; ++i)
            charAlt_[i] = static_cast<uint8_t>(~charNormal_[i]);
        altDerived_ = true;
    } else {
        std::copy(alternate.begin(), alternate.end(), charAlt_.begin());
        altDerived_ = false;
    }
    return true;
}

uint8_t VidexVideotermCard::deviceSelectRead(uint8_t low4)
{
    // a2videoterm.cpp:382-392 — the bank follows the address of EVERY
    // access; only offset 1 drives the bus (the 6845 register). Offset 0 is
    // not a status register on this card.
    selectBank(low4);
    if (low4 == 1) return crtc_.registerRead();
    return openBus();
}

void VidexVideotermCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // a2videoterm.cpp:399-411. RS is A0 for offsets 0/1 only: $C0n2/3,
    // $C0n6… reach nothing on the 6845 but still move the bank.
    if (low4 == 0)      crtc_.addressWrite(v);
    else if (low4 == 1) crtc_.registerWrite(v);
    selectBank(low4);
}

uint8_t VidexVideotermCard::slotRomRead(uint8_t low8)
{
    return rom_[0x300u + low8];   // a2videoterm.cpp:417-420
}

uint8_t VidexVideotermCard::expansionRomRead(uint16_t offset)
{
    // a2videoterm.cpp:444-459.
    if (offset < 0x400) return rom_[offset];
    if (offset < 0x600)
        return vram_[static_cast<std::size_t>((offset & 0x1FF) + bank_)];
    return openBus();
}

void VidexVideotermCard::expansionRomWrite(uint16_t offset, uint8_t v)
{
    // a2videoterm.cpp:464-471 — only the VRAM window is writable.
    if (offset >= 0x400 && offset < 0x600)
        vram_[static_cast<std::size_t>((offset & 0x1FF) + bank_)] = v;
}

void VidexVideotermCard::onReset()
{
    // device_reset (:367-370) puts the window back on bank 0; reset_from_bus
    // (:372-375) resets the 6845. POM2 has one reset hook for both.
    bank_ = 0;
    crtc_.reset();
}

uint64_t VidexVideotermCard::crtcFrameAt(uint64_t emuCycles, double cpuHz) const
{
    double hz = crtc_.frameRateHz(kCharClockHz);
    if (!(hz > 0.0)) hz = 60.0;   // registers not programmed yet
    if (!(cpuHz > 0.0)) return 0;
    const double frames = std::floor(static_cast<double>(emuCycles) * hz / cpuHz);
    return frames > 0.0 ? static_cast<uint64_t>(frames) : 0;
}

void VidexVideotermCard::paintPicture(uint32_t* dst, uint32_t lit,
                                      uint32_t dark, uint64_t emuCycles,
                                      double cpuHz) const
{
    // The whole frame, the way MAME's draw_scanline (mc6845.cpp:815-852)
    // walks it: the display start address is latched at the top, each
    // character row covers R9+1 rasters, and the row address advances by R1
    // after the last raster of a row. Each raster is crtc_update_row
    // (a2videoterm.cpp:473-500). What lies outside R1 × R6 rows is black,
    // as outside the display-enable window of a real CRTC.
    std::fill(dst, dst + kPictureWidth * kPictureHeight, dark);

    const int rasters = crtc_.maxRaster() + 1;
    const int cols    = std::min(crtc_.horizDisplayed(), kPictureWidth / kCellWidth);
    const int rows    = crtc_.vertDisplayed();
    if (cols <= 0 || rows <= 0) return;

    const bool blink = crtc_.cursorBlinkOn(crtcFrameAt(emuCycles, cpuHz));
    uint16_t lineAddr = crtc_.startAddress();
    for (int row = 0; row < rows; ++row) {
        const int y0 = row * rasters;
        if (y0 >= kPictureHeight) break;
        for (int ra = 0; ra < rasters; ++ra) {
            const int y = y0 + ra;
            if (y >= kPictureHeight) break;
            int cursorX = -1;
            if (!crtc_.cursorVisible(ra, lineAddr, blink, cursorX)) cursorX = -1;
            uint32_t* p = dst + static_cast<std::size_t>(y) * kPictureWidth;
            const std::size_t glyphRow = static_cast<std::size_t>(ra & 0x0F);
            for (int i = 0; i < cols; ++i) {
                const uint8_t chr = vram_[static_cast<std::size_t>((lineAddr + i) & 0x7FF)];
                const auto& set = (chr & 0x80) ? charAlt_ : charNormal_;
                uint8_t data = set[static_cast<std::size_t>(chr & 0x7F) * 16 + glyphRow];
                uint32_t fg = lit, bg = dark;
                if (i == cursorX) std::swap(fg, bg);
                // Bits 7..0 then bit 0 once more: `data = (data << 1) |
                // (data & 1)` shifts the low bit in behind itself.
                for (int j = 0; j < kCellWidth; ++j) {
                    *p++ = (data & 0x80) ? fg : bg;
                    data = static_cast<uint8_t>((data << 1) | (data & 1));
                }
            }
        }
        lineAddr = static_cast<uint16_t>((lineAddr + crtc_.horizDisplayed()) & 0x3FFF);
    }
}

void VidexVideotermCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(kVersion);
    out.push_back(static_cast<uint8_t>(bank_ / 512));
    crtc_.saveState(out);
    out.insert(out.end(), vram_.begin(), vram_.end());
}

void VidexVideotermCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // Refuse a foreign, older or short blob WHOLE: nothing is applied until
    // every field has been validated (card_snapshot_contract P2/P3).
    if (!data || len < kBlobBytes) return;
    if (!std::equal(kMagic, kMagic + 4, data) || data[4] != kVersion) return;
    const uint8_t bank = data[5];
    if (bank > 3) return;
    pom2::Hd6845Crtc crtc;
    if (!crtc.loadState(data + kHeader, pom2::Hd6845Crtc::kStateBytes)) return;
    crtc_ = crtc;
    bank_ = bank * 512;
    const uint8_t* v = data + kHeader + pom2::Hd6845Crtc::kStateBytes;
    std::copy(v, v + kVramBytes, vram_.begin());
}
