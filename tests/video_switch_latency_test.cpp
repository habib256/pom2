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

// Two measured timings of the $C050-$C057 display switches
// (docs/audits/parite-apple2video-2026-11-01.md, Rapport 2).
//
// 1. A mid-line TEXT ($C050/$C051) or MIXED ($C052/$C053) flip reaches the
//    screen ONE column right of a PAGE2 flip thrown at the same horizontal
//    cycle.
//    [A] tomcw, AppleWin GH#656: real //e PAL photos, `LDA $C055/$C054` on
//        line 0 and `LDA $C050/$C051` on line 7 at the same phase — the GR
//        box sits one column right of the PAGE2 box.
//    [A] arekkusu42 "VIDMODE LATENCY" photos, MAME PR #15247: TEXT/MIXED 3
//        PH0 cycles against PAGE2 2.
//
// 2. The READ that flips a mode returns the floating-bus byte of the mode
//    BEFORE the flip (the scanner fetched it in PHASE 1, the switch changes
//    in PHASE 0).
//    [A] Ryan Schmidt, real unenhanced //e, TomHarte/CLK#1204: TEXT + HIRES,
//        `LDX $C050` → a text byte, `LDY $C050` → an HGR byte.
//    [B] Sather, Understanding the Apple IIe p. 7-26.
//
// Core + display test: no ImGui, no CPU — drives Memory directly.

#include "Apple2Display.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

constexpr int W = 280;

uint64_t at(int line, int hpos) { return static_cast<uint64_t>(line) * 65 + hpos; }

// HGR page 1 black, HGR page 2 white, text page all INVERSE SPACE (lit
// cell): a column switched to page 2 or to text turns lit, so the first lit
// pixel of a line is the column the switch took effect.
void populate(Memory& mem)
{
    uint8_t* m = const_cast<uint8_t*>(mem.data());
    for (int a = 0x0400; a < 0x0C00; ++a) m[a] = 0x20;   // inverse space
    for (int a = 0x2000; a < 0x4000; ++a) m[a] = 0x00;   // HGR1 black
    for (int a = 0x4000; a < 0x6000; ++a) m[a] = 0x7F;   // HGR2 white
}

int firstLit(const uint32_t* px, int y)
{
    for (int x = 0; x < W; ++x)
        if ((px[y * W + x] & 0x00FFFFFFu) != 0) return x;
    return -1;
}

void testMidLineColumn()
{
    Memory mem;
    mem.setIIEMode(true);
    populate(mem);
    mem.memRead(0xC050);   // graphics
    mem.memRead(0xC052);   // full screen
    mem.memRead(0xC057);   // HIRES
    mem.memRead(0xC054);   // page 1
    mem.setCycleCounter(0);
    mem.beginVideoEventFrame();

    // PAGE2 at hpos 40 on line 10, TEXT at hpos 40 on line 50, MIXED is
    // checked the same way below 160 lines (its text band); each switch is
    // undone in the next line's HBL. PAGE2 goes both ways in the frame, so
    // the DROL one-way shortcut stays out of it.
    mem.setCycleCounter(at(10, 40)); mem.memRead(0xC055);
    mem.setCycleCounter(at(11, 5));  mem.memRead(0xC054);
    mem.setCycleCounter(at(50, 40)); mem.memRead(0xC051);
    mem.setCycleCounter(at(51, 5));  mem.memRead(0xC050);
    // The last HBL cycle: PAGE2 governs the whole line, TEXT one column in.
    mem.setCycleCounter(at(90, 24));  mem.memRead(0xC055);
    mem.setCycleCounter(at(91, 5));   mem.memRead(0xC054);
    mem.setCycleCounter(at(100, 24)); mem.memRead(0xC051);
    mem.setCycleCounter(at(101, 5));  mem.memRead(0xC050);
    // MIXED: lines 160+ become text from the switch on.
    mem.setCycleCounter(at(170, 40)); mem.memRead(0xC053);
    mem.setCycleCounter(at(171, 5));  mem.memRead(0xC052);

    Apple2Display d;
    d.setAuxMemory(mem.auxDataMutable());
    d.render(mem);
    if (d.width() != W) { CHECK(false, "expected the 280-wide path"); return; }
    const uint32_t* px = d.pixels();

    const int page  = firstLit(px, 10);
    const int text  = firstLit(px, 50);
    const int page0 = firstLit(px, 90);
    const int text0 = firstLit(px, 100);
    const int mixed = firstLit(px, 170);
    std::printf("first lit x: PAGE2@40=%d TEXT@40=%d PAGE2@24=%d TEXT@24=%d MIXED@40=%d\n",
                page, text, page0, text0, mixed);

    // PAGE2 anchor (MAD EFFECT-calibrated hpos - 24): column 16 → x 112.
    CHECK(page == 16 * 7, "PAGE2 at hpos 40 shows from column 16");
    // TEXT / MIXED: one column later than PAGE2 on the same cycle.
    CHECK(text == page + 7, "TEXT at hpos 40 shows one column right of PAGE2 (column 17)");
    CHECK(mixed == page + 7, "MIXED at hpos 40 shows one column right of PAGE2 (column 17)");
    CHECK(page0 == 0, "PAGE2 at hpos 24 governs the whole line");
    CHECK(text0 == 7, "TEXT at hpos 24 shows from column 1");
}

void testFloatingBusBeforeFlip(bool iie)
{
    Memory mem;
    mem.setIIEMode(iie);
    uint8_t* m = const_cast<uint8_t*>(mem.data());
    for (int a = 0x0400; a < 0x0C00; ++a) m[a] = 0xA0;   // text pages
    for (int a = 0x2000; a < 0x4000; ++a) m[a] = 0x7F;   // HGR page 1
    for (int a = 0x4000; a < 0x6000; ++a) m[a] = 0x55;   // HGR page 2

    // CLK#1204 instant.s: TEXT + HIRES, MIXED off, then LDX/LDY $C050.
    mem.memRead(0xC051);
    mem.memRead(0xC052);
    mem.memRead(0xC057);
    mem.memRead(0xC054);
    mem.setCycleCounter(at(10, 40));        // inside the visible window
    const uint8_t x = mem.memRead(0xC050);
    mem.setCycleCounter(at(10, 44));
    const uint8_t y = mem.memRead(0xC050);
    std::printf("%s LDX $C050=%02X LDY $C050=%02X\n", iie ? "//e" : "][+", x, y);
    CHECK(x == 0xA0, "the access that leaves TEXT returns a text byte");
    CHECK(y == 0x7F, "the next access returns an HGR byte");

    // Same rule for a page flip: graphics HIRES page 1, LDA $C055 twice.
    mem.setCycleCounter(at(20, 40));
    const uint8_t p1 = mem.memRead(0xC055);
    mem.setCycleCounter(at(20, 44));
    const uint8_t p2 = mem.memRead(0xC055);
    std::printf("%s LDA $C055=%02X LDA $C055=%02X\n", iie ? "//e" : "][+", p1, p2);
    CHECK(p1 == 0x7F, "the access that selects PAGE2 returns a page-1 byte");
    CHECK(p2 == 0x55, "the next access returns a page-2 byte");
}

}  // namespace

int main()
{
    testMidLineColumn();
    testFloatingBusBeforeFlip(true);
    testFloatingBusBeforeFlip(false);
    if (failures == 0) std::printf("OK video_switch_latency\n");
    return failures == 0 ? 0 : 1;
}
