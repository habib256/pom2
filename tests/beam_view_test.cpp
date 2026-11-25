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

// Pins the beam view (Memory.h § Beam view, MemoryBeamView.cpp): with it on,
// the display paints the tube at THIS cycle — the frame in progress above the
// beam, the previous frame from the beam down — for soft switches and for
// video RAM alike; with it off, the last completed frame, as before.

#include "Apple2Display.h"
#include "Memory.h"
#include "ScreenProbe.h"
#include "SlowMotion.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t CLR_TEXT  = 0xC050;
constexpr uint16_t SET_HIRES = 0xC057;
constexpr uint16_t SET_PAGE1 = 0xC054;
constexpr uint16_t SET_PAGE2 = 0xC055;
constexpr int W = 280, H = 192;
constexpr uint64_t kFrame = 17030;   // NTSC: 65 x 262

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

void populate(Memory& mem)
{
    for (uint32_t a = 0x2000; a < 0x4000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(a & 0x7F));
    for (uint32_t a = 0x4000; a < 0x6000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(~(a * 3) & 0x7F));
}

void hgrPage1(Memory& mem)
{
    mem.memRead(CLR_TEXT);
    mem.memRead(SET_HIRES);
    mem.memRead(SET_PAGE1);
}

std::vector<uint32_t> frameOf(Memory& mem)
{
    Apple2Display d;
    d.setAuxMemory(mem.auxData());
    d.render(mem);
    const uint32_t* p = d.pixels();
    return std::vector<uint32_t>(p, p + static_cast<size_t>(W) * H);
}

bool sameRow(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int y)
{
    return std::memcmp(a.data() + static_cast<size_t>(y) * W,
                       b.data() + static_cast<size_t>(y) * W,
                       W * sizeof(uint32_t)) == 0;
}

void advanceTo(Memory& mem, uint64_t cycle)
{
    while (mem.getCycleCounter() < cycle) {
        const uint64_t left = cycle - mem.getCycleCounter();
        mem.advanceCycles(static_cast<int>(left < 1000 ? left : 1000));
    }
}

}  // namespace

