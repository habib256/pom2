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
// $C0B0 / $C0B1 and their mirrors — BYTE Fig. 4 decodes A0 only):
//
//  1. registers and VRAM through the two ports, the read-ahead, every
//     even / odd $C0BX a mirror of them (MAME reads $FF there; the schematic
//     says otherwise)
//  2. a Graphics I screen: one pattern, one colour, the R7 border — rendered
//     by the VDP's raster into the card's 288 × 216 picture
//  3. the frame interrupt: /NMI (JP2's default) or slot IRQ with R1 IE,
//     dropped by the status read
//  4. bus reset, snapshot round trip, never the Apple's screen by itself
//  5. the access window (silicon-strict, the card's default): a burst with
//     no time between the bytes loses them and counts them; spaced, it lands;
//     the gap each mode needs
//  6. an NMOS indexed store's phantom read, through the real CPU
//  7. jumper JP2: /INT on IRQ, NMI (edge, wire-OR, snapshot) or nothing
//
// 1-4 drive the ports with no CPU time between accesses, so they switch the
// window off; 5 puts it back.

#include "EzColorCard.h"
#include "CardVideoSource.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdio>
#include <initializer_list>
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
    CHECK(card->vdp().isSiliconStrictMode() && "the card enforces the window");
    card->vdp().setSiliconStrictMode(false);
    auto frames = [&](int n) {
        for (int i = 0; i < n * TMS9918::kCyclesPerFrame / 64 + 1; ++i) card->advanceCycles(64);
    };

    // 1. Ports.
    reg(mem, 1, 0x80);                                  // 16 KB, display off
    writeAddr(mem, 0x1234);
    for (uint8_t b : { 0x11, 0x22, 0x33 }) mem.memWrite(kData, b);
    CHECK(card->vdp().vramData()[0x1234] == 0x11 && card->vdp().vramData()[0x1236] == 0x33);
    readAddr(mem, 0x1234);
    CHECK(mem.memRead(kData) == 0x11 && mem.memRead(kData) == 0x22 && "read-ahead buffer");
    // A0 is the only address line on the card ([B] BYTE Aug. 1982 Fig. 4):
    // $C0B3 / $C0BD are the register port, $C0BE / $C0B2 the data port.
    mem.memWrite(0xC0B3, 0x00); mem.memWrite(0xC0BD, 0x40 | 0x15);   // write address $1500
    mem.memWrite(0xC0BE, 0x5A); mem.memWrite(0xC0B2, 0xA5);
    CHECK(card->vdp().vramData()[0x1500] == 0x5A && card->vdp().vramData()[0x1501] == 0xA5
          && "every even $C0BX is the data port, every odd one the register port");
    mem.memWrite(0xC0B7, 0x00); mem.memWrite(0xC0B9, 0x15);           // read address $1500
    CHECK(mem.memRead(0xC0BC) == 0x5A && mem.memRead(0xC0B4) == 0xA5 && "mirrored reads");
    CHECK((mem.memRead(0xC0BF) & 0x1F) == (mem.memRead(kCtrl) & 0x1F) && "odd = status");

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

    // 3. Frame interrupt — on /NMI, JP2's default (§ 7 covers the edges),
    // then on /IRQ.
    CHECK(!card->slotIrqAsserted() && !card->slotNmiAsserted());
    reg(mem, 1, 0xE0);                                  // + IE
    frames(1);
    CHECK(card->slotNmiAsserted() && !card->slotIrqAsserted() && "frame flag + IE = /NMI");
    uint8_t st = mem.memRead(kCtrl);
    CHECK((st & 0x80) && "F set in the status");
    CHECK(!card->slotNmiAsserted() && "status read drops /INT");
    card->setInterruptWiring(EzColorCard::InterruptWiring::Irq);
    frames(1);
    CHECK(card->slotIrqAsserted() && !card->slotNmiAsserted() && "JP2 on IRQ: slot IRQ");
    st = mem.memRead(kCtrl);
    CHECK((st & 0x80) && !card->slotIrqAsserted() && "status read drops /INT");

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
    // …its registers, not its DRAM: MAME tms9928a.cpp device_reset leaves
    // VRAM alone, and a Ctrl-Reset used to wipe it (bug hunt 2026-10-31).
    CHECK(card->vdp().vramData()[0x1234] == 0x11 && card->vdp().vramData()[0x1236] == 0x33
          && "bus RESET keeps VRAM");
    CHECK(card->vdp().vramData()[0x0800] == 0xF0 && "…the patterns too");
    mem.slotBus().reset(/*powerCycle=*/true);
    CHECK(card->vdp().vramData()[0x1235] == 0x00 && card->vdp().vramData()[0x1234] == 0xFF
          && "power cycle: the power-on pattern");

    // 5. The window. Display on, Graphics I, the beam on an active line.
    card->vdp().setSiliconStrictMode(true);
    reg(mem, 1, 0xC0);
    card->advanceCycles(64);
    writeAddr(mem, 0x0400);
    card->advanceCycles(40);
    const uint64_t before = card->vdp().droppedWriteCount();
    for (int i = 0; i < 4; ++i) mem.memWrite(kData, uint8_t(0xA0 + i));   // no time between
    CHECK(card->vdp().droppedWriteCount() > before && "back to back: dropped and counted");
    card->advanceCycles(40);                            // the one pending access lands…
    CHECK(card->vdp().vramData()[0x0400] == 0xA3 && "…and it is the newest (openMSX)");
    writeAddr(mem, 0x0500);
    card->advanceCycles(40);
    const uint64_t spaced = card->vdp().droppedWriteCount();
    for (int i = 0; i < 4; ++i) { mem.memWrite(kData, uint8_t(0xB0 + i)); card->advanceCycles(40); }
    CHECK(card->vdp().droppedWriteCount() == spaced && "40 cycles apart: every byte lands");
    CHECK(card->vdp().vramData()[0x0503] == 0xB3);

    // The gap a stream of data writes needs during the display, three frames
    // of it in each mode (openMSX's slot tables): 8 cycles is always enough,
    // 7 is not in Graphics I / II (Multicolor needs 7, Text 3). What a
    // program keeping ≥ 18 cycles relies on.
    {
        struct M { const char* n; uint8_t r0, r1; } modes[] = {
            {"GfxI", 0x00, 0xC0}, {"GfxII", 0x02, 0xC0}, {"Text", 0x00, 0xD0}, {"Multi", 0x00, 0xC8} };
        auto drops = [&](const M& md, int gap) {
            reg(mem, 0, md.r0); card->advanceCycles(40);
            reg(mem, 1, md.r1); card->advanceCycles(40);
            writeAddr(mem, 0x0000); card->advanceCycles(40);
            card->vdp().resetDroppedWriteCount();
            const int n = 3 * TMS9918::kCyclesPerFrame / gap;
            for (int i = 0; i < n; ++i) { mem.memWrite(kData, uint8_t(i)); card->advanceCycles(gap); }
            return card->vdp().droppedWriteCount();
        };
        for (const auto& md : modes) {
            CHECK(drops(md, 8) == 0 && "8 cycles apart: never dropped");
            CHECK(drops(md, 18) == 0);
        }
        CHECK(drops(modes[0], 7) > 0 && drops(modes[1], 7) > 0 && "7 is too close in Graphics I / II");
        CHECK(drops(modes[3], 7) == 0 && drops(modes[2], 3) == 0);
    }

    // 5b. Bug hunt 2026-11-07, three VDP fixes, on fresh cards.
    auto vAddr = [](TMS9918& v, uint16_t a, bool write) {
        v.writeControl(uint8_t(a)); v.writeControl(uint8_t((write ? 0x40 : 0x00) | (a >> 8)));
    };
    auto vReg = [](TMS9918& v, int r, uint8_t x) { v.writeControl(x); v.writeControl(uint8_t(0x80 | r)); };
    {   // R1 bit 7 clear (4K DRAM mapping) still reaches all 16 KB — it used
        // to wrap at $0FFF, so $1234 landed on $0234 (the name table's page).
        EzColorCard c;
        TMS9918& v = c.vdp();
        CHECK(v.isSiliconStrictMode() && v.regsData()[1] == 0);
        const uint8_t below = v.vramData()[0x0234];
        vAddr(v, 0x1234, true);  v.writeData(0xAA); c.advanceCycles(40);
        CHECK(v.vramData()[0x1234] == 0xAA && v.vramData()[0x0234] == below && "4K mode: no 4 KB wrap");
        vAddr(v, 0x1234, false); c.advanceCycles(40);
        CHECK(v.readData() == 0xAA && "…and it reads back");
        // Flip to the 16K mapping: the byte is now at the address the DRAM
        // cell has there ([D] openMSX change4k8kMapping: VA6 <-> VA12).
        vReg(v, 1, 0x80); c.advanceCycles(40);
        CHECK(v.vramData()[0x0934] == 0xAA && "16K mapping: the same cell, rearranged");
        vReg(v, 1, 0x00); c.advanceCycles(40);
        CHECK(v.vramData()[0x1234] == 0xAA && "back to 4K: back where it was");
    }
    {   // A snapshot taken while a data-port write waits for its VRAM slot
        // keeps it: the restored VDP lands the byte (it used to be dropped).
        EzColorCard c;
        TMS9918& v = c.vdp();
        vReg(v, 1, 0xC0); c.advanceCycles(64);         // Graphics I, display on
        vAddr(v, 0x0400, true); c.advanceCycles(40);
        v.writeData(0x5A);                              // pending: >= 1 cycle away
        CHECK(v.vramData()[0x0400] != 0x5A);
        std::vector<uint8_t> b;
        c.appendSnapshotState(b);
        EzColorCard r;
        r.loadSnapshotState(b.data(), b.size());
        r.advanceCycles(40);
        CHECK(r.vdp().vramData()[0x0400] == 0x5A && "restored pending write lands");
        // A blob from before the fix (bits 1-2 of the strict byte clear)
        // still loads, with no access pending.
        b[4 + TMS9918::kStateBytes - 3] &= 0x01;
        EzColorCard o;
        o.loadSnapshotState(b.data(), b.size());
        o.advanceCycles(40);
        CHECK(o.vdp().isSiliconStrictMode() && o.vdp().vramData()[0x0400] != 0x5A);
    }
    {   // Text mode has no sprite engine: a "SAT" over text bytes raises no
        // 5S / collision. Graphics I with the same VRAM does (the control).
        EzColorCard c;
        TMS9918& v = c.vdp();
        v.setSiliconStrictMode(false);
        vReg(v, 5, 0x00); vReg(v, 6, 0x00);             // SAT and sprite patterns at $0000
        vAddr(v, 0x0000, true);
        for (int i = 0; i < 0x100; ++i) v.writeData(0x20);   // 32 sprites at Y=$20, pattern $20
        vReg(v, 1, 0xD0);                               // Text, display on
        (void)v.readControl();
        for (int i = 0; i < 2 * TMS9918::kCyclesPerFrame / 64 + 1; ++i) c.advanceCycles(64);
        CHECK((v.readControl() & 0x60) == 0 && "Text: no 5S, no collision");
        vReg(v, 1, 0xC0);                               // Graphics I, same VRAM
        for (int i = 0; i < 2 * TMS9918::kCyclesPerFrame / 64 + 1; ++i) c.advanceCycles(64);
        CHECK((v.readControl() & 0x40) && "Graphics I: the 5th sprite is flagged");
    }

    // 6. The VDP keeps its own crystal's time (bug hunt 2026-10-31). The card
    // is told the CPU clock (setCpuClock: an accelerator, a PAL machine); the
    // VDP's tables are in 1.022727 MHz cycles, so the card converts. Before,
    // a 4x accelerator fired the frame flag 4x as often.
    {
        auto firstF = [](double hz) {
            EzColorCard c;
            c.vdp().setSiliconStrictMode(false);
            c.setCpuClock(hz);
            for (long n = 1; n < 10L * TMS9918::kCyclesPerFrame; ++n) {
                c.advanceCycles(1);
                if (c.vdp().statusRegister() & 0x80) return n;
            }
            return -1L;
        };
        const long k = TMS9918::kActiveDisplayCycles;
        CHECK(firstF(1022727.0) == k && "NTSC, no accelerator: unchanged");
        const long acc = firstF(4.0 * 1022727.0);
        CHECK(acc >= 4 * k - 4 && acc <= 4 * k + 4 && "a 4x CPU: the frame takes 4x the cycles");
        const long pal = firstF(1015625.0);
        const long palWant = long(double(k) * 1015625.0 / 1022727.0 + 0.999);
        CHECK(pal >= palWant - 1 && pal <= palWant + 1 && "PAL host: the VDP's own 59.92 Hz");
        // The VDP's frame is its own 262 x 342 clocks, not the 59.94 Hz
        // broadcast field (17062) — POM1 989d019b, ported 2026-11-16.
        static_assert(TMS9918::kCyclesPerFrame == 17067, "342 x 262 x 4/21");
    }

    // ── An indexed store's phantom read REACHES the card (NMOS) ──────────
    // `STA $C080,X` with X = $40 reads $C0C0 one cycle before it writes it
    // (M6502::nmosIndexDummyRead). Report from pom2games: the byte lands at
    // the pointer and not one further, "so the read does not reach the card".
    // It does; what differs is the VDP.
    {
        struct Rig {
            Memory mem;
            M6502 cpu{ &mem };
            EzColorCard* card = nullptr;
            explicit Rig(bool window) {
                mem.setCpu(&cpu);
                cpu.setCpuMode(M6502::CpuMode::NMOS);
                auto c = std::make_unique<EzColorCard>();
                card = c.get();
                card->vdp().setSiliconStrictMode(window);
                mem.slotBus().plug(4, std::move(c));
                // R1: 16 KB, display off (the free zone: the chip's easiest).
                mem.memWrite(0xC0C1, 0x80); mem.memWrite(0xC0C1, 0x81);
                card->advanceCycles(64);
            }
            void run(std::initializer_list<uint8_t> code, int instrs) {
                uint16_t a = 0x0300;
                for (uint8_t b : code) mem.memWrite(a++, b);
                cpu.setProgramCounter(0x0300);
                for (int i = 0; i < instrs; ++i) cpu.run(1);
                card->advanceCycles(64);
            }
        };
        // LDX #$40 / LDA #$A5 / STA $C080,X
        const std::initializer_list<uint8_t> store = { 0xA2, 0x40, 0xA9, 0xA5, 0x9D, 0x80, 0xC0 };

        // Window off, each access served on the spot: the phantom read takes
        // the pointer forward and the byte lands one further.
        Rig lax(false);
        lax.mem.memWrite(0xC0C1, 0x00); lax.mem.memWrite(0xC0C1, 0x41);   // write $0100
        lax.run(store, 3);
        CHECK(lax.card->vdp().vramData()[0x0101] == 0xA5 && "phantom read advanced the pointer");
        CHECK(lax.card->vdp().vramAddress() == 0x0102);

        // Window on (the default). The store comes 1 cycle behind the read,
        // inside the access the read scheduled: the write retargets that one
        // pending access, the slot fires once, with the newer byte, at the
        // unmoved pointer. The read is not lost: it is the reason the store
        // counts as a too-fast access. [D] openMSX VDP.cc (via POM1's
        // TMS9918.cpp, see writeData). [F] No capture of a real TMS9918A
        // under a 1 us read→write pair is known; settled by a logic analyser
        // on the VRAM bus of a real card.
        Rig strict(true);
        const uint8_t untouched = strict.card->vdp().vramData()[0x0101];
        strict.mem.memWrite(0xC0C1, 0x00); strict.mem.memWrite(0xC0C1, 0x41);
        strict.card->vdp().resetDroppedWriteCount();
        strict.run(store, 3);
        CHECK(strict.card->vdp().vramData()[0x0100] == 0xA5 && "merged into the read's slot");
        CHECK(strict.card->vdp().vramData()[0x0101] == untouched);
        CHECK(strict.card->vdp().vramAddress() == 0x0101);
        CHECK(strict.card->vdp().droppedWriteCount() == 1 && "and the host can see it");

        // On $C0n1 the phantom read is a status read, in either mode: it
        // clears the byte flip-flop, so a register port written through
        // `STA $C081,X` never completes a pair. No VDP timing in this one.
        Rig flip(true);
        // LDX #$40 / LDA #$34 / STA $C081,X / LDA #$52 / STA $C081,X
        flip.run({ 0xA2, 0x40, 0xA9, 0x34, 0x9D, 0x81, 0xC0, 0xA9, 0x52, 0x9D, 0x81, 0xC0 }, 5);
        CHECK((flip.card->vdp().vramAddress() & 0xFF) == 0x52 && "second byte taken as a first");
        CHECK((flip.card->vdp().vramAddress() & 0x3F00) != 0x1200 && "no write-address command");
    }

    // ── Jumper JP2: /INT on /IRQ, /NMI or nothing (CARDS-016) ───────────
    // [B] BYTE Aug. 1982 Fig. 4: /INT → JP2 → bus pin 29, /NMI. The NMI is
    // taken on the line's FALLING EDGE: one per frame while the handler reads
    // the status, none while /INT stays low, and the bus is a wire-OR of every
    // slot (a second card pulling a line already low is not an edge).
    {
        Memory m;
        int nmis = 0;
        m.slotBus().setNmiRouter([&] { ++nmis; });
        auto mk = [&](int slot) {
            auto c = std::make_unique<EzColorCard>();
            EzColorCard* p = c.get();
            p->vdp().setSiliconStrictMode(false);
            m.slotBus().plug(slot, std::move(c));
            return p;
        };
        auto ctl = [&](int slot) { return uint16_t(0xC081 + slot * 16); };
        auto setR1 = [&](int slot, uint8_t v) { m.memWrite(ctl(slot), v); m.memWrite(ctl(slot), 0x81); };
        auto frame = [](EzColorCard* c) {
            for (int i = 0; i < TMS9918::kCyclesPerFrame / 64 + 1; ++i) c->advanceCycles(64);
        };

        EzColorCard* a = mk(3);
        CHECK(a->interruptWiring() == EzColorCard::InterruptWiring::Nmi && "NMI by default, as published");
        CHECK(EzColorCard::parseInterruptWiring("irq") == EzColorCard::InterruptWiring::Irq);
        CHECK(EzColorCard::parseInterruptWiring("open") == EzColorCard::InterruptWiring::Open);
        CHECK(EzColorCard::parseInterruptWiring("") == EzColorCard::InterruptWiring::Nmi);
        setR1(3, 0xE0);                                     // 16 KB, display on, IE
        frame(a);
        CHECK(nmis == 1 && a->slotNmiAsserted() && !a->slotIrqAsserted() && "NMI, not IRQ");
        frame(a);
        CHECK(nmis == 1 && "/INT still low: no second edge without a status read");
        (void)m.memRead(ctl(3));                            // the handler reads the status
        CHECK(!a->slotNmiAsserted() && !m.slotBus().nmiLineLow());
        frame(a);
        CHECK(nmis == 2 && "released, then the next frame is a new edge");

        // Wire-OR: a second card pulling a line already low is no edge.
        EzColorCard* b = mk(5);
        setR1(5, 0xE0);
        frame(b);
        CHECK(nmis == 2 && b->slotNmiAsserted() && "line already low (slot 3)");
        (void)m.memRead(ctl(3));
        CHECK(m.slotBus().nmiLineLow() && nmis == 2 && "slot 5 still holds it");
        (void)m.memRead(ctl(5));
        CHECK(!m.slotBus().nmiLineLow());
        m.slotBus().plug(5, nullptr);

        // Open: the frame flag rises, nothing leaves the card.
        a->setInterruptWiring(EzColorCard::InterruptWiring::Open);
        frame(a);
        CHECK(a->vdp().irqAsserted() && !a->slotNmiAsserted() && !a->slotIrqAsserted() && nmis == 2);
        // Moving the jumper with /INT low pulls the new line at once: an edge.
        a->setInterruptWiring(EzColorCard::InterruptWiring::Nmi);
        CHECK(nmis == 3 && a->slotNmiAsserted());
        a->setInterruptWiring(EzColorCard::InterruptWiring::Irq);
        CHECK(!a->slotNmiAsserted() && a->slotIrqAsserted() && "back on pin 30");

        // A snapshot restores the /NMI LEVEL with no edge: the saved machine
        // already took it. A spurious NMI here would run the guest's handler
        // twice after every rewind.
        a->setInterruptWiring(EzColorCard::InterruptWiring::Nmi);   // edge 4
        std::vector<uint8_t> nmiBlob;
        a->appendSnapshotState(nmiBlob);
        (void)m.memRead(ctl(3));                            // line high again
        const int nmisBefore = nmis;
        a->loadSnapshotState(nmiBlob.data(), nmiBlob.size());
        CHECK(a->slotNmiAsserted() && m.slotBus().nmiLineLow() && nmis == nmisBefore
              && "restored low, no edge");
        m.slotBus().plug(3, nullptr);
        CHECK(!m.slotBus().nmiLineLow() && "unplug releases the line");

        // Through the real CPU: Memory routes the edge to M6502::setNMI and
        // the next step vectors through $FFFA (3 bytes pushed, I set).
        Memory mc;
        M6502 cpu{ &mc };
        mc.setCpu(&cpu);
        auto c = std::make_unique<EzColorCard>();
        EzColorCard* e = c.get();
        e->vdp().setSiliconStrictMode(false);
        mc.slotBus().plug(3, std::move(c));
        mc.memWrite(0xC0B1, 0xE0); mc.memWrite(0xC0B1, 0x81);
        mc.memWrite(0x0300, 0xEA);                          // NOP
        cpu.setProgramCounter(0x0300);
        cpu.setStatusRegister(uint8_t(cpu.getStatusRegister() | 0x04));   // SEI: NMI ignores it
        const uint8_t sp = cpu.getStackPointer();
        frame(e);
        cpu.run(1);
        // The entry pushed the NOP's own address and P with B clear; read the
        // stack rather than PC, which depends on what $FFFA holds here.
        CHECK(mc.memRead(uint16_t(0x0100 + sp)) == 0x03 && mc.memRead(uint16_t(0x0100 + uint8_t(sp - 1))) == 0x00
              && "NMI entry pushed PC = $0300");
        CHECK((mc.memRead(uint16_t(0x0100 + uint8_t(sp - 2))) & 0x10) == 0 && "an NMI, not a BRK");
    }

    // ── The E-Z Color's picture never takes the Apple's screen ───────────
    // `preferScreenSource` is pinned in card_video_source, but nothing
    // asserted the ANSWER this card gives it. Revert the one-line override and
    // the whole suite stays green while a Videx Videoterm plugged beside an
    // E-Z Color loses its 80 columns — the card's picture belongs to its own
    // monitor (Display ▸ Card Monitor), not to the main screen.
    {
        EzColorCard ez;
        CHECK(!ez.takesMonitor());
        // The other side of the rule lives in videx_videoterm_bus, which is
        // the test that links that card: a Videoterm DOES take the screen.
        CHECK(pom2::preferScreenSource(nullptr, &ez) == &ez);   // alone: it is the source
        CHECK(pom2::preferPanelSource(nullptr, &ez) == &ez);
    }

    if (g_fail) {
        std::fprintf(stderr, "ezcolor_card: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ezcolor_card: PASS\n");
    return 0;
}
