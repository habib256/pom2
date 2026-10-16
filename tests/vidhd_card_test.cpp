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

// vidhd_card — the VidHD's Super Hi-Res on a //e (VidHDCard.h, DISPLAY-001).
// No ROM: a //e-mode Memory with the card in slot 3, driven only through the
// guest's own bus accesses.
//
//   1. detection: $C0B0-$C0B2 read $24 $EA $4C; $C0B3 is the floating bus
//   2. STA $C029 #$C1 turns SHR on (the display publishes 640 × 400 whatever
//      TEXT says), #$00 turns it off, a bus reset clears NEWVIDEO; $C022,
//      $C034, $C035 latch; a READ of $C029 latches nothing; $C030 still
//      clicks the speaker when $C034 is written
//   3. pixels written into AUX through RAMWRT ($C005) render in 320 mode,
//      320 + colour fill, and 640 mode, at chosen coordinates, both through
//      paintPicture and through Apple2Display
//   4. the picture follows the RamWorks bank the CPU has selected
//   5. snapshot: the four registers round-trip; a foreign blob is ignored

#include "Apple2Display.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SlotCardFactory.h"
#include "VidHDCard.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

constexpr int W = VidHDCard::kPictureWidth;
constexpr int H = VidHDCard::kPictureHeight;

// 0xAABBGGRR for a 4-bit RGB triple.
constexpr uint32_t rgb(unsigned r, unsigned g, unsigned b)
{
    return 0xFF000000u | ((b * 17u) << 16) | ((g * 17u) << 8) | (r * 17u);
}

// Write a byte into AUX the way a //e program does: RAMWRT on, STA, off.
void auxPoke(Memory& mem, uint16_t addr, uint8_t v)
{
    mem.memWrite(0xC005, 0);   // RAMWRT on
    mem.memWrite(addr, v);
    mem.memWrite(0xC004, 0);   // RAMWRT off
}

void setColor(Memory& mem, int palette, int index, unsigned r, unsigned g, unsigned b)
{
    const uint16_t a = static_cast<uint16_t>(0x9E00 + palette * 32 + index * 2);
    auxPoke(mem, a,     static_cast<uint8_t>((g << 4) | b));
    auxPoke(mem, a + 1, static_cast<uint8_t>(r));
}

uint32_t px(const std::vector<uint32_t>& p, int x, int y) { return p[static_cast<std::size_t>(y) * W + x]; }

