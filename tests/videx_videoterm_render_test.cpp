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

// videx_videoterm_render — the Videoterm's picture, as MAME's
// crtc_update_row (a2videoterm.cpp:473-500) paints it, and the soft video
// switch that puts it on POM2's monitor. Synthetic font, no dump needed.
//
//   * geometry: 720 × 216 published by Apple2Display while TEXT + AN0
//   * a cell is 9 dots: bits 7..0 of the glyph row, then bit 0 AGAIN
//   * VRAM bit 7 selects the alternate set; with no alternate dump that set
//     is ~normal
//   * R12/R13 scroll the picture, and the VRAM address wraps at 2 KB
//   * R1 × R6 × (R9+1) bound the lit area; the rest is black
//   * the cursor inverts its cell on rasters R10..R11, and blinks as a pure
//     function of emulated time (R10 bits 6-5)
//   * TEXT off or AN0 off -> the Apple's picture (280 × 192), and an AN0
//     flip repaints even though the static-text frame skip was primed
//   * a green monitor tints the card's picture too

#include "Apple2Display.h"
#include "Memory.h"
#include "SlotBus.h"
#include "VidexVideotermCard.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr int W = VidexVideotermCard::kPictureWidth;

// Glyph 'A' ($41): raster 0 = $81 (dots 0, 7 and the replicated 8th),
// raster 1 = $80 (dot 0 only — bit 0 clear, so no ninth dot), raster 8 = $01
// (dots 7 and 8). Glyph ' ' is blank. Every other glyph: raster 2 = $FF.
std::vector<uint8_t> font()
{
    std::vector<uint8_t> f(VidexVideotermCard::kCharSetBytes, 0);
    for (int g = 0; g < 128; ++g) if (g != 0x20 && g != 0x41) f[g * 16 + 2] = 0xFF;
    f[0x41 * 16 + 0] = 0x81;
    f[0x41 * 16 + 1] = 0x80;
    f[0x41 * 16 + 8] = 0x01;
    return f;
}

// The 60 Hz firmware's init table (6.ic6.bin offset $A1).
void programCrtc(Memory& mem)
{
    static const uint8_t kInit[16] = { 0x7B, 0x50, 0x5E, 0x2F, 0x1B, 0x08, 0x18, 0x19,
                                       0x00, 0x08, 0xE0, 0x08, 0x00, 0x00, 0x00, 0x00 };
    for (int r = 0; r < 16; ++r) {
        mem.memWrite(0xC0B0, static_cast<uint8_t>(r));
        mem.memWrite(0xC0B1, kInit[r]);
    }
}
void crtcWrite(Memory& mem, uint8_t reg, uint8_t v)
{
    mem.memWrite(0xC0B0, reg);
    mem.memWrite(0xC0B1, v);
}

uint32_t px(const std::vector<uint32_t>& p, int x, int y) { return p[static_cast<std::size_t>(y) * W + x]; }

std::vector<uint32_t> paint(const VidexVideotermCard& c, uint64_t cyc = 0)
{
    std::vector<uint32_t> p(static_cast<std::size_t>(W) * VidexVideotermCard::kPictureHeight);
    c.paintPicture(p.data(), kWhite, kBlack, cyc, 1022727.0);
    return p;
}

// Write `v` at linear VRAM address `a` through the bus, as the firmware does.
void vramPoke(Memory& mem, int a, uint8_t v)
{
    (void)mem.memRead(static_cast<uint16_t>(0xC0B0 + ((a >> 9) & 3) * 4));
    (void)mem.memRead(0xC300);    // claim $C800 for slot 3
    mem.memWrite(static_cast<uint16_t>(0xCC00 + (a & 0x1FF)), v);
}

} // namespace