int main()
{
    Memory p1Ref; populate(p1Ref); hgrPage1(p1Ref);
    const auto fP1 = frameOf(p1Ref);
    Memory p2Ref; populate(p2Ref); hgrPage1(p2Ref); p2Ref.memRead(SET_PAGE2);
    const auto fP2 = frameOf(p2Ref);
    Memory zeroRef; populate(zeroRef); hgrPage1(zeroRef);
    for (uint32_t a = 0x2000; a < 0x4000; ++a) zeroRef.memWrite(static_cast<uint16_t>(a), 0);
    const auto fZero = frameOf(zeroRef);
    check(fP1 != fP2 && fP1 != fZero, "the three references differ");

    // ── Soft switches. Frame 0 shows page 1 throughout. In frame 1 the
    // program flips to page 2 at line 100, and the machine is looked at with
    // the beam on line 150.
    for (const bool beamView : {false, true}) {
        Memory mem; populate(mem); hgrPage1(mem);
        mem.setCycleCounter(0);
        mem.setBeamView(beamView);
        advanceTo(mem, kFrame + 100 * 65 + 5);     // frame 1, line 100, in HBL
        mem.memRead(SET_PAGE2);
        advanceTo(mem, kFrame + 150 * 65 + 5);     // the beam reaches line 150
        check(mem.beamViewActive() == beamView, "active exactly when asked for");
        const auto f = frameOf(mem);
        if (beamView) {
            check(sameRow(f, fP1, 50),  "beam view: above the flip, page 1");
            check(sameRow(f, fP2, 120), "beam view: flip to beam, page 2 already");
            check(sameRow(f, fP1, 170), "beam view: below the beam, the previous frame's page 1");
            const auto lines = mem.displayStatePerLine();
            check(!lines[50].page2 && lines[120].page2 && !lines[170].page2,
                  "per-line state follows the same composite");
            // The page as fetched: page 1's bytes, page 2's, page 1's again.
            std::vector<uint8_t> page(0x2000);
            mem.captureFetchedHiRes(page.data());
            const auto lineIs = [&](int y, uint16_t base) {
                const uint16_t off = Memory::hiResLineOffset(y);
                for (int c = 0; c < 40; ++c)
                    if (page[off + c] != mem.memRead(static_cast<uint16_t>(base + off + c)))
                        return false;
                return true;
            };
            check(lineIs(50, 0x2000) && lineIs(120, 0x4000) && lineIs(170, 0x2000),
                  "fetched hi-res page takes each line from the page it showed");
            check(Memory::hiResLineOffset(0) == 0x0000 && Memory::hiResLineOffset(1) == 0x0400 &&
                  Memory::hiResLineOffset(8) == 0x0080 && Memory::hiResLineOffset(64) == 0x0028 &&
                  Memory::hiResLineOffset(191) == 0x1FD0, "hi-res line offsets");
        } else {
            for (int y : {50, 120, 170})
                check(sameRow(f, fP1, y), "published frame: page 1 everywhere");
        }
        // The frame in progress closes: both views agree on it again.
        advanceTo(mem, 2 * kFrame + 250 * 65);     // frame 2, in VBL
    }

    // ── Video RAM. The visible page is erased while the beam is on line 100;
    // looked at with the beam on line 150.
    for (const bool beamView : {false, true}) {
        Memory mem; populate(mem); hgrPage1(mem);
        mem.setCycleCounter(0);
        mem.setBeamView(beamView);
        if (!beamView) {                           // arm the log the usual way
            advanceTo(mem, 50 * 65);
            mem.memRead(SET_PAGE2); mem.memRead(SET_PAGE1);
        }
        // Frame 3: the plain log arms when frame 0 closes — a few cycles
        // INTO frame 1 with this harness's 1000-cycle steps — so the first
        // published frame it covers whole is frame 2.
        advanceTo(mem, 3 * kFrame + 100 * 65 + 5);
        for (uint32_t a = 0x2000; a < 0x4000; ++a)
            mem.memWrite(static_cast<uint16_t>(a), 0x00);
        advanceTo(mem, 3 * kFrame + 150 * 65 + 5);
        const auto f = frameOf(mem);
        if (beamView) {
            check(sameRow(f, fP1, 50),    "beam view: fetched before the erase");
            check(sameRow(f, fZero, 120), "beam view: fetched after the erase");
            check(sameRow(f, fP1, 170),   "beam view: not fetched yet, the previous frame");
        } else {
            for (int y : {50, 120, 170})
                check(sameRow(f, fP1, y), "published frame: nothing erased yet");
        }
        check(mem.memRead(0x2000) == 0x00 && mem.memRead(0x3FFF) == 0x00,
              "the paint put RAM back exactly");
    }

    // ── Beam in vertical blanking: the frame in progress is complete.
    {
        Memory mem; populate(mem); hgrPage1(mem);
        mem.setCycleCounter(0);
        mem.setBeamView(true);
        advanceTo(mem, kFrame + 100 * 65 + 5);
        for (uint32_t a = 0x2000; a < 0x4000; ++a)
            mem.memWrite(static_cast<uint16_t>(a), 0x00);
        advanceTo(mem, kFrame + 200 * 65);
        const auto f = frameOf(mem);
        check(sameRow(f, fP1, 50) && sameRow(f, fZero, 120) && sameRow(f, fZero, 170),
              "VBL: the whole frame in progress, nothing of the previous one");
    }

    // ── The Screen Inspector's probe (ScreenProbe.h).
    {
        using pom2::probeScreen;
        //                 text  mixed hires page2 80col dhgr  80store  y   fx
        auto p = probeScreen(false, false, true, false, false, false, false, 0, 0.0);
        check(std::strcmp(p.mode, "HIRES") == 0 && p.address == 0x2000 && p.bit == 0 && p.xMax == 279,
              "hi-res top left is $2000 bit 0");
        p = probeScreen(false, false, true, true, false, false, false, 191, 0.9999);
        check(p.address == 0x5FF7 && p.x == 279 && p.bit == 6 && p.page2,
              "hi-res page 2 bottom right is $5FF7 bit 6");
        p = probeScreen(false, false, true, true, false, false, true, 1, 0.0);
        check(p.address == 0x2400 && !p.page2, "80STORE pins the scanner to page 1");
        p = probeScreen(true, false, false, false, false, false, false, 8, 0.5);
        check(std::strcmp(p.mode, "TEXT") == 0 && p.address == 0x0480 + 20 && p.row == 1 && p.bit == -1,
              "text row 1 column 20 is $0494");
        p = probeScreen(false, true, true, false, false, false, false, 160, 0.0);
        check(std::strcmp(p.mode, "TEXT") == 0 && p.address == 0x0650, "mixed: line 160 is text row 20");
        p = probeScreen(false, true, true, false, false, false, false, 159, 0.0);
        check(std::strcmp(p.mode, "HIRES") == 0, "mixed: line 159 is still graphics");
        p = probeScreen(true, false, false, false, true, false, false, 0, 0.0);
        check(std::strcmp(p.mode, "TEXT80") == 0 && p.aux && p.address == 0x0400, "80 columns: column 0 is aux $0400");
        p = probeScreen(true, false, false, false, true, false, false, 0, 1.0 / 80 + 0.001);
        check(!p.aux && p.address == 0x0400 && p.x == 1, "80 columns: column 1 is main $0400");
        p = probeScreen(false, false, true, false, true, true, false, 0, 0.9999);
        check(std::strcmp(p.mode, "DHGR") == 0 && p.x == 559 && !p.aux && p.address == 0x2027,
              "DHGR right edge is main $2027");
        p = probeScreen(false, false, false, false, false, false, false, 191, 0.0);
        check(std::strcmp(p.mode, "LORES") == 0 && p.address == 0x07D0, "lo-res row 23 is $07D0");
    }
    // ── Slow motion budgets (SlowMotion.h).
    check(pom2::slowSpeedBudget(20280, 0.001) == 20 && pom2::slowSpeedBudget(17030, 0.5) == 8515 &&
          pom2::slowSpeedBudget(10, 0.001) == 1, "slow speed budgets, never below one cycle");

    if (g_failures) return 1;
    std::puts("beam_view: OK");
    return 0;
}
