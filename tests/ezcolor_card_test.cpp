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

// ezcolor_card — the E-Z Color Graphics Interface (EzColorCard.h) in slot 3
// of a ][+, driven through the real bus (Memory::memRead / memWrite at
// $C0B0 / $C0B1, MAME ezcgi.cpp's decode):
//
//  1. registers and VRAM through the two ports, the read-ahead, $FF elsewhere
//  2. a Graphics I screen: one pattern, one colour, the R7 border — rendered
//     by the VDP's raster into the card's 288 × 216 picture
//  3. the frame interrupt: slot IRQ with R1 IE, dropped by the status read
//  4. bus reset, snapshot round trip, never the Apple's screen by itself

#include "EzColorCard.h"
#include "Memory.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; } } while (0)

constexpr uint16_t kData = 0xC0B0, kCtrl = 0xC0B1;   // slot 3

void reg(Memory& m, int r, uint8_t v) { m.memWrite(kCtrl, v); m.memWrite(kCtrl, uint8_t(0x80 | r)); }
void writeAddr(Memory& m, uint16_t a) { m.memWrite(kCtrl, uint8_t(a)); m.memWrite(kCtrl, uint8_t(0x40 | (a >> 8))); }
void readAddr(Memory& m, uint16_t a)  { m.memWrite(kCtrl, uint8_t(a)); m.memWrite(kCtrl, uint8_t(a >> 8)); }

} // namespace

int main()
{
    Memory mem;
    auto owned = std::make_unique<EzColorCard>();
    EzColorCard* card = owned.get();
    mem.slotBus().plug(3, std::move(owned));
    auto frames = [&](int n) {
        for (int i = 0; i < n * TMS9918::kCyclesPerFrame / 64 + 1; ++i) card->advanceCycles(64);
    };

    // 1. Ports.
    CHECK(mem.memRead(0xC0B2) == 0xFF && mem.memRead(0xC0BF) == 0xFF);
    reg(mem, 1, 0x80);                                  // 16 KB, display off
    writeAddr(mem, 0x1234);
    for (uint8_t b : { 0x11, 0x22, 0x33 }) mem.memWrite(kData, b);
    CHECK(card->vdp().vramData()[0x1234] == 0x11 && card->vdp().vramData()[0x1236] == 0x33);
    readAddr(mem, 0x1234);
    CHECK(mem.memRead(kData) == 0x11 && mem.memRead(kData) == 0x22 && "read-ahead buffer");

    // 2. Graphics I: name table $0000, colours $2000, patterns $0800.
    reg(mem, 0, 0x00);
    reg(mem, 2, 0x00);
    reg(mem, 3, 0x80);                                  // colour table $2000
    reg(mem, 4, 0x01);                                  // patterns $0800
    reg(mem, 7, 0x04);                                  // border: dark blue
    writeAddr(mem, 0x0000);
    for (int i = 0; i < 768; ++i) mem.memWrite(kData, 0);   // every cell = pattern 0
    writeAddr(mem, 0x0800);
    for (int i = 0; i < 8; ++i) mem.memWrite(kData, 0xF0);  // left half lit
    writeAddr(mem, 0x2000);
    mem.memWrite(kData, 0x6A);                          // fg dark red, bg dark yellow ($A)
    reg(mem, 1, 0xC0);                                  // display on, no IRQ
    frames(2);
    std::vector<uint32_t> pic(std::size_t(card->pictureWidth()) * card->pictureHeight());
    card->paintPicture(pic.data(), 0, 0, 0, 0);
    auto px = [&](int x, int y) { return pic[std::size_t(y) * TMS9918::kFullWidth + x]; };
    const int ox = TMS9918::kBorderLeft, oy = TMS9918::kBorderTop;
    CHECK(card->pictureWidth() == 288 && card->pictureHeight() == 216);
    CHECK(px(ox + 0, oy + 0) == (TMS9918::kPalette[6] | 0xFF000000u) && "lit dot: fg");
    CHECK(px(ox + 4, oy + 0) == (TMS9918::kPalette[10] | 0xFF000000u) && "unlit dot: bg");
    CHECK(px(ox + 8, oy + 100) == (TMS9918::kPalette[6] | 0xFF000000u) && "every cell");
    CHECK(px(2, 2) == (TMS9918::kPalette[4] | 0xFF000000u) && "R7 border");

    // 3. Frame interrupt.
    CHECK(!card->slotIrqAsserted());
    reg(mem, 1, 0xE0);                                  // + IE
    frames(1);
    CHECK(card->slotIrqAsserted() && "frame flag + IE = slot IRQ");
    const uint8_t st = mem.memRead(kCtrl);
    CHECK((st & 0x80) && "F set in the status");
    CHECK(!card->slotIrqAsserted() && "status read drops /INT");

    // 4. Snapshot, reset, the monitor.
    std::vector<uint8_t> blob;
    card->appendSnapshotState(blob);
    EzColorCard copy;
    copy.loadSnapshotState(blob.data(), blob.size());
    std::vector<uint8_t> again;
    copy.appendSnapshotState(again);
    CHECK(blob == again);
    std::vector<uint32_t> pic2(pic.size());
    copy.paintPicture(pic2.data(), 0, 0, 0, 0);
    CHECK(pic2[std::size_t(oy) * TMS9918::kFullWidth + ox] == pic[std::size_t(oy) * TMS9918::kFullWidth + ox]
          && "restored picture rebuilt from VRAM");
    CHECK(!card->ownsScreen(true, true) && !card->ownsScreen(false, false));
    mem.slotBus().reset();
    CHECK(card->vdp().regsData()[1] == 0 && "bus reset resets the VDP");

    if (g_fail) {
        std::fprintf(stderr, "ezcolor_card: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ezcolor_card: PASS\n");
    return 0;
}
