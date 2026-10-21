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

#include "VidHDCard.h"

#include <algorithm>

namespace {
// Snapshot blob: magic, version, the four latched registers. AppleWin's
// unit (VidHD.cpp:170-180) saves the same four plus its ][+ memory mode,
// which POM2 does not model.
constexpr uint8_t kMagic[4] = { 'V', 'D', 'H', 'D' };
constexpr uint8_t kVersion  = 2;   // 2: $C022 is painted, powers up $F0
constexpr std::size_t kBlobBytes = 4 + 1 + 4;
} // namespace

uint8_t VidHDCard::slotRomRead(uint8_t low8)
{
    // AppleWin 6c579da5bd94, VidHD.cpp InitializeIO(): IORead is the
    // IOReadCx handler (Memory.h RegisterIoHandler), not IOReadC0.
    uint8_t value;
    return peekSlotRom(low8, value) ? value : openBus();
}

bool VidHDCard::peekSlotRom(uint8_t low8, uint8_t& out) const
{
    switch (low8) {
        case 0: out = 0x24; return true;
        case 1: out = 0xEA; return true;
        case 2: out = 0x4C; return true;
        default: return false;
    }
}

uint32_t VidHDCard::iigsColour(uint8_t index)
{
    // Apple II Technical Note IIgs #63, Table 1 — the master colour ($0RGB)
    // of each $C022 / $C034 colour register value.
    static constexpr uint16_t kMaster[16] = {
        0x0000, 0x0D03, 0x0009, 0x0D2D, 0x0072, 0x0555, 0x022F, 0x06AF,
        0x0850, 0x0F60, 0x0AAA, 0x0F98, 0x01D0, 0x0FF0, 0x04F9, 0x0FFF,
    };
    const uint16_t m = kMaster[index & 0x0F];
    const uint32_t r = ((m >> 8) & 0xF) * 17u;
    const uint32_t g = ((m >> 4) & 0xF) * 17u;
    const uint32_t b = ( m       & 0xF) * 17u;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

bool VidHDCard::textColours(uint32_t& fg, uint32_t& bg) const
{
    if (screenColor_ == 0xF0) return false;   // the default: plain text
    fg = iigsColour(static_cast<uint8_t>(screenColor_ >> 4));
    bg = iigsColour(static_cast<uint8_t>(screenColor_ & 0x0F));
    return true;
}

void VidHDCard::onVideoSoftSwitchWrite(uint16_t addr, uint8_t value)
{
    // AppleWin VidHD.cpp:93-99 (`VideoIOWrite`, the IIgs registers). The
    // broadcast also carries the Chat Mauve's switches; the card ignores
    // every address but these four.
    switch (addr) {
        case 0xC022: screenColor_ = value; break;
        case 0xC029: newVideo_    = value; break;
        case 0xC034: borderColor_ = value; break;
        case 0xC035: shadow_      = value; break;
        default: break;
    }
}

void VidHDCard::paintPicture(uint32_t* dst, uint32_t /*lit*/, uint32_t dark,
                             uint64_t /*emuCycles*/, double /*cpuHz*/) const
{
    // A colour picture: the monitor's phosphor colour does not apply, and
    // nothing blinks.
    const uint8_t* aux = auxRam();
    if (!aux) {
        std::fill(dst, dst + kPictureWidth * kPictureHeight, dark);
        return;
    }
    uint32_t row[kPictureWidth];
    for (int y = 0; y < kShrLines; ++y) {
        // NTSC.cpp:1872-1887 — SCB, palette, then the line's 160 bytes.
        const uint8_t scb = aux[kScbBase + y];
        const uint8_t* pal = aux + kPaletteBase + (scb & 0x0F) * 32;
        const uint8_t* src = aux + kPixelBase + y * kBytesPerLine;
        const auto color = [pal](int idx) {
            return rgb12(pal[idx * 2], pal[idx * 2 + 1]);
        };
        if (!(scb & 0x80)) {
            // 320 mode (pom2gs VGC.cpp drawShrLine): high nibble first, each
            // dot doubled. Colour fill (SCB bit 5): index 0 repeats the
            // previous dot's colour; the line starts from palette entry 0.
            const bool fill = (scb & 0x20) != 0;
            uint32_t last = color(0);
            for (int b = 0; b < kBytesPerLine; ++b) {
                const uint8_t v = src[b];
                for (int half = 0; half < 2; ++half) {
                    const int idx = half ? (v & 0x0F) : (v >> 4);
                    const uint32_t c = (fill && idx == 0) ? last : color(idx);
                    last = c;
                    row[b * 4 + half * 2]     = c;
                    row[b * 4 + half * 2 + 1] = c;
                }
            }
        } else {
            // 640 mode: four 2-bit dots, MSB pair first, each column of four
            // from its own palette quarter (VidHD.cpp:143-157; IIgs HW Ref
            // table 4-21).
            static constexpr int kOff[4] = { 8, 12, 0, 4 };
            for (int b = 0; b < kBytesPerLine; ++b) {
                const uint8_t v = src[b];
                for (int d = 0; d < 4; ++d)
                    row[b * 4 + d] = color(kOff[d] + ((v >> ((3 - d) * 2)) & 0x03));
            }
        }
        uint32_t* out = dst + static_cast<std::size_t>(y) * 2 * kPictureWidth;
        std::copy(row, row + kPictureWidth, out);
        std::copy(row, row + kPictureWidth, out + kPictureWidth);
    }
}

void VidHDCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMagic, kMagic + 4);
    out.push_back(kVersion);
    out.push_back(screenColor_);
    out.push_back(newVideo_);
    out.push_back(borderColor_);
    out.push_back(shadow_);
}

void VidHDCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // Foreign, older or short blobs are refused whole (SlotPeripheral.h).
    if (!data || len < kBlobBytes) return;
    if (!std::equal(kMagic, kMagic + 4, data)) return;
    if (data[4] != kVersion && data[4] != 1) return;
    screenColor_ = data[5];
    // v1 (v0.9.7) powered $C022 up at 0 and never painted it, so a program
    // that left it alone saved 0 — black on black today. That 0 meant
    // "untouched": the IIgs power-on value is $F0.
    if (data[4] == 1 && screenColor_ == 0) screenColor_ = 0xF0;
    newVideo_    = data[6];
    borderColor_ = data[7];
    shadow_      = data[8];
}