std::vector<uint32_t> paint(const VidHDCard& c)
{
    std::vector<uint32_t> p(static_cast<std::size_t>(W) * H, 0);
    c.paintPicture(p.data(), 0xFFFFFFFFu, 0xFF000000u, 0, 1.0e6);
    return p;
}

} // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);
    pom2::SlotCardFactory factory;
    auto made = factory.create({ "vidhd", 3, true, pom2::SystemProfile::AppleIIe });
    expect(static_cast<bool>(made), "the factory builds a VidHD with no ROM");
    if (!made) { std::printf("vidhd_card: %d failure(s)\n", failures); return 1; }
    auto* card = static_cast<VidHDCard*>(made.card.get());
    mem.slotBus().plug(3, std::move(made.card));

    // ── 1. detection ────────────────────────────────────────────────────
    expect(mem.memRead(0xC0B0) == 0x24, "$C0n0 reads $24");
    expect(mem.memRead(0xC0B1) == 0xEA, "$C0n1 reads $EA");
    expect(mem.memRead(0xC0B2) == 0x4C, "$C0n2 reads $4C");
    expect(mem.memRead(0xC0B3) == mem.memRead(0xC0C3),
           "$C0n3 reads what an empty slot's $C0n3 reads (the floating bus)");

    // ── 2. registers off the motherboard bus ────────────────────────────
    expect(!card->shrEnabled() && !card->ownsScreen(true, false), "power-on: SHR off");
    (void)mem.memRead(0xC029);
    expect(card->newVideo() == 0, "a READ of $C029 latches nothing");
    mem.memWrite(0xC029, 0x41);
    expect(!card->shrEnabled(), "NEWVIDEO $41 (bit 7 clear): SHR off");
    mem.memWrite(0xC029, 0xC1);
    expect(card->newVideo() == 0xC1 && card->shrEnabled(), "STA $C029 #$C1: SHR on");
    expect(card->ownsScreen(true, false) && card->ownsScreen(false, true),
           "SHR owns the screen whatever TEXT / AN0 say");
    const uint64_t clicks = mem.getSpeakerToggleCount();
    mem.memWrite(0xC022, 0xF6);
    mem.memWrite(0xC034, 0x0B);
    mem.memWrite(0xC035, 0x08);
    expect(card->screenColor() == 0xF6 && card->borderColor() == 0x0B &&
           card->shadow() == 0x08, "$C022 / $C034 / $C035 latch the written byte");
    expect(mem.getSpeakerToggleCount() == clicks + 2,
           "$C034 / $C035 writes still click the speaker");
    mem.memWrite(0xC029, 0x00);
    expect(!card->shrEnabled(), "STA $C029 #$00: SHR off");
    mem.memWrite(0xC029, 0xC1);
    mem.slotBus().reset();
    expect(card->newVideo() == 0 && !card->shrEnabled(), "bus reset clears NEWVIDEO");
    expect(card->screenColor() == 0xF6, "reset leaves the other registers");

    // ── 3. pixels in aux, through the //e's own switches ────────────────
    // Line 10: 320 mode, palette 2. Line 11: 320 + fill, palette 2.
    // Line 12: 640 mode, palette 3.
    setColor(mem, 2, 0, 0, 0, 0);
    setColor(mem, 2, 1, 0xF, 0, 0);      // red
    setColor(mem, 2, 2, 0, 0xF, 0);      // green
    setColor(mem, 2, 5, 0x1, 0x2, 0x3);
    for (int i = 0; i < 16; ++i) setColor(mem, 3, i, i, 15 - i, 0x8);
    auxPoke(mem, 0x9D00 + 10, 0x02);
    auxPoke(mem, 0x9D00 + 11, 0x22);
    auxPoke(mem, 0x9D00 + 12, 0x83);
    const uint16_t l10 = 0x2000 + 10 * 160, l11 = 0x2000 + 11 * 160, l12 = 0x2000 + 12 * 160;
    auxPoke(mem, l10 + 0, 0x12);         // red, green
    auxPoke(mem, l10 + 7, 0x50);         // colour 5, colour 0
    auxPoke(mem, l11 + 0, 0x20);         // green, fill → green
    auxPoke(mem, l11 + 1, 0x01);         // fill → green, red
    auxPoke(mem, l11 + 2, 0x00);         // fill, fill → red, red
    auxPoke(mem, l12 + 3, 0x1B);         // 00 01 10 11 → 8+0, 12+1, 0+2, 4+3
    expect(mem.peekMainRam(l10) == 0 && mem.auxData()[l10] == 0x12,
           "the pixel bytes went to aux, not main");
    mem.memWrite(0xC029, 0xC1);
    {
        const auto p = paint(*card);
        // 320: byte 0 = dots 0-1 → x 0-3; each dot two pixels wide.
        expect(px(p, 0, 20) == rgb(15, 0, 0) && px(p, 1, 20) == rgb(15, 0, 0),
               "320: high nibble first, doubled (red at x 0-1)");
        expect(px(p, 2, 20) == rgb(0, 15, 0) && px(p, 3, 20) == rgb(0, 15, 0),
               "320: low nibble second (green at x 2-3)");
        expect(px(p, 28, 20) == rgb(1, 2, 3) && px(p, 30, 20) == rgb(0, 0, 0),
               "320: byte 7 at x 28-31, colour 5 then colour 0");
        expect(px(p, 0, 21) == px(p, 0, 20) && px(p, 2, 21) == px(p, 2, 20),
               "each SHR line is painted on two output rows");
        // 320 + fill.
        expect(px(p, 0, 22) == rgb(0, 15, 0) && px(p, 2, 22) == rgb(0, 15, 0) &&
               px(p, 4, 22) == rgb(0, 15, 0), "fill: index 0 repeats green");
        expect(px(p, 6, 22) == rgb(15, 0, 0) && px(p, 8, 22) == rgb(15, 0, 0) &&
               px(p, 10, 22) == rgb(15, 0, 0), "fill: red, then index 0 repeats red");
        // 640: byte 3 → x 12-15, palette offsets 8, 12, 0, 4.
        const auto c3 = [](int i) { return rgb(static_cast<unsigned>(i), static_cast<unsigned>(15 - i), 8); };
        expect(px(p, 12, 24) == c3(8 + 0), "640: dot 0 from palette entry 8 + 0");
        expect(px(p, 13, 24) == c3(12 + 1), "640: dot 1 from palette entry 12 + 1");
        expect(px(p, 14, 24) == c3(0 + 2), "640: dot 2 from palette entry 0 + 2");
        expect(px(p, 15, 24) == c3(4 + 3), "640: dot 3 from palette entry 4 + 3");
        expect(px(p, 15, 25) == c3(7), "640: doubled vertically too");
    }

    // ── Through Apple2Display ───────────────────────────────────────────
    {
        Apple2Display disp;
        disp.setCardVideoSource(card);
        mem.memWrite(0xC029, 0x00);
        mem.memWrite(0xC051, 0);   // TEXT
        disp.render(mem);
        expect(disp.width() == 280 && !disp.showingCardPicture(),
               "SHR off: the Apple's picture");
        mem.memWrite(0xC029, 0xC1);
        disp.render(mem);
        expect(disp.showingCardPicture() && disp.width() == W && disp.height() == H,
               "SHR on: the display publishes the card's 640x400");
        expect(disp.pixels()[20 * W + 2] == rgb(0, 15, 0),
               "the published picture is the card's (green at x 2, row 20)");
        expect(!disp.signalProduced(), "no composite signal for an SHR frame");
        mem.memWrite(0xC050, 0);   // graphics: SHR keeps the screen
        disp.render(mem);
        expect(disp.showingCardPicture(), "SHR keeps the screen in graphics mode too");
        mem.memWrite(0xC029, 0x01);
        disp.render(mem);
        expect(disp.width() == 280 && !disp.showingCardPicture(),
               "NEWVIDEO cleared: back to the Apple's picture");
        disp.setCardVideoSource(nullptr);
    }

    // ── 4. RamWorks: the CPU's selected aux bank ────────────────────────
    {
        Memory rw;
        rw.setIIEMode(true);
        rw.setRamWorksBanks(4);
        auto c = std::make_unique<VidHDCard>(2);
        auto* vh = c.get();
        rw.slotBus().plug(2, std::move(c));
        auxPoke(rw, 0x9E00 + 2, 0x0F);    // bank 0: palette 0 entry 1 = blue
        auxPoke(rw, 0x9E00 + 3, 0x00);
        auxPoke(rw, 0x2000, 0x11);
        rw.memWrite(0xC073, 1);           // RamWorks bank 1
        auxPoke(rw, 0x9E00 + 2, 0x00);    // bank 1: entry 1 = red
        auxPoke(rw, 0x9E00 + 3, 0x0F);
        auxPoke(rw, 0x2000, 0x11);
        rw.memWrite(0xC029, 0xC1);
        expect(px(paint(*vh), 0, 0) == rgb(15, 0, 0), "RamWorks bank 1 selected: its SHR");
        rw.memWrite(0xC073, 0);
        expect(px(paint(*vh), 0, 0) == rgb(0, 0, 15), "RamWorks bank 0 selected: its SHR");
    }

    // ── 5. snapshot ─────────────────────────────────────────────────────
    {
        mem.memWrite(0xC029, 0xE1);
        mem.memWrite(0xC022, 0x1E);
        mem.memWrite(0xC034, 0x05);
        mem.memWrite(0xC035, 0x0C);
        std::vector<uint8_t> blob;
        card->appendSnapshotState(blob);
        VidHDCard fresh(3);
        fresh.loadSnapshotState(blob.data(), blob.size());
        expect(fresh.newVideo() == 0xE1 && fresh.screenColor() == 0x1E &&
               fresh.borderColor() == 0x05 && fresh.shadow() == 0x0C,
               "the four registers round-trip");
        std::vector<uint8_t> again;
        fresh.appendSnapshotState(again);
        expect(again == blob, "save -> load -> save reproduces the blob");
        const std::vector<uint8_t> foreign(16, 0xAB);
        fresh.loadSnapshotState(foreign.data(), foreign.size());
        expect(fresh.newVideo() == 0xE1, "a foreign blob is ignored");
        fresh.loadSnapshotState(blob.data(), blob.size() - 1);
        expect(fresh.newVideo() == 0xE1, "a short blob is ignored");
    }

    if (failures) { std::printf("vidhd_card: %d failure(s)\n", failures); return 1; }
    std::printf("vidhd_card: OK\n");
    return 0;
}
