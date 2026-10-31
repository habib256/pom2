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

// VidexUltratermCard implementation — a2ultraterm.cpp line references in
// the header.

#include "VidexUltratermCard.h"

#include <algorithm>
#include <cmath>

namespace {
// Snapshot blob: magic, version, ctrl1, ctrl2, 512-byte block, CRTC, VRAM.
// The ROMs never travel (SlotPeripheral.h contract).
constexpr uint8_t kMagic[4] = { 'V', 'D', 'X', 'U' };
constexpr uint8_t kVersion  = 1;
constexpr std::size_t kHeader = 4 + 1 + 3;
constexpr std::size_t kBlobBytes =
    kHeader + pom2::Hd6845Crtc::kStateBytes + VidexUltratermCard::kVramBytes;

uint32_t scaled(uint32_t c, int num, int den)
{
    uint32_t out = c & 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8)
        out |= ((((c >> sh) & 0xFFu) * uint32_t(num) / uint32_t(den)) & 0xFFu) << sh;
    return out;
}
} // namespace

bool VidexUltratermCard::loadRoms(const std::vector<uint8_t>& firmware,
                                  const std::vector<uint8_t>& chars)
{
    if (firmware.size() != kRomBytes || chars.size() != kCharBytes) return false;
    std::copy(firmware.begin(), firmware.end(), rom_.begin());
    std::copy(chars.begin(), chars.end(), chars_.begin());
    return true;
}

uint8_t VidexUltratermCard::deviceSelectRead(uint8_t low4)
{
    selectBlock(low4);
    switch (low4 & 3) {
    case 1: return crtc_.registerRead();
    case 2: return ctrl1_;
    case 3: return ctrl2_;
    default: return openBus();
    }
}

void VidexUltratermCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4 & 3) {
    case 0: crtc_.addressWrite(v); break;
    case 1: crtc_.registerWrite(v); break;
    case 2: ctrl1_ = v; break;     // write_ctrl1: the page comes from ctrl1 itself
    case 3: ctrl2_ = v; break;
    }
    selectBlock(low4);
}

uint8_t VidexUltratermCard::slotRomRead(uint8_t low8)
{
    return rom_[std::size_t(slot_) * 0x100 + low8];
}

uint8_t VidexUltratermCard::expansionRomRead(uint16_t offset)
{
    if (offset < 0x400 || (ctrl1_ & kMemSel))
        return rom_[0x800u + (offset & 0x7FF)];
    return vram_[windowIndex(offset)];
}

void VidexUltratermCard::expansionRomWrite(uint16_t offset, uint8_t v)
{
    if (offset >= 0x400)
        vram_[windowIndex(offset)] = v;
}

void VidexUltratermCard::onReset()
{
    // reset_from_bus: write_ctrl1(0) and the 6845's reset. Control 2 stays.
    ctrl1_ = 0;
    block512_ = 0;
    crtc_.reset();
}

int VidexUltratermCard::pictureWidth() const
{
    return std::clamp(crtc_.horizDisplayed() * kCellWidth, 0, kMaxWidth);
}

int VidexUltratermCard::pictureHeight() const
{
    return std::clamp(crtc_.vertDisplayed() * (crtc_.maxRaster() + 1), 0, kMaxHeight);
}

void VidexUltratermCard::paintPicture(uint32_t* dst, uint32_t lit, uint32_t dark,
                                      uint64_t emuCycles, double cpuHz) const
{
    const int w = pictureWidth();
    const int h = pictureHeight();
    std::fill(dst, dst + std::size_t(w) * std::size_t(h), dark);
    const int cols = w / kCellWidth;
    const int rasters = crtc_.maxRaster() + 1;
    if (cols <= 0 || h <= 0) return;

    // Grey levels of crtc_update_row: 0 dark, 1 lowlight, 3 highlight.
    const uint32_t level[4] = { dark, scaled(lit, 3, 4), scaled(lit, 3, 4), lit };
    const double charHz = ((ctrl1_ & kClkSel) ? kHighClockHz : kLowClockHz) / kCellWidth;
    double hz = crtc_.frameRateHz(charHz);
    if (!(hz > 0.0)) hz = 60.0;
    const uint64_t frame = cpuHz > 0.0
        ? uint64_t(std::floor(double(emuCycles) * hz / cpuHz)) : 0;
    const bool blink = crtc_.cursorBlinkOn(frame);
    const uint16_t mask = displayMask();
    const std::size_t charBase = (ctrl2_ & 0x04) ? 0 : 0x800;

    uint16_t lineAddr = crtc_.startAddress();
    for (int y0 = 0; y0 < h; y0 += rasters) {
        for (int ra = 0; ra < rasters && y0 + ra < h; ++ra) {
            int cursorX = -1;
            if (!crtc_.cursorVisible(ra, lineAddr, blink, cursorX)) cursorX = -1;
            uint32_t* p = dst + std::size_t(y0 + ra) * std::size_t(w);
            for (int i = 0; i < cols; ++i) {
                const uint8_t chr = vram_[(lineAddr + i) & mask];
                uint8_t data = chars_[charBase + std::size_t(chr & 0x7F) * 16 + std::size_t(ra & 0x0F)];
                int fg = 1, bg = 0;
                if (!(ctrl2_ & 0x80)) {
                    const bool high = (chr & 0x80) != 0;
                    const uint8_t hl  = high ? 0x10 : 0x01;
                    const uint8_t inv = high ? 0x20 : 0x02;
                    fg = (ctrl2_ & hl) ? 3 : 1;
                    if (ctrl2_ & inv) std::swap(fg, bg);
                }
                if (i == cursorX) data = 0xFF;
                for (int j = 0; j < kCellWidth; ++j) {
                    *p++ = level[(data & 0x80) ? fg : bg];
                    data = uint8_t((data << 1) | (data & 1));
                }
            }
        }
        lineAddr = uint16_t((lineAddr + crtc_.horizDisplayed()) & 0x3FFF);
    }
}

void VidexUltratermCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(kVersion);
    out.push_back(ctrl1_);
    out.push_back(ctrl2_);
    out.push_back(uint8_t(block512_ / 512));
    crtc_.saveState(out);
    out.insert(out.end(), vram_.begin(), vram_.end());
}

void VidexUltratermCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (!data || len < kBlobBytes) return;
    if (!std::equal(kMagic, kMagic + 4, data) || data[4] != kVersion || data[7] > 3) return;
    pom2::Hd6845Crtc crtc;
    if (!crtc.loadState(data + kHeader, pom2::Hd6845Crtc::kStateBytes)) return;
    crtc_ = crtc;
    ctrl1_ = data[5];
    ctrl2_ = data[6];
    block512_ = data[7] * 512;
    const uint8_t* v = data + kHeader + pom2::Hd6845Crtc::kStateBytes;
    std::copy(v, v + kVramBytes, vram_.begin());
}
