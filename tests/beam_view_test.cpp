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
#include "AppleIoNames.h"
#include "FetchedPagesFile.h"
#include "Memory.h"
#include "ScreenProbe.h"
#include "SlowMotion.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
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
            Memory::FetchedPages pages;
            mem.captureFetchedPages(pages);
            const auto lineIs = [&](int y, uint16_t base) {
                const uint16_t off = Memory::hiResLineOffset(y);
                for (int c = 0; c < 40; ++c)
                    if (pages.hiResMain[off + c] != mem.memRead(static_cast<uint16_t>(base + off + c)))
                        return false;
                return true;
            };
            check(lineIs(50, 0x2000) && lineIs(120, 0x4000) && lineIs(170, 0x2000),
                  "fetched hi-res page takes each line from the page it showed");
            check(pages.usedHiRes && !pages.usedText && !pages.usedHiResAux,
                  "a hi-res frame uses the hi-res page only");
            check(mem.fetchedVideoByte(120, 0x4000 + Memory::hiResLineOffset(120), false) ==
                  mem.memRead(static_cast<uint16_t>(0x4000 + Memory::hiResLineOffset(120))),
                  "one fetched byte");
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

    // ── A switch INSIDE a line, and a text band: the state is per byte.
    // French Touch's demos flip TEXT / PAGE2 in the middle of scanlines and
    // mix text, lo-res and hi-res in one frame (DIX sources).
    {
        Memory mem; populate(mem); hgrPage1(mem);
        for (uint16_t a = 0x0400; a < 0x0800; ++a) mem.memWrite(a, 0xC1);   // 'A'
        for (uint16_t a = 0x0800; a < 0x0C00; ++a) mem.memWrite(a, 0xC2);   // 'B'
        mem.setCycleCounter(0);
        mem.setBeamView(true);
        advanceTo(mem, kFrame + 40 * 65 + 24 + 20);   // line 40, byte column 20
        mem.memRead(SET_PAGE2);
        advanceTo(mem, kFrame + 41 * 65 + 5);         // next line, in HBL
        mem.memRead(SET_PAGE1);
        advanceTo(mem, kFrame + 96 * 65 + 5);         // line 96: text from here
        mem.memRead(0xC051);
        advanceTo(mem, kFrame + 200 * 65);            // VBL: the frame is whole
        const Memory::DisplayFrame frame = mem.displayFrame();
        check(!frame.at(40, 19).page2 && frame.at(40, 20).page2 && !frame.at(41, 0).page2,
              "PAGE2 thrown at byte 20 governs bytes 20-39 of that line only");
        check(frame.splitInside(40) && !frame.splitInside(41) && !frame.splitInside(96),
              "only line 40 is split inside its bytes");
        check(!frame.at(95, 39).textMode && frame.at(96, 0).textMode, "text from line 96");
        check(frame.splitColumns(40)[20] && !frame.splitColumns(40)[19] && !frame.splitColumns(41)[20] &&
              frame.splitColumns(-1)[20], "the split is at byte 20, on line 40");
        Memory::FetchedPages pages;
        mem.captureFetchedPages(pages);
        const uint16_t off = Memory::hiResLineOffset(40);
        check(pages.hiResMain[off + 19] == mem.memRead(static_cast<uint16_t>(0x2000 + off + 19)) &&
              pages.hiResMain[off + 20] == mem.memRead(static_cast<uint16_t>(0x4000 + off + 20)),
              "fetched hi-res page follows the flip inside the line");
        check(pages.usedText && pages.usedHiRes && pages.textMain[Memory::textRowOffset(12)] == 0xC1,
              "the text band lands in the fetched text page");
        // ...and to the host: one file per page used, tagged BIN + address.
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "pom2_beam_view_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        std::string error;
        const auto files = pom2::saveFetchedPages(pages, dir, "SCREEN-T", error);
        check(error.empty() && files.size() == 2, "two pages written: hi-res and text");
        check(fs::exists(dir / "SCREEN-T-HGR#062000") && fs::file_size(dir / "SCREEN-T-HGR#062000") == 0x2000,
              "hi-res page: 8 KB, BIN at $2000");
        check(fs::exists(dir / "SCREEN-T-TEXT#060400") && fs::file_size(dir / "SCREEN-T-TEXT#060400") == 0x0400,
              "text page: 1 KB, BIN at $0400");
        fs::remove_all(dir, ec);
        check(Memory::textRowOffset(0) == 0 && Memory::textRowOffset(1) == 0x80 &&
              Memory::textRowOffset(8) == 0x28 && Memory::textRowOffset(23) == 0x3D0,
              "text row offsets");
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
    // ── Built-in I/O names (AppleIoNames.h): a sample against POM2's decode.
    {
        Memory mem;
        mem.memRead(0xC055);
        check(mem.getDisplayState().page2 &&
              std::strcmp(pom2::motherboardIoName(0xC055, false), "PAGE2 on (page 2)") == 0,
              "$C055 is PAGE2 on, and says so");
        mem.memRead(0xC050);
        check(!mem.getDisplayState().textMode &&
              std::strcmp(pom2::motherboardIoName(0xC050, false), "TEXT off (graphics)") == 0,
              "$C050 is TEXT off, and says so");
        mem.memRead(0xC057);
        check(mem.getDisplayState().hiRes &&
              std::strcmp(pom2::motherboardIoName(0xC057, false), "HIRES on") == 0, "$C057 is HIRES on");
        check(pom2::motherboardIoName(0xC040, false) == nullptr, "an address with no entry has no name");
        check(std::strcmp(pom2::via6522RegisterName(4), "T1C-L") == 0 &&
              std::strcmp(pom2::via6522RegisterName(0x0D), "IFR") == 0 &&
              std::strcmp(pom2::diskIISwitchName(9), "motor on") == 0, "6522 and Disk II names");
    }
    // ── Slow motion budgets (SlowMotion.h).
    check(pom2::slowSpeedBudget(20280, 0.001) == 20 && pom2::slowSpeedBudget(17030, 0.5) == 8515 &&
          pom2::slowSpeedBudget(10, 0.001) == 1, "slow speed budgets, never below one cycle");

    if (g_failures) return 1;
    std::puts("beam_view: OK");
    return 0;
}
