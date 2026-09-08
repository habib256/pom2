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

// Chat Mauve mode-latch beam split (docs/chatmauve_plan.md P6, first rung).
//
// A mid-frame `$C05E/$C05F` pair reclocks the card's 2-bit latch; before
// this, the renderer sampled the latch ONCE per frame (end state), so a
// French-Touch-style frame that runs COL140 on top and BW560 below rendered
// the whole frame in the end mode. Now the beam-raced replay walks the
// latch from the same event log the DisplayState replay uses (a Dhgr
// ON→OFF event = the AN3 rising edge, clocking the current 80COL level),
// seeded by the card's timestamped ring (`latchBefore`). What this pins:
//
//   1. One frame, latch COL140 at the top and clocked to BW560 at row 96
//      (two $C05F/$C05E pairs with 80COL data low, ending back in DHGR):
//      the top band matches a full-frame COL140 render, the bottom band a
//      full-frame BW560 render — same RAM, same frame.
//   2. The reverse clocking (BW560 → COL140 mid-frame).
//   3. A frame with events but no latch clock still renders uniformly
//      (regression guard for the segment merge).
//   4. TEXT40 ⇄ Chat Mauve HGR beam split (the DIX raster shape): before
//      2026-09-02 the text band painted into the 280-wide `frame` while
//      the HGR band painted into `frame80` — two buffers, last segment
//      wins, half the picture lost. Every band under the card now lands
//      in frame80 (the legacy band is pixel-doubled during a replay).

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"
#include "M6502.h"
#include "CpuClock.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr uint16_t CLR_TEXT     = 0xC050;
constexpr uint16_t SET_HIRES    = 0xC057;
constexpr uint16_t IIE_80COL_ON = 0xC00D;
constexpr uint16_t IIE_80COL_OFF= 0xC00C;
constexpr uint16_t DHIRES_ON    = 0xC05E;
constexpr uint16_t DHIRES_OFF   = 0xC05F;

constexpr int W = 560;
constexpr int kSplitRow = 96;

struct Rig {
    Memory mem;
    Apple2Display disp;
    LeChatMauveCard* card = nullptr;   // owned by the bus

    Rig()
    {
        mem.setIIEMode(true);
        auto c = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Feline);
        card = c.get();
        card->setMemory(&mem);         // timestamps the latch ring
        mem.slotBus().plug(7, std::move(c));
        disp.setAuxMemory(mem.auxDataMutable());
        disp.setChatMauveCard(card);
        disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

        uint8_t* aux = mem.auxDataMutable();
        for (uint32_t a = 0x2000; a < 0x4000; ++a) {
            mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(a & 0x7F));
            aux[a] = static_cast<uint8_t>((a * 5) & 0x7F);
        }
        mem.memRead(CLR_TEXT);
        mem.memRead(SET_HIRES);
        mem.memWrite(IIE_80COL_ON, 0);
        mem.memRead(DHIRES_ON);        // DHGR; latch untouched so far
    }

    // Clock the latch twice with `bit` as data, ending back in DHGR, at the
    // HBL of `row` (col 0). Each $C05F is an AN3 rising edge.
    void clockLatchAt(int row, bool bit)
    {
        // One cycle per access, as the real bus would (the ring and the
        // event log are both timestamped off the counter).
        uint64_t c = static_cast<uint64_t>(row) * 65 + 2;
        mem.setCycleCounter(c++);
        mem.memWrite(bit ? IIE_80COL_ON : IIE_80COL_OFF, 0);
        for (int k = 0; k < 2; ++k) {
            mem.setCycleCounter(c++);
            mem.memRead(DHIRES_OFF);   // AN3 rising → clock
            mem.setCycleCounter(c++);
            mem.memRead(DHIRES_ON);    // back to DHGR
        }
        // 80COL must be ON for the DHGR display itself.
        mem.setCycleCounter(c++);
        mem.memWrite(IIE_80COL_ON, 0);
    }

    std::vector<uint32_t> frame()
    {
        disp.render(mem);
        assert(disp.width() == W);
        const uint32_t* p = disp.pixels();
        return std::vector<uint32_t>(p, p + static_cast<size_t>(W) * disp.height());
    }
};

