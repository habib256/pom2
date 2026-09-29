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

// videx_videoterm_bus — the Videx Videoterm's bus decode, against MAME
// `a2videoterm.cpp:379-471` and `mc6845.cpp:143-270`, through a real Memory
// (so the $C800 claim and the $CFFF release are SlotBus's, not the test's).
// Synthetic ROM and font: no dump needed.
//
//   * 6845: the $C0B0 latch, $C0B1 writes with MAME's masks, the readable
//     subset (R12-R15 on the HD6845S), write-only registers reading 0
//   * the VRAM bank follows address bits 3-2 of ANY $C0nX access, read or
//     write — including accesses that reach no 6845 register ($C0B6 …)
//   * $CnXX = firmware[$300 + low byte]; writes there change nothing
//   * $C800-$CBFF = firmware[0..$3FF] once slot 3 has claimed the window;
//     $CC00-$CDFF = the 512-byte VRAM window of the selected quarter;
//     $CFFF releases the window
//   * a bus reset puts the window on bank 0 and resets the 6845 (cursor
//     address + latch cleared, timing registers kept)
//   * a 1 KB firmware is padded with $FF (ROMREGION_ERASEFF); other sizes
//     are refused, and so is a char ROM of the wrong size

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

std::vector<uint8_t> syntheticRom(std::size_t n)
{
    std::vector<uint8_t> r(n);
    for (std::size_t i = 0; i < n; ++i)
        r[i] = static_cast<uint8_t>((i * 7 + 3) ^ (i >> 8));
    return r;
}

std::vector<uint8_t> syntheticFont()
{
    std::vector<uint8_t> f(VidexVideotermCard::kCharSetBytes);
    for (std::size_t i = 0; i < f.size(); ++i) f[i] = static_cast<uint8_t>(i * 13);
    return f;
}

void crtcWrite(Memory& mem, uint8_t reg, uint8_t v)
{
    mem.memWrite(0xC0B0, reg);
    mem.memWrite(0xC0B1, v);
}
uint8_t crtcRead(Memory& mem, uint8_t reg)
{
    mem.memWrite(0xC0B0, reg);
    return mem.memRead(0xC0B1);
}

} // namespace