int main()
{
    Memory mem;
    auto owned = std::make_unique<VidexVideotermCard>(3);
    expect(owned->loadFirmware(std::vector<uint8_t>(2048, 0xEA)), "firmware");
    auto alt = font();
    for (auto& b : alt) b = 0;
    alt[0x41 * 16 + 0] = 0x40;    // alternate 'A': dot 1 only
    expect(owned->loadCharRoms(font(), alt), "fonts");
    VidexVideotermCard* card = owned.get();
    mem.slotBus().plug(3, std::move(owned));
    programCrtc(mem);
    for (int a = 0; a < 2048; ++a) vramPoke(mem, a, 0x20);   // spaces
    crtcWrite(mem, 10, 0x20);     // cursor off for the glyph checks

    // ── Glyph dots, the 9th dot, the alternate set ──────────────────────
    vramPoke(mem, 0, 0x41);       // row 0 col 0: 'A'
    vramPoke(mem, 1, 0xC1);       // row 0 col 1: alternate 'A'
    vramPoke(mem, 80, 0x42);      // row 1 col 0: a "bar" glyph
    {
        const auto p = paint(*card);
        expect(px(p, 0, 0) == kWhite, "'A' raster 0 dot 0 lit");
        expect(px(p, 1, 0) == kBlack, "'A' raster 0 dot 1 dark");
        expect(px(p, 7, 0) == kWhite, "'A' raster 0 dot 7 (bit 0) lit");
        expect(px(p, 8, 0) == kWhite, "'A' raster 0 dot 8 = bit 0 replicated");
        expect(px(p, 0, 1) == kWhite && px(p, 8, 1) == kBlack,
               "'A' raster 1: bit 0 clear -> no ninth dot");
        expect(px(p, 7, 8) == kWhite && px(p, 8, 8) == kWhite,
               "'A' raster 8 (the ninth raster, R9=8) is painted");
        expect(px(p, 9, 0) == kBlack && px(p, 10, 0) == kWhite && px(p, 11, 0) == kBlack,
               "VRAM bit 7 -> the alternate set's 'A' in cell 1");
        // Row 1 starts at y = 9: 9 rasters per row.
        bool bar = true;
        for (int x = 0; x < 9; ++x) bar = bar && px(p, x, 9 + 2) == kWhite;
        expect(bar, "row 1 = VRAM[R1]: glyph $42 raster 2 fills all 9 dots");
        expect(px(p, 0, 9 + 1) == kBlack, "row 1 raster 1 dark");
    }
    // Derived alternate set = ~normal.
    {
        VidexVideotermCard inv(3);
        expect(inv.loadCharRoms(font(), {}), "normal-only font");
        std::vector<uint8_t> blob;
        card->appendSnapshotState(blob);
        inv.loadSnapshotState(blob.data(), blob.size());   // same VRAM + CRTC
        const auto p = paint(inv);
        // cell 1 = $C1 = ~'A': raster 0 is ~$81 = $7E -> dots 1..6 lit, 7 dark,
        // and bit 0 = 0 so dot 8 dark.
        expect(px(p, 9, 0) == kBlack && px(p, 10, 0) == kWhite &&
               px(p, 15, 0) == kWhite && px(p, 16, 0) == kBlack &&
               px(p, 17, 0) == kBlack,
               "derived alternate set is the inverse of the normal glyph");
    }

    // ── Bounds: R1 columns, R6 rows ─────────────────────────────────────
    {
        for (int a = 0; a < 2048; ++a) vramPoke(mem, a, 0x42);   // all bars
        crtcWrite(mem, 1, 40);
        crtcWrite(mem, 6, 2);
        const auto p = paint(*card);
        expect(px(p, 40 * 9 - 1, 2) == kWhite, "R1=40: column 39 painted");
        expect(px(p, 40 * 9, 2) == kBlack, "R1=40: column 40 black");
        expect(px(p, 0, 9 + 2) == kWhite, "R6=2: row 1 painted");
        expect(px(p, 0, 18 + 2) == kBlack, "R6=2: row 2 black");
        crtcWrite(mem, 1, 0x50);
        crtcWrite(mem, 6, 0x18);
        for (int a = 0; a < 2048; ++a) vramPoke(mem, a, 0x20);
    }

    // ── Start address scroll + 2 KB wrap ────────────────────────────────
    {
        vramPoke(mem, 0x7FF, 0x41);
        vramPoke(mem, 0x000, 0x42);
        crtcWrite(mem, 12, 0x07);
        crtcWrite(mem, 13, 0xFF);   // start = $7FF
        const auto p = paint(*card);
        expect(px(p, 0, 0) == kWhite && px(p, 1, 0) == kBlack,
               "start $7FF: cell (0,0) shows VRAM[$7FF] ('A')");
        bool bar = true;
        for (int x = 9; x < 18; ++x) bar = bar && px(p, x, 2) == kWhite;
        expect(bar, "cell (0,1) wraps to VRAM[$000]");
        crtcWrite(mem, 12, 0x00);
        crtcWrite(mem, 13, 80);     // scroll one row
        vramPoke(mem, 80, 0x41);
        const auto q = paint(*card);
        expect(px(q, 0, 0) == kWhite && px(q, 8, 0) == kWhite,
               "start 80: row 0 shows VRAM[80]");
        crtcWrite(mem, 13, 0);
        vramPoke(mem, 0x7FF, 0x20); vramPoke(mem, 0, 0x20); vramPoke(mem, 80, 0x20);
    }

    // ── Cursor + blink ──────────────────────────────────────────────────
    {
        crtcWrite(mem, 14, 0x00);
        crtcWrite(mem, 15, 81);     // row 1, column 1
        crtcWrite(mem, 11, 8);
        crtcWrite(mem, 10, 0x00 | 7);   // steady, rasters 7..8
        auto p = paint(*card);
        const int cx = 9, cy = 9;
        expect(px(p, cx, cy + 7) == kWhite && px(p, cx + 8, cy + 8) == kWhite,
               "steady cursor: rasters 7-8 of its cell inverted");
        expect(px(p, cx, cy + 6) == kBlack, "steady cursor: raster 6 untouched");
        expect(px(p, cx + 9, cy + 7) == kBlack, "steady cursor: next cell untouched");
        crtcWrite(mem, 10, 0x20 | 7);   // off
        p = paint(*card);
        expect(px(p, cx, cy + 7) == kBlack, "cursor mode 1: no cursor");
        crtcWrite(mem, 10, 0x09);       // start 9 > R9: no cursor
        p = paint(*card);
        expect(px(p, cx, cy + 8) == kBlack, "cursor start > R9: no cursor (HD6845S)");

        // Fast blink: on while bit 4 of the CRTC frame index is set.
        crtcWrite(mem, 10, 0x40 | 0);
        const double hz = card->crtc().frameRateHz(VidexVideotermCard::kCharClockHz);
        expect(hz > 59.9 && hz < 60.2, "60 Hz table: 17.43 MHz / 1116 / 260 = 60.07 Hz");
        const auto cyclesFor = [&](uint64_t frame) {
            return static_cast<uint64_t>((static_cast<double>(frame) + 0.5) * 1022727.0 / hz);
        };
        expect(card->crtcFrameAt(cyclesFor(20), 1022727.0) == 20, "frame index from emuCycles");
        p = paint(*card, cyclesFor(3));
        expect(px(p, cx, cy) == kBlack, "fast blink, frame 3: off");
        p = paint(*card, cyclesFor(17));
        expect(px(p, cx, cy) == kWhite, "fast blink, frame 17: on");
        p = paint(*card, cyclesFor(33));
        expect(px(p, cx, cy) == kBlack, "fast blink, frame 33: off again");
        crtcWrite(mem, 10, 0x60 | 0);   // slow: bit 5
        p = paint(*card, cyclesFor(17));
        expect(px(p, cx, cy) == kBlack, "slow blink, frame 17: off");
        p = paint(*card, cyclesFor(40));
        expect(px(p, cx, cy) == kWhite, "slow blink, frame 40: on");
        crtcWrite(mem, 10, 0x20);
    }

    // ── The soft video switch, through Apple2Display ────────────────────
    {
        Apple2Display disp;
        vramPoke(mem, 0, 0x41);
        mem.memWrite(0xC051, 0);   // TEXT
        mem.memWrite(0xC058, 0);   // AN0 off
        disp.render(mem);
        expect(disp.width() == 280 && disp.height() == 192, "no card on the display: 280x192");
        disp.setVidexCard(card);
        disp.render(mem);
        expect(disp.width() == 280 && !disp.showingCardPicture(),
               "TEXT but AN0 off: the Apple's picture");
        disp.render(mem);            // primes the static-text skip
        mem.memWrite(0xC059, 0);   // AN0 on
        disp.render(mem);
        expect(disp.showingCardPicture(), "TEXT + AN0: the card's picture");
        expect(disp.width() == 720 && disp.height() == 216, "published at 720x216");
        const uint32_t* p = disp.pixels();
        expect(p[0] == kWhite && p[1] == kBlack && p[8] == kWhite,
               "the published picture is the card's (cell 0 = 'A')");
        expect(!disp.signalProduced(), "no composite signal for a card frame");
        disp.render(mem);
        expect(disp.width() == 720, "stays on the card while TEXT + AN0");
        mem.memWrite(0xC058, 0);   // AN0 off: back to 40 columns
        disp.render(mem);
        expect(disp.width() == 280 && disp.height() == 192 && !disp.showingCardPicture(),
               "AN0 off repaints the Apple's picture (skip key invalidated)");
        // Compare against a forced full repaint: the skip must not have
        // served a stale buffer.
        std::vector<uint32_t> a(disp.pixels(), disp.pixels() + 280 * 192);
        disp.invalidateTextFrameCache();
        disp.render(mem);
        std::vector<uint32_t> b(disp.pixels(), disp.pixels() + 280 * 192);
        expect(a == b, "the Apple frame after AN0 off equals a forced repaint");
        mem.memWrite(0xC059, 0);
        mem.memWrite(0xC050, 0);   // graphics, AN0 still on
        disp.render(mem);
        expect(disp.width() == 280 && !disp.showingCardPicture(),
               "AN0 on but TEXT off: the Apple's graphics");
        mem.memWrite(0xC051, 0);
        disp.render(mem);
        expect(disp.width() == 720, "TEXT back on with AN0: the card again");
        // Green monitor tints the card's picture.
        disp.setHiResMode(Apple2Display::HiResMode::MonoGreen);
        disp.render(mem);
        expect(disp.pixels()[0] != kWhite && disp.pixels()[0] != kBlack &&
               (disp.pixels()[0] & 0x0000FF00u) == 0x0000FF00u,
               "a green monitor shows the 80 columns in green");
        expect(disp.pixels()[1] == kBlack, "unlit dots stay black");
        disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        // Unwiring the card returns the monitor to the Apple.
        disp.setVidexCard(nullptr);
        disp.render(mem);
        expect(disp.width() == 280 && !disp.showingCardPicture(),
               "no card pointer: the Apple's picture");
    }

    if (failures) {
        std::printf("videx_videoterm_render: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("videx_videoterm_render: ok\n");
    return 0;
}