bool rowEqual(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int y)
{
    return std::memcmp(a.data() + static_cast<size_t>(y) * W,
                       b.data() + static_cast<size_t>(y) * W,
                       W * sizeof(uint32_t)) == 0;
}

} // namespace

int main()
{
    // References: uniform COL140 and uniform BW560 frames of the same RAM.
    std::vector<uint32_t> col140, bw560;
    {
        Rig r;                                        // latch reset = COL140
        col140 = r.frame();
    }
    {
        Rig r;
        r.card->overrideMode(LeChatMauveCard::RenderMode::BW560);
        bw560 = r.frame();
    }
    for (int y : { 8, 100, 180 })
        assert(!rowEqual(col140, bw560, y) && "modes must differ visibly");

    // 1. COL140 on top, clocked to BW560 at row 96 — one beam-raced frame.
    {
        Rig r;
        r.mem.setCycleCounter(0);
        r.mem.beginVideoEventFrame();
        r.clockLatchAt(kSplitRow, false);             // 00 → BW560
        auto split = r.frame();
        for (int y : { 0, 40, kSplitRow - 1 })
            assert(rowEqual(split, col140, y) && "top band must be COL140");
        for (int y : { kSplitRow, 130, 191 })
            assert(rowEqual(split, bw560, y) && "bottom band must be BW560");
    }

    // 2. The reverse: BW560 on top, back to COL140 mid-frame.
    {
        Rig r;
        // Park the latch at BW560 BEFORE the frame starts, with real edges
        // so the ring carries the history.
        uint64_t c = 1;
        r.mem.setCycleCounter(c++);
        r.mem.memWrite(IIE_80COL_OFF, 0);
        for (int k = 0; k < 2; ++k) {
            r.mem.setCycleCounter(c++); r.mem.memRead(DHIRES_OFF);
            r.mem.setCycleCounter(c++); r.mem.memRead(DHIRES_ON);
        }
        r.mem.setCycleCounter(c++);
        r.mem.memWrite(IIE_80COL_ON, 0);
        assert(r.card->currentMode() == LeChatMauveCard::RenderMode::BW560);
        r.mem.setCycleCounter(1000);
        r.mem.beginVideoEventFrame();
        r.clockLatchAt(kSplitRow, true);              // 11 → COL140
        // Frame cycles are absolute: re-stamp the clocking inside THIS frame.
        auto split = r.frame();
        for (int y : { 0, 40, kSplitRow - 1 })
            assert(rowEqual(split, bw560, y) && "top band must be BW560");
        for (int y : { kSplitRow + 1, 130, 191 })
            assert(rowEqual(split, col140, y) && "bottom band must be COL140");
    }

    // 3. Events without a latch clock: uniform COL140 (the pre-P6 picture).
    {
        Rig r;
        r.mem.setCycleCounter(0);
        r.mem.beginVideoEventFrame();
        r.mem.setCycleCounter(static_cast<uint64_t>(kSplitRow) * 65 + 2);
        r.mem.memWrite(IIE_80COL_OFF, 0);             // data line moves…
        r.mem.memWrite(IIE_80COL_ON, 0);              // …but AN3 never rises
        auto plain = r.frame();
        for (int y : { 0, kSplitRow, 191 })
            assert(rowEqual(plain, col140, y) && "no clock → no split");
    }

    // 5. The latch ring is stamped with the SAME clock as the video events.
    //    With a CPU driving the stores, an event is stamped at its data cycle
    //    (instruction start + elapsed) while the card used to stamp its ring
    //    at the instruction start. A frame whose FIRST event is the $C05F
    //    edge then seeded the replay with the POST-clock latch and clocked
    //    it a second time: the top band came out in the next mode. Park the
    //    latch at BW560 before the frame (as case 2 does), keep 80COL on so
    //    the data line is 1, and clock twice from a $C05F that is the first
    //    event of its frame: top band BW560, bottom band COL140.
    {
        Rig r;
        M6502 cpu(&r.mem);
        r.mem.setCpu(&cpu);
        uint64_t c = 1;
        r.mem.setCycleCounter(c++);
        r.mem.memWrite(IIE_80COL_OFF, 0);
        for (int k = 0; k < 2; ++k) {
            r.mem.setCycleCounter(c++); r.mem.memRead(DHIRES_OFF);
            r.mem.setCycleCounter(c++); r.mem.memRead(DHIRES_ON);
        }
        r.mem.setCycleCounter(c++);
        r.mem.memWrite(IIE_80COL_ON, 0);
        assert(r.card->currentMode() == LeChatMauveCard::RenderMode::BW560);
        // $0300: STA $C05E / STA $C05F / STA $C05E / STA $C05F / STA $C05E
        const uint8_t code[] = { 0x8D, 0x5E, 0xC0, 0x8D, 0x5F, 0xC0, 0x8D, 0x5E, 0xC0,
                                 0x8D, 0x5F, 0xC0, 0x8D, 0x5E, 0xC0 };
        for (size_t i = 0; i < sizeof code; ++i)
            r.mem.memWrite(static_cast<uint16_t>(0x0300 + i), code[i]);
        cpu.setProgramCounter(0x0300);
        // Event scanlines are the absolute cycle modulo the frame length, so
        // put the frame boundary on a frame multiple: the $C05E lands in the
        // previous frame's last cycles, the $C05F at scanline kSplitRow of
        // this one — and is this frame's FIRST event.
        const uint64_t frameCycles = 65ull *
            static_cast<uint64_t>(pom2VideoTiming(r.mem.videoStandard()).scanlinesPerFrame);
        const uint64_t base = 2 * frameCycles;
        r.mem.setCycleCounter(base - 4);
        (void)cpu.run(1);                            // STA $C05E — before the frame
        r.mem.setCycleCounter(base);
        r.mem.beginVideoEventFrame();
        r.mem.setCycleCounter(base + static_cast<uint64_t>(kSplitRow) * 65);
        for (int i = 0; i < 4; ++i) (void)cpu.run(1);
        assert(r.card->currentMode() == LeChatMauveCard::RenderMode::COL140);
        auto split = r.frame();
        for (int y : { 0, 40, kSplitRow - 1 })
            assert(rowEqual(split, bw560, y) &&
                   "top band must be the PRE-clock latch (BW560), not double-clocked");
        for (int y : { kSplitRow + 1, 130, 191 })
            assert(rowEqual(split, col140, y) && "bottom band must be COL140");
    }

    // 4. TEXT40 ⇄ Chat Mauve HGR split — the DIX raster shape.
    {
        auto mkRig = []() {
            auto r = std::make_unique<Rig>();
            r->mem.memWrite(IIE_80COL_OFF, 0);   // 40 columns, as DIX runs
            r->mem.memRead(DHIRES_OFF);          // AN3 on: single Féline HGR
            for (uint16_t a = 0x0400; a < 0x0800; ++a)
                r->mem.memWrite(a, static_cast<uint8_t>(0xA0 + (a & 0x3F)));
            return r;
        };
        std::vector<uint32_t> hgr560;
        { auto r = mkRig(); hgr560 = r->frame(); }
        std::vector<uint32_t> text560(static_cast<size_t>(W) * 192);
        {
            auto r = mkRig();
            r->mem.memRead(0xC051);              // SET TEXT, whole frame
            r->disp.render(r->mem);
            assert(r->disp.width() == 280 && "static 40-col text stays native 280");
            const uint32_t* p = r->disp.pixels();
            for (int y = 0; y < 192; ++y)
                for (int x = 0; x < 280; ++x) {
                    const uint32_t px = p[static_cast<size_t>(y) * 280 + x];
                    text560[static_cast<size_t>(y) * W + 2 * x]     = px;
                    text560[static_cast<size_t>(y) * W + 2 * x + 1] = px;
                }
        }
        auto r = mkRig();
        r->mem.setCycleCounter(0);
        r->mem.beginVideoEventFrame();
        r->mem.setCycleCounter(static_cast<uint64_t>(kSplitRow) * 65 + 2);
        r->mem.memRead(0xC051);                  // TEXT from row 96 down
        auto split = r->frame();                 // asserts width == 560
        for (int y : { 0, 40, kSplitRow - 1 })
            assert(rowEqual(split, hgr560, y) && "top band must be Chat Mauve HGR");
        for (int y : { kSplitRow, 130, 191 })
            assert(rowEqual(split, text560, y) &&
                   "bottom band must be the 40-col text, in the SAME buffer");
    }

    std::printf("chatmauve latch split: OK (COL140/BW560 split both ways, no-clock guard, TEXT/HGR buffer split)\n");
    return 0;
}