int main()
{
    // ── Loaders ─────────────────────────────────────────────────────────
    {
        VidexVideotermCard c(3);
        expect(!c.loadFirmware(std::vector<uint8_t>(1000)), "a 1000-byte firmware is refused");
        expect(!c.loadFirmware(std::vector<uint8_t>(4096)), "a 4 KB firmware is refused");
        expect(!c.firmwareLoaded(), "no firmware after two refusals");
        expect(c.loadFirmware(std::vector<uint8_t>(1024, 0x11)), "the 1 KB v2.4 is accepted");
        expect(c.expansionRomRead(0x3FF) == 0x11, "1 KB firmware: $CBFF is its last byte");
        expect(c.slotRomRead(0xFF) == 0x11, "1 KB firmware: $CnFF = rom[$3FF]");
        // The upper KB is never visible on the bus, but the padding is what
        // MAME's ROMREGION_ERASEFF leaves there.
        expect(!c.loadCharRoms(std::vector<uint8_t>(100), {}), "a short char ROM is refused");
        expect(!c.loadCharRoms(syntheticFont(), std::vector<uint8_t>(10)),
               "a short alternate char ROM is refused");
        expect(c.loadCharRoms(syntheticFont(), {}), "normal set alone is accepted");
        expect(c.alternateIsDerived(), "no alternate set -> derived as ~normal");
        expect(c.loadCharRoms(syntheticFont(), syntheticFont()), "both sets accepted");
        expect(!c.alternateIsDerived(), "an explicit alternate set is used as given");
    }

    Memory mem;
    auto owned = std::make_unique<VidexVideotermCard>(3);
    const auto rom = syntheticRom(VidexVideotermCard::kRomBytes);
    expect(owned->loadFirmware(rom), "2 KB firmware loads");
    expect(owned->loadCharRoms(syntheticFont(), {}), "font loads");
    VidexVideotermCard* card = owned.get();
    mem.slotBus().plug(3, std::move(owned));
    expect(card->takesC800(), "the card takes $C800 (a2videoterm.cpp:122)");

    // ── 6845 register file ──────────────────────────────────────────────
    crtcWrite(mem, 1, 80);
    crtcWrite(mem, 9, 0xFF);
    expect(card->crtc().reg(1) == 80, "R1 written through $C0B0/$C0B1");
    expect(card->crtc().reg(9) == 0x1F, "R9 masked to 5 bits");
    expect(crtcRead(mem, 1) == 0, "R1 is write-only: reads 0");
    crtcWrite(mem, 12, 0xFF);
    crtcWrite(mem, 13, 0x34);
    expect(crtcRead(mem, 12) == 0x3F, "R12 masked to 6 bits and readable (HD6845S)");
    expect(crtcRead(mem, 13) == 0x34, "R13 readable (HD6845S)");
    crtcWrite(mem, 14, 0x05);
    crtcWrite(mem, 15, 0x67);
    expect(crtcRead(mem, 14) == 0x05 && crtcRead(mem, 15) == 0x67,
           "R14/R15 cursor address read back");
    expect(card->crtc().cursorAddress() == 0x0567, "cursor address decoded");
    crtcWrite(mem, 10, 0xFF);
    expect(card->crtc().reg(10) == 0x7F, "R10 masked to 7 bits");
    crtcWrite(mem, 16, 0x12);
    expect(crtcRead(mem, 16) == 0, "R16 (light pen) is read-only");
    mem.memWrite(0xC0B0, 0xFF);
    expect(card->crtc().latch() == 0x1F, "address latch keeps 5 bits");
    // A write to $C0B2 reaches no 6845 register (RS is A0 on offsets 0/1).
    mem.memWrite(0xC0B0, 13);
    mem.memWrite(0xC0B2, 0x99);
    expect(card->crtc().reg(13) == 0x34, "$C0B2 does not write the 6845");

    // ── VRAM bank = address bits 3-2 of any $C0nX access ────────────────
    (void)mem.memRead(0xC0B0);
    expect(card->vramBank() == 0, "$C0B0 read -> bank 0");
    (void)mem.memRead(0xC0B7);
    expect(card->vramBank() == 512, "$C0B7 read -> bank 1");
    mem.memWrite(0xC0BA, 0);
    expect(card->vramBank() == 1024, "$C0BA write -> bank 2");
    (void)mem.memRead(0xC0BC);
    expect(card->vramBank() == 1536, "$C0BC read -> bank 3");
    mem.memWrite(0xC0B1, 0x00);   // a register write moves it back to 0
    expect(card->vramBank() == 0, "$C0B1 write -> bank 0");

    // ── $CnXX and the $C800 window ──────────────────────────────────────
    expect(mem.memRead(0xC305) == rom[0x305], "$C305 = rom[$305]");
    expect(mem.memRead(0xC3FF) == rom[0x3FF], "$C3FF = rom[$3FF]");
    mem.memWrite(0xC380, 0x00);   // the firmware's claim write: no effect
    expect(mem.memRead(0xC380) == rom[0x380], "a $C3xx write changes nothing");
    expect(mem.slotBus().getActiveExpansionSlot() == 3, "slot 3 claims $C800");
    expect(mem.memRead(0xC800) == rom[0x000], "$C800 = rom[0]");
    expect(mem.memRead(0xCB00) == rom[0x300], "$CB00 = rom[$300] (the $Cn mirror)");
    expect(mem.memRead(0xCBFF) == rom[0x3FF], "$CBFF = rom[$3FF]");
    mem.memWrite(0xC900, 0x55);
    expect(mem.memRead(0xC900) == rom[0x100], "the ROM half ignores writes");

    for (int bank = 0; bank < 4; ++bank) {
        (void)mem.memRead(static_cast<uint16_t>(0xC0B0 + bank * 4));
        mem.memWrite(0xCC00, static_cast<uint8_t>(0xA0 + bank));
        mem.memWrite(0xCDFF, static_cast<uint8_t>(0xB0 + bank));
    }
    for (int bank = 0; bank < 4; ++bank) {
        expect(card->vram()[bank * 512] == 0xA0 + bank, "$CC00 lands at bank*512");
        expect(card->vram()[bank * 512 + 511] == 0xB0 + bank, "$CDFF lands at bank*512+511");
    }
    (void)mem.memRead(0xC0B8);    // bank 2
    expect(mem.memRead(0xCC00) == 0xA2, "$CC00 reads bank 2 back");
    expect(mem.memRead(0xCDFF) == 0xB2, "$CDFF reads bank 2 back");
    mem.memWrite(0xCE10, 0x77);   // $CE00-$CFFE: nothing there
    for (int i = 0; i < 2048; ++i)
        if (card->vram()[i] == 0x77) { expect(false, "$CE10 write reached VRAM"); break; }

    (void)mem.memRead(0xCFFF);
    expect(mem.slotBus().getActiveExpansionSlot() != 3, "$CFFF releases the window");
    (void)mem.memRead(0xC300);
    expect(mem.slotBus().getActiveExpansionSlot() == 3, "touching $C3xx reclaims it");

    // ── Reset ───────────────────────────────────────────────────────────
    crtcWrite(mem, 0, 0x7B);
    crtcWrite(mem, 14, 0x01);
    crtcWrite(mem, 15, 0x23);
    mem.memWrite(0xC0B0, 6);      // latch = R6
    (void)mem.memRead(0xC0BC);    // bank 3
    mem.slotBus().reset();
    expect(card->vramBank() == 0, "reset: window back on bank 0");
    expect(card->crtc().latch() == 0, "reset: 6845 address latch cleared");
    expect(card->crtc().cursorAddress() == 0, "reset: 6845 cursor address cleared");
    expect(card->crtc().reg(0) == 0x7B, "reset: timing registers survive (mc6845.cpp:1036)");
    expect(card->crtc().reg(1) == 80, "reset: R1 survives");
    expect(card->vram()[512] == 0xA1, "reset: VRAM survives");

    if (failures) {
        std::printf("videx_videoterm_bus: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("videx_videoterm_bus: ok\n");
    return 0;
}
