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

// Smoke test for LeChatMauveCard + Apple2Display ChatMauveRGB pipeline.
//
// What this pins:
//   1. FIFO clocking. Default reset state = 0b11 (COL140). Each rising
//      edge of $C05F (AN3 going high) pushes the current $C00C/$C00D
//      level into a 2-bit FIFO. We replay the Arlequin BW560 sequence
//      and assert the FIFO ends up at 0b00.
//   2. ChatMauveRGB direct decoding (no MAME bit-doubler, no half-dot
//      delay, no 7-bit LUT). With FIFO=COL140, $01 at col 0 produces
//      one violet pixel + 6 black; $7F produces 7 white. Both differ
//      visibly from the NTSC pipeline tested in hgr_render_smoke.
//   3. The latch only governs DHGR: BW560 there is 560 plain dots, while
//      single HGR keeps the LCM colour rule whatever the latch reads.
//   4. Lo-res Chat Mauve palette. Indices 5 ($5) and 10 ($A) decode to
//      DISTINCT grays under Chat Mauve, where NTSC's //gs-corrected
//      palette merges them.
//   5. Falling back to ColorNTSC when the card isn't plugged keeps the
//      NTSC pipeline intact (regression guard).
//   6-12. The Eve (docs/chatmauve_plan.md § 3.4): the sixteen switches and
//      CPREG, the CPREG auto-write through Memory's aux shadow (and its
//      coexistence with write watches), TXT16 with the Eve nibble order,
//      TXTGREEN, table IX-1 (BW560 / CP280 / blank / COL280A), what AN3 off
//      does to single HGR on each variant, and the v3 snapshot blob.

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kWhite = 0xFFFFFFFFu;

constexpr uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}

void writeHgrByte(Memory& mem, int y, int col, uint8_t v) {
    const uint16_t addr = static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
    mem.memWrite(static_cast<uint16_t>(addr + col), v);
}

void clearHgrLine(Memory& mem, int y) {
    for (int col = 0; col < 40; ++col) writeHgrByte(mem, y, col, 0x00);
}

uint16_t loresAddr(int textRow) {
    return static_cast<uint16_t>(0x0400 + 0x80 * (textRow & 7) + 0x28 * (textRow >> 3));
}

const uint32_t* pixelAt(const Apple2Display& d, int x, int y) {
    return d.pixels() + (y * d.width() + x);
}

uint8_t r8(uint32_t c) { return  c        & 0xFFu; }
uint8_t g8(uint32_t c) { return (c >> 8 ) & 0xFFu; }
uint8_t b8(uint32_t c) { return (c >> 16) & 0xFFu; }

// Replay one bit through the Arlequin sequence (data on 80COL, clock on
// AN3). Each bit needs: write 80COL = bit, drop AN3, raise AN3.
void clockOneBitThroughFifo(Memory& mem, bool dataBit) {
    (void)mem.memRead(dataBit ? 0xC00D : 0xC00C);
    (void)mem.memRead(0xC05E);
    (void)mem.memRead(0xC05F);
}

} // namespace

int main()
{
    // ─── 1. FIFO clocking ────────────────────────────────────────────────
    {
        Memory mem;
        auto card = std::make_unique<LeChatMauveCard>();
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));

        // After plug + onReset() the card is in COL140 (FIFO=11).
        raw->onReset();
        assert(raw->fifoBits() == 0b11);
        assert(raw->currentMode() == LeChatMauveCard::RenderMode::COL140);

        // Push 0,0 → BW560.
        clockOneBitThroughFifo(mem, false);
        clockOneBitThroughFifo(mem, false);
        assert(raw->fifoBits() == 0b00);
        assert(raw->currentMode() == LeChatMauveCard::RenderMode::BW560);

        // Push 1,1 → COL140.
        clockOneBitThroughFifo(mem, true);
        clockOneBitThroughFifo(mem, true);
        assert(raw->fifoBits() == 0b11);
        assert(raw->currentMode() == LeChatMauveCard::RenderMode::COL140);

        // Hammering $C05F without re-arming $C05E shouldn't shift again
        // (rising-edge detection only triggers once per up-down cycle).
        (void)mem.memRead(0xC00C);  // data = 0
        (void)mem.memRead(0xC05F);  // first call: rising edge if AN3 was low
        const uint8_t after = raw->fifoBits();
        (void)mem.memRead(0xC05F);  // no $C05E in between → no edge
        (void)mem.memRead(0xC05F);
        assert(raw->fifoBits() == after);

        // After Apple II reset, FIFO returns to COL140.
        raw->onReset();
        assert(raw->fifoBits() == 0b11);

        // Round 10 #8: AN3 powers up HIGH, so a bare $C05F right after reset
        // is NOT a rising edge and must NOT clock the FIFO. (With an3Prev
        // mis-initialised to false this spuriously shifted COL140 → 0b10.)
        (void)mem.memRead(0xC00C);   // data bit = 0 (would land in FIFO if it shifted)
        (void)mem.memRead(0xC05F);   // bare $C05F after reset — no preceding $C05E
        assert(raw->fifoBits() == 0b11 &&
               "bare $C05F after reset must not shift (AN3 powers up high)");
    }

    // ─── 2. ChatMauveRGB COL140 HGR decode ───────────────────────────────
    {
        Memory mem;
        Apple2Display display;
        auto card = std::make_unique<LeChatMauveCard>();
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);

        // Force HGR mode + page 1.
        (void)mem.memRead(0xC050);
        (void)mem.memRead(0xC057);
        (void)mem.memRead(0xC054);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

        // Card state at construction = COL140 (FIFO=11) — no need to clock.
        assert(raw->currentMode() == LeChatMauveCard::RenderMode::COL140);

        // Chat Mauve HGR now renders into frame80 natively (560-wide),
        // so the dot-grid stride is doubled. The decode follows AppleWin
        // RGBMonitor.cpp UpdateHiResRGBCell: a pixel is COLOUR only when
        // it forms a lone 010 / 101 pattern with its neighbours (taking
        // its even-aligned pair's palette entry, 2 output dots); any
        // other pattern is plain black/white at full pixel resolution.
        // (`width()` only flips to 560 after the first render, so we
        // check it post-render below.)

        // $01 at col 0: lone bit 0 (pattern 010) → pair code 01 at MSB=0
        // → magenta on ITS 2 output dots; neighbour pixels are black.
        clearHgrLine(mem, 0);
        writeHgrByte(mem, 0, 0, 0x01);
        display.render(mem);
        assert(display.width() == 560);
        // kChatMauveHGR[0][1] = Feline MAGENTA rgb(0xaa, 0x1a, 0xd1).
        const uint32_t magenta = pack(0xAA, 0x1A, 0xD1);
        for (int x = 0; x < 2; ++x) assert(*pixelAt(display, x, 0) == magenta);
        // The rest of the row: clear pixels → black.
        for (int x = 2; x < 560; ++x) assert(*pixelAt(display, x, 0) == kBlack);

        // $02 at col 0: lone bit 1 (010) → green (bank 0, code 10) on
        // dots 2..3; pixel 0 stays black.
        clearHgrLine(mem, 1);
        writeHgrByte(mem, 1, 0, 0x02);
        display.render(mem);
        // kChatMauveHGR[0][2] = Feline GREEN rgb(0x6f, 0xe6, 0x2c).
        const uint32_t green = pack(0x6F, 0xE6, 0x2C);
        assert(*pixelAt(display, 0, 1) == kBlack);
        assert(*pixelAt(display, 1, 1) == kBlack);
        for (int x = 2; x < 4; ++x) assert(*pixelAt(display, x, 1) == green);

        // $81 at col 0: bit0=1 only, MSB=1 → bank 1 → blue on dots 0..1.
        // No half-dot delay (that's NTSC-only), no fringing.
        clearHgrLine(mem, 2);
        writeHgrByte(mem, 2, 0, 0x81);
        display.render(mem);
        // kChatMauveHGR[1][1] = Feline BLUE rgb(0x00, 0x8a, 0xb5).
        const uint32_t blue = pack(0x00, 0x8A, 0xB5);
        for (int x = 0; x < 2; ++x) assert(*pixelAt(display, x, 2) == blue);
        // Critically, dots 2+ should be BLACK (no fringing — NTSC would
        // smear here because of the half-dot phase shift).
        assert(*pixelAt(display, 2, 2) == kBlack);
        assert(*pixelAt(display, 4, 2) == kBlack);

        // $7F full row: every pair = 11 → white.
        clearHgrLine(mem, 3);
        for (int col = 0; col < 40; ++col) writeHgrByte(mem, 3, col, 0x7F);
        display.render(mem);
        for (int x = 0; x < 560; ++x) assert(*pixelAt(display, x, 3) == kWhite);
    }

    // ─── 3. The latch is a DHGR thing: BW560 forces strict B/W THERE, and
    //        single HGR stays the LCM colour rule whatever the latch says ──
    //
    // AppleWin `updateScreenSingleHires40RGB` calls UpdateHiResRGBCell for
    // every RGB-card HGR frame without looking at g_rgbMode; only the DHGR
    // painter consults RGB_Is560Mode(). POM2 used to make HGR mono when the
    // latch read 00 — its own invention, gone.
    {
        Memory mem;
        mem.setIIEMode(true);
        Apple2Display display;
        display.setAuxMemory(mem.auxData());
        auto card = std::make_unique<LeChatMauveCard>();
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);

        (void)mem.memRead(0xC050);
        (void)mem.memRead(0xC057);
        (void)mem.memRead(0xC054);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        raw->overrideMode(LeChatMauveCard::RenderMode::BW560);

        // Single HGR, latch at BW560: a lone dot is still the LCM magenta.
        clearHgrLine(mem, 0);
        writeHgrByte(mem, 0, 0, 0x01);
        display.render(mem);
        assert(display.width() == 560);
        const uint32_t magenta = pack(0xAA, 0x1A, 0xD1);
        assert(*pixelAt(display, 0, 0) == magenta && *pixelAt(display, 1, 0) == magenta);

        // DHGR (80COL + AN3 off), latch at BW560: 560 dots of plain black and
        // white — aux bit 0 is dot 0; a pattern that would be a colour cell
        // under COL140 ($A = 1010) is two white dots and two black ones.
        mem.memWrite(0xC00D, 0);
        (void)mem.memRead(0xC05E);
        mem.auxDataMutable()[0x2000] = 0x0A;
        mem.memWrite(0x2000, 0x00);
        display.render(mem);
        assert(*pixelAt(display, 0, 0) == kBlack && *pixelAt(display, 1, 0) == kWhite);
        assert(*pixelAt(display, 2, 0) == kBlack && *pixelAt(display, 3, 0) == kWhite);
        for (int x = 4; x < 560; ++x) assert(*pixelAt(display, x, 0) == kBlack);
        // ...and COL140 makes the same nibble one grey cell.
        raw->overrideMode(LeChatMauveCard::RenderMode::COL140);
        display.render(mem);
        const uint32_t grey1 = 0xFF7E979Fu;   // Feline idx 5
        for (int x = 0; x < 4; ++x) assert(*pixelAt(display, x, 0) == grey1);
    }

    // ─── 4. Lo-res Chat Mauve palette: distinct grays at 5 / A ───────────
    {
        Memory mem;
        Apple2Display display;
        auto card = std::make_unique<LeChatMauveCard>();
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);

        // Force lo-res mode (graphics, lo-res, page 1, no mixed).
        (void)mem.memRead(0xC050);
        (void)mem.memRead(0xC056);
        (void)mem.memRead(0xC054);
        (void)mem.memRead(0xC052);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

        // Lo-res byte $55 at row 0 col 0: low nibble = 5 (gray 1), high = 5.
        // Lo-res byte $AA at row 0 col 1: low nibble = A (gray 2), high = A.
        const uint16_t row0 = loresAddr(0);
        mem.memWrite(static_cast<uint16_t>(row0 + 0), 0x55);
        mem.memWrite(static_cast<uint16_t>(row0 + 1), 0xAA);
        display.render(mem);

        // Block (col=0, blockRow=0) covers x=[0..6], y=[0..3].
        const uint32_t gray1 = *pixelAt(display, 3, 1);
        // Block (col=1, blockRow=0) covers x=[7..13], y=[0..3].
        const uint32_t gray2 = *pixelAt(display, 10, 1);

        // AppleWin Feline palette: index 5 = rgb(0x9f,0x97,0x7e) (olive
        // tint), index 10 = rgb(0x78,0x68,0x7f) (mauve tint). They are
        // tinted (R/G/B differ by ~30 within each gray), but visibly
        // DISTINCT from each other — that's the whole point of the
        // Chat Mauve palette vs NTSC composite (which collapses 5 ≡ 10).
        //
        // The empirical capture's perceived luminance puts gray1 (idx 5)
        // *brighter* than gray2 (idx 10) — the opposite of POM2's old
        // synthetic 0x55 / 0xAA pair. We pin the trademark (distinct +
        // tinted) rather than the previous arbitrary "darker → lighter"
        // ordering.
        assert(gray1 != gray2);
        const int lum1 = (int(r8(gray1)) + int(g8(gray1)) + int(b8(gray1))) / 3;
        const int lum2 = (int(r8(gray2)) + int(g8(gray2)) + int(b8(gray2))) / 3;
        assert(std::abs(lum1 - lum2) >= 16);                // distinct luminance
        assert(std::abs(int(r8(gray1)) - int(b8(gray1))) >= 8);   // tinted (not pure)
        assert(std::abs(int(r8(gray2)) - int(b8(gray2))) >= 4);   // tinted (not pure)
    }

    // ─── 5. Dragon Wars compat: invertBit7 flips the MIXED-mode selector ──
    //
    // AppleWin's `-rgb-card-invert-bit7` is `RGB_IsMixModeInvertBit7()`: it
    // inverts the per-byte colour/mono selector of the mixed DHGR mode and
    // nothing else — the HGR bank bit is left alone (UpdateHiResRGBCell never
    // looks at the flag). POM2 used to XOR the HGR bank too; that is gone.
    {
        Memory mem;
        mem.setIIEMode(true);
        Apple2Display display;
        display.setAuxMemory(mem.auxData());
        auto card = std::make_unique<LeChatMauveCard>();
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        (void)mem.memRead(0xC050); (void)mem.memRead(0xC057); (void)mem.memRead(0xC054);
        mem.memWrite(0xC00D, 0); (void)mem.memRead(0xC05E);       // DHGR
        raw->overrideMode(LeChatMauveCard::RenderMode::Mixed);
        clearHgrLine(mem, 0);
        mem.auxDataMutable()[0x2000] = 0x0A;                      // bit 7 clear: BW under the patent
        mem.memWrite(0x2000, 0x00);
        const uint32_t grey1 = 0xFF7E979Fu;

        assert(!raw->invertBit7());
        display.render(mem);
        assert(*pixelAt(display, 0, 0) == kBlack && *pixelAt(display, 1, 0) == kWhite);  // mono dots
        raw->setInvertBit7(true);
        display.render(mem);
        for (int x = 0; x < 4; ++x) assert(*pixelAt(display, x, 0) == grey1);            // a colour cell
        raw->setInvertBit7(false);

        // Single HGR: the flag changes nothing — $01 is bank 0 magenta either way.
        (void)mem.memRead(0xC05F); mem.memWrite(0xC00C, 0);
        clearHgrLine(mem, 1);
        writeHgrByte(mem, 1, 0, 0x01);
        const uint32_t magenta = pack(0xAA, 0x1A, 0xD1);
        display.render(mem);
        assert(*pixelAt(display, 0, 1) == magenta);
        raw->setInvertBit7(true);
        display.render(mem);
        assert(*pixelAt(display, 0, 1) == magenta);
        raw->setInvertBit7(false);
    }

    // ─── 6. Eve registers $C0B0-$C0BF — sixteen switches + CPREG ─────────
    //
    // Eve manual IX (docs/chatmauve_plan.md § 3.4): eight switches × off/on
    // at the slot-3 addresses, ALL OFF at power-on, any access decodes the
    // address, every WRITE also latches the data byte into CPREG. Ctrl-Reset
    // clears the switches unless LOCKRES. A Féline has no registers there.
    {
        using Sw = LeChatMauveCard::EveSwitch;
        Memory mem;
        mem.setIIEMode(true);
        auto card = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Eve);
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));

        assert(raw->eveSwitches() == 0 && raw->cpreg() == 0);   // power-on
        (void)mem.memRead(0xC0B9);                              // TXT16 on (read strobe)
        assert(raw->eveSwitch(Sw::TXT16));
        assert(raw->cpreg() == 0);                              // a read carries no byte
        mem.memWrite(0xC0B8, 0x5A);                             // TXT16 off, CPREG = $5A
        assert(!raw->eveSwitch(Sw::TXT16));
        assert(raw->cpreg() == 0x5A);
        mem.memWrite(0xC0BB, 0x93);                             // TXTGREEN on, CPREG = $93
        assert(raw->eveSwitch(Sw::TXTGREEN) && raw->cpreg() == 0x93);
        mem.memWrite(0xC0B1, 0x00); mem.memWrite(0xC0B3, 0x00);
        mem.memWrite(0xC0B5, 0x00); mem.memWrite(0xC0B7, 0x00);
        mem.memWrite(0xC0BD, 0x00);
        assert(raw->eveSwitch(Sw::ENHRCPREG) && raw->eveSwitch(Sw::HR1) &&
               raw->eveSwitch(Sw::HR2) && raw->eveSwitch(Sw::HR3) &&
               raw->eveSwitch(Sw::LOCKCPREG));
        assert(raw->cpreg() == 0x00);

        // Ctrl-Reset clears everything — LOCKRES off.
        raw->onReset();
        assert(raw->eveSwitches() == 0);
        // With LOCKRES on the card survives the reset (LOCKRES included).
        mem.memWrite(0xC0BF, 0x11);
        mem.memWrite(0xC0B9, 0x11);
        raw->onReset();
        assert(raw->eveSwitch(Sw::LOCKRES) && raw->eveSwitch(Sw::TXT16));
        assert(raw->cpreg() == 0x11);                           // a register, not a switch

        // A Féline ignores the whole window.
        Memory mem2;
        auto feline = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Feline);
        LeChatMauveCard* fraw = feline.get();
        mem2.slotBus().plug(7, std::move(feline));
        mem2.memWrite(0xC0B9, 0x5A); (void)mem2.memRead(0xC0BB);
        assert(fraw->eveSwitches() == 0 && fraw->cpreg() == 0);

        // Slot-3 collision guard still applies to the widened window: a
        // foreign card in slot 3 (any non-Chat-Mauve peripheral) hides it.
        struct Foreign : SlotPeripheral {
            std::string_view name() const override { return "SSC"; }
        };
        Memory mem3;
        mem3.setIIEMode(true);
        auto eve3 = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Eve);
        LeChatMauveCard* e3 = eve3.get();
        mem3.slotBus().plug(7, std::move(eve3));
        mem3.slotBus().plug(3, std::make_unique<Foreign>());
        mem3.memWrite(0xC0B1, 0x77);
        assert(e3->eveSwitches() == 0 && e3->cpreg() == 0);
    }

    // ─── 7. CPREG auto-write — Memory's aux shadow ───────────────────────
    //
    // The LLE-relevant mechanism: a CPU write that lands in MAIN text page
    // (TXT16 on) or HGR page (ENHRCPREG on) makes the card write CPREG into
    // AUX at the same address; LOCKCPREG freezes it. Zero cost on the hot
    // path — the page is diverted through writable[] like a write watch —
    // so the interplay with a real write watch is pinned too.
    {
        Memory mem;
        mem.setIIEMode(true);
        auto card = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Eve);
        LeChatMauveCard* raw = card.get();
        raw->setMemory(&mem);
        mem.slotBus().plug(7, std::move(card));
        const uint8_t* aux = mem.auxData();

        // Nothing armed: a text write is just a text write.
        mem.memWrite(0x0400, 0xC1);
        assert(mem.data()[0x0400] == 0xC1 && aux[0x0400] == 0x00);
        assert(!mem.auxShadowText() && !mem.auxShadowHgr());

        // The manual's recipe: POKE -16199,16*F+C → TXT16 on, CPREG = $F1
        // (background 15, dot 1); then every character PRINTed gets it.
        mem.memWrite(0xC0B9, 0xF1);
        assert(mem.auxShadowText() && !mem.auxShadowHgr());
        mem.memWrite(0x0401, 0xC2);
        assert(mem.data()[0x0401] == 0xC2);
        assert(aux[0x0401] == 0xF1);
        mem.memWrite(0x07FF, 0xA0);  assert(aux[0x07FF] == 0xF1);
        mem.memWrite(0x0800, 0xA0);  assert(aux[0x0800] == 0x00);   // page 2: outside
        mem.memWrite(0x2000, 0x7F);  assert(aux[0x2000] == 0x00);   // HGR: ENHRCPREG off

        // A new CPREG is what the next writes deposit.
        mem.memWrite(0xC0B9, 0x3C);
        mem.memWrite(0x0402, 0xC3);  assert(aux[0x0402] == 0x3C);

        // ENHRCPREG extends it to the HGR page (CP280's HPLOT).
        mem.memWrite(0xC0B1, 0x3C);
        assert(mem.auxShadowHgr());
        mem.memWrite(0x2000, 0x7F);  assert(mem.data()[0x2000] == 0x7F && aux[0x2000] == 0x3C);
        mem.memWrite(0x3FFF, 0x01);  assert(aux[0x3FFF] == 0x3C);
        mem.memWrite(0x4000, 0x01);  assert(aux[0x4000] == 0x00);   // page 2: outside

        // LOCKCPREG freezes both (the CPU's own write still lands).
        mem.memWrite(0xC0BD, 0x3C);
        assert(!mem.auxShadowText() && !mem.auxShadowHgr());
        mem.memWrite(0x0403, 0xC4);  assert(mem.data()[0x0403] == 0xC4 && aux[0x0403] == 0x00);
        mem.memWrite(0xC0BC, 0x3C);                                // unlock
        assert(mem.auxShadowText() && mem.auxShadowHgr());

        // A write the MMU already routes to AUX (80STORE + PAGE2) IS the aux
        // byte — the card does not overwrite it with CPREG.
        mem.memWrite(0xC001, 0); (void)mem.memRead(0xC055);
        mem.memWrite(0x0404, 0xC5);
        assert(aux[0x0404] == 0xC5 && mem.data()[0x0404] != 0xC5);
        mem.memWrite(0xC000, 0); (void)mem.memRead(0xC054);

        // Write watch on a shadowed address: both mechanisms divert the same
        // byte; disarming one must not lose the other or drop the write.
        mem.setWriteWatch(0x0405, true);
        mem.memWrite(0x0405, 0xC6);
        assert(mem.data()[0x0405] == 0xC6 && aux[0x0405] == 0x3C);
        mem.setWriteWatch(0x0405, false);
        mem.memWrite(0x0405, 0xC7);
        assert(mem.data()[0x0405] == 0xC7 && aux[0x0405] == 0x3C);  // shadow still on
        mem.setWriteWatch(0x0406, true);
        mem.memWrite(0xC0BD, 0x00);                                // LOCKCPREG: shadow off
        mem.memWrite(0x0406, 0xC8);
        assert(mem.data()[0x0406] == 0xC8);                        // the watch keeps it writable
        mem.setWriteWatch(0x0406, false);
        mem.memWrite(0x0406, 0xC9);
        assert(mem.data()[0x0406] == 0xC9);                        // permission restored
        mem.memWrite(0xC0BC, 0x00);

        // Unplugging the card disarms what it programmed.
        mem.slotBus().unplug(7);
        assert(!mem.auxShadowText() && !mem.auxShadowHgr());
        mem.memWrite(0x0407, 0xCA);
        assert(mem.data()[0x0407] == 0xCA && aux[0x0407] == 0x00);
    }

    // ─── 8. Eve TXT16 — colour text, hi nibble = BACKGROUND ─────────────
    //
    // Manual IV-2.2 / III-2: `POKE -16199,16*F+C` — F (fond) in the high
    // nibble, C (couleur du caractère) in the low one; the opposite of the
    // Video-7 order the card used to render. Needs 80COL off; no AN3
    // condition.
    {
        Memory mem;
        mem.setIIEMode(true);
        Apple2Display display;
        display.setAuxMemory(mem.auxData());
        auto card = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Eve);
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        (void)mem.memRead(0xC051);        // TEXT
        mem.memWrite(0xC00C, 0);          // 80COL off

        const uint16_t r0 = loresAddr(0);
        mem.memWrite(r0, 0xA0);           // normal space: every dot = background
        mem.auxDataMutable()[r0] = 0x93;  // F = 9 (orange), C = 3 (magenta)
        const uint32_t orange  = pack(0xFF, 0x72, 0x47);
        const uint32_t magenta = pack(0xAA, 0x1A, 0xD1);

        // TXT16 off: plain text, the space is black.
        display.render(mem);
        assert(*pixelAt(display, 0, 0) == kBlack);

        mem.memWrite(0xC0B9, 0x93);       // TXT16 on
        display.render(mem);
        assert(display.width() == 560);
        for (int x = 0; x < 14; ++x) assert(*pixelAt(display, x, 0) == orange);   // background = hi nibble
        mem.memWrite(r0, 0x20);           // inverse space: every dot = foreground
        display.render(mem);
        for (int x = 0; x < 14; ++x) assert(*pixelAt(display, x, 0) == magenta);

        // 80COL on → TXT16 has no effect (80-col text is mono only).
        mem.memWrite(0xC00D, 0);
        display.render(mem);
        for (int x = 0; x < 560; ++x) {
            const uint32_t c = *pixelAt(display, x, 0);
            assert(c == kBlack || c == kWhite);
        }
    }

    // ─── 9. Eve TXTGREEN — white → green, 40 and 80 columns ─────────────
    {
        Memory mem;
        mem.setIIEMode(true);
        Apple2Display display;
        display.setAuxMemory(mem.auxData());
        auto card = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Eve);
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        (void)mem.memRead(0xC051);
        mem.memWrite(0xC00C, 0);
        mem.memWrite(loresAddr(0), 0x20);            // inverse space = a white block
        mem.memWrite(loresAddr(1), 0xA0);            // normal space below it (RAM is $00 = inverse '@')
        const uint32_t green = pack(0x33, 0xFF, 0x33);

        display.render(mem);
        assert(*pixelAt(display, 0, 0) == kWhite);
        mem.memWrite(0xC0BB, 0x00);                  // TXTGREEN on
        display.render(mem);
        assert(*pixelAt(display, 0, 0) == green);
        assert(*pixelAt(display, 0, 8) == kBlack);   // row 1 is empty: untouched

        mem.memWrite(0xC00D, 0);                     // 80 columns: still green
        mem.auxDataMutable()[loresAddr(0)] = 0x20;   // aux = even column 0
        display.render(mem);
        assert(display.width() == 560);
        assert(*pixelAt(display, 0, 0) == green);

        // Mixed HGR + text: only the text band is tinted.
        mem.memWrite(0xC00C, 0);
        (void)mem.memRead(0xC050); (void)mem.memRead(0xC057); (void)mem.memRead(0xC053);
        for (int col = 0; col < 40; ++col) writeHgrByte(mem, 0, col, 0x7F);   // white HGR row
        mem.memWrite(loresAddr(20), 0x20);
        display.render(mem);
        assert(*pixelAt(display, 0, 0) == kWhite);   // graphics stays white
        assert(*pixelAt(display, 0, 160) == green);  // text band is green
        mem.memWrite(0xC0BA, 0x00);
        display.render(mem);
        assert(*pixelAt(display, 0, 160) == kWhite);
    }

    // ─── 10. Eve table IX-1 — HR1/HR2/HR3 in DHGR (AN3 off, 80COL on) ────
    {
        using Sw = LeChatMauveCard::EveSwitch;
        using DM = LeChatMauveCard::DhgrMode;
        Memory mem;
        mem.setIIEMode(true);
        Apple2Display display;
        display.setAuxMemory(mem.auxData());
        auto card = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Eve);
        LeChatMauveCard* raw = card.get();
        raw->setMemory(&mem);
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        (void)mem.memRead(0xC050); (void)mem.memRead(0xC057); (void)mem.memRead(0xC054);
        mem.memWrite(0xC00D, 0);          // 80COL on
        (void)mem.memRead(0xC05E);        // AN3 off → DHGR
        const uint16_t a0 = static_cast<uint16_t>(0x2000);   // row 0
        clearHgrLine(mem, 0);
        mem.memWrite(a0, 0x7F);           // main col 0: 7 dots lit
        mem.auxDataMutable()[a0] = 0x93;  // aux col 0: bits 0,1,4,7 → as colours F=9 C=3
        const uint32_t orange  = pack(0xFF, 0x72, 0x47);
        const uint32_t magenta = pack(0xAA, 0x1A, 0xD1);

        // HR all off: COL140 whatever the latch says (Purplesoft's `& GR 6`
        // leaves the latch at 00 and expects COL140).
        assert(raw->dhgrMode() == DM::COL140);
        raw->overrideMode(LeChatMauveCard::RenderMode::BW560);
        assert(raw->dhgrMode() == DM::COL140);
        // HR2 + HR3 (Purplesoft's `& GR 10`): BW560 — aux bit 0 is dot 0
        // (lit), aux bit 2 is dot 2 (dark). HR3 alone (the scan's reading of
        // that row) is kept on BW560 too.
        raw->setEveSwitch(Sw::HR2, true); raw->setEveSwitch(Sw::HR3, true);
        assert(raw->dhgrMode() == DM::BW560);
        display.render(mem);
        assert(display.width() == 560);
        assert(*pixelAt(display, 0, 0) == kWhite && *pixelAt(display, 2, 0) == kBlack);
        raw->setEveSwitch(Sw::HR2, false);
        assert(raw->dhgrMode() == DM::BW560);
        // HR1 + HR2 + HR3 (`& GR 9`): CP280 — main bits = dots, aux = colours, hi = bg.
        raw->setEveSwitch(Sw::HR1, true); raw->setEveSwitch(Sw::HR2, true);
        assert(raw->dhgrMode() == DM::CP280);
        display.render(mem);
        for (int x = 0; x < 14; ++x) assert(*pixelAt(display, x, 0) == magenta);   // fg = lo nibble
        mem.memWrite(a0, 0x00);
        display.render(mem);
        for (int x = 0; x < 14; ++x) assert(*pixelAt(display, x, 0) == orange);    // bg = hi nibble
        // ...and with 80COL OFF, which is how Purplesoft's `& GR 9` runs it
        // (the Eve is the aux memory: it has the attribute byte regardless).
        mem.memWrite(0xC00C, 0);
        assert(raw->hgrMode(/*an3On=*/false) == LeChatMauveCard::HgrMode::Cp280);
        mem.memWrite(a0, 0x7F);
        display.render(mem);
        for (int x = 0; x < 14; ++x) assert(*pixelAt(display, x, 0) == magenta);
        mem.memWrite(a0, 0x00);
        display.render(mem);
        for (int x = 0; x < 14; ++x) assert(*pixelAt(display, x, 0) == orange);
        mem.memWrite(0xC00D, 0);
        // HR1 + HR2: blank. CPREG keeps working underneath (ENHRCPREG on).
        raw->setEveSwitch(Sw::HR3, false);
        assert(raw->dhgrMode() == DM::Blank);
        display.render(mem);
        for (int x = 0; x < 560; ++x) assert(*pixelAt(display, x, 0) == kBlack);
        mem.memWrite(0xC0B1, 0x5A);
        mem.memWrite(a0 + 1, 0x01);
        assert(mem.auxData()[a0 + 1] == 0x5A);
        // HR1 alone: COL280A — the 560 stream in 2-dot cells, exactly the
        // bytes Purplesoft's `& PLOT` writes: `& COLOR= 9` → (main $2A,
        // aux $55) = code 1 = orange; `12` → ($55, $2A) = code 2 = green;
        // `15` → ($7F, $7F) = code 3 = white; a byte pair of zeros = black.
        raw->setEveSwitch(Sw::HR2, false);
        assert(raw->dhgrMode() == DM::COL280A);
        clearHgrLine(mem, 0);
        mem.memWrite(a0, 0x2A);     mem.auxDataMutable()[a0]     = 0x55;
        mem.memWrite(a0 + 1, 0x55); mem.auxDataMutable()[a0 + 1] = 0x2A;
        mem.memWrite(a0 + 2, 0x7F); mem.auxDataMutable()[a0 + 2] = 0x7F;
        // Column 3: both banks zero — the aux byte LAST, because ENHRCPREG is
        // on and the main write just deposited CPREG there (§ 7).
        mem.memWrite(a0 + 3, 0x00); mem.auxDataMutable()[a0 + 3] = 0x00;
        display.render(mem);
        const uint32_t green = pack(0x6F, 0xE6, 0x2C);
        for (int x = 0;  x < 14; ++x) assert(*pixelAt(display, x, 0) == orange);
        for (int x = 14; x < 28; ++x) assert(*pixelAt(display, x, 0) == green);
        for (int x = 28; x < 42; ++x) assert(*pixelAt(display, x, 0) == kWhite);
        assert(*pixelAt(display, 42, 0) == kBlack);
        // COL280B: the same codes through the second palette (`& COLOR= 7`,
        // `11`, `13` on Purplesoft's side): light blue, pink, yellow.
        raw->setEveSwitch(Sw::HR1, false); raw->setEveSwitch(Sw::HR2, true);
        assert(raw->dhgrMode() == DM::COL280B);
        display.render(mem);
        for (int x = 0;  x < 14; ++x) assert(*pixelAt(display, x, 0) == pack(0x9F, 0x9E, 0xFF));
        for (int x = 14; x < 28; ++x) assert(*pixelAt(display, x, 0) == pack(0xFF, 0x7A, 0xCF));
        for (int x = 28; x < 42; ++x) assert(*pixelAt(display, x, 0) == pack(0xFF, 0xF6, 0x7B));
    }

    // ─── 11. AN3 off in single HGR: Féline mono, Video-7 F/B, Eve keeps LCM ─
    {
        auto run = [](LeChatMauveCard::Variant v, uint32_t& dot0, uint32_t& dot14) {
            Memory mem;
            mem.setIIEMode(true);
            Apple2Display display;
            display.setAuxMemory(mem.auxData());
            auto card = std::make_unique<LeChatMauveCard>(7, v);
            LeChatMauveCard* raw = card.get();
            mem.slotBus().plug(7, std::move(card));
            display.setChatMauveCard(raw);
            display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
            (void)mem.memRead(0xC050); (void)mem.memRead(0xC057); (void)mem.memRead(0xC054);
            mem.memWrite(0xC00C, 0);      // 80COL off
            (void)mem.memRead(0xC05E);    // AN3 off — `POKE -16290,0`
            clearHgrLine(mem, 0);
            writeHgrByte(mem, 0, 0, 0x01);           // a lone dot → magenta under LCM
            writeHgrByte(mem, 0, 1, 0x7F);           // a white byte
            mem.auxDataMutable()[0x2000 + 1] = 0x93; // F/B colours for byte 1
            display.render(mem);
            assert(display.width() == 560);
            dot0  = *pixelAt(display, 0, 0);
            dot14 = *pixelAt(display, 14, 0);
        };
        const uint32_t magenta = pack(0xAA, 0x1A, 0xD1);
        const uint32_t orange  = pack(0xFF, 0x72, 0x47);
        uint32_t d0 = 0, d14 = 0;
        run(LeChatMauveCard::Variant::Feline, d0, d14);
        assert(d0 == kWhite && d14 == kWhite);        // monochrome
        run(LeChatMauveCard::Variant::IIcAdapter, d0, d14);
        assert(d0 == kWhite && d14 == kWhite);
        run(LeChatMauveCard::Variant::Video7, d0, d14);
        assert(d14 == orange);                         // F/B: fg = hi nibble 9
        run(LeChatMauveCard::Variant::Eve, d0, d14);
        assert(d0 == magenta && d14 == kWhite);        // LCM colour, unchanged
    }

    // ─── 12. Snapshot v3 round trip, and a v2 blob's toggles land on switches ─
    {
        using Sw = LeChatMauveCard::EveSwitch;
        LeChatMauveCard eve(7, LeChatMauveCard::Variant::Eve);
        eve.overrideMode(LeChatMauveCard::RenderMode::BW560);
        eve.setEveSwitch(Sw::TXT16, true); eve.setEveSwitch(Sw::LOCKRES, true);
        eve.setCpreg(0x5A);
        std::vector<uint8_t> blob;
        eve.appendSnapshotState(blob);
        assert(blob.size() == 19 && blob[2] == 5);
        LeChatMauveCard back(7, LeChatMauveCard::Variant::Eve);
        back.loadSnapshotState(blob.data(), blob.size());
        assert(back.currentMode() == LeChatMauveCard::RenderMode::BW560);
        assert(back.eveSwitches() == eve.eveSwitches() && back.cpreg() == 0x5A);
        // v2: 'C' 'M' 2 fifo mode an3 80col colourText duochrome.
        const uint8_t v2[9] = { 'C', 'M', 2, 0b11, 0b11, 1, 0, 1, 1 };
        LeChatMauveCard fromV2(7, LeChatMauveCard::Variant::Eve);
        fromV2.loadSnapshotState(v2, sizeof v2);
        assert(fromV2.eveSwitch(Sw::TXT16) && fromV2.eveSwitch(Sw::TXTGREEN));
        // A field a version does not carry restores its power-on value, not
        // the live session's: v1 leaves the switches, CPREG and the RVB
        // Graph registers at power-on, v2 CPREG and the registers.
        {
            LeChatMauveCard live(7, LeChatMauveCard::Variant::Eve);
            live.loadSnapshotState(blob.data(), blob.size());   // CPREG $5A
            live.loadSnapshotState(v2, sizeof v2);
            assert(live.cpreg() == 0 && "v2 has no CPREG: power-on, not live");
            const uint8_t v1[7] = { 'C', 'M', 1, 0, 0, 0, 0 };
            live.loadSnapshotState(blob.data(), blob.size());
            live.loadSnapshotState(v1, sizeof v1);
            assert(live.eveSwitches() == 0 && live.cpreg() == 0);
            const LeChatMauveCard::RvbGraphState powerOn{};
            assert(live.rvbGraph().textColour == powerOn.textColour &&
                   live.rvbGraph().c[0] == powerOn.c[0] &&
                   live.rvbGraph().b[0] == powerOn.b[0]);
        }
        // On a Féline the switch byte is meaningless and stays clear.
        LeChatMauveCard fel(7, LeChatMauveCard::Variant::Feline);
        fel.loadSnapshotState(blob.data(), blob.size());
        assert(fel.eveSwitches() == 0);
    }

    // ─── 13. RVB Graph: the Manuel de Référence, chapters III-IV ──────────
    //
    // Every write below is a POKE the manual prints (pp. 9-16, checked on the
    // scan, not the OCR): the control register $C0F0 + S + U + K with data
    // 16·(15 − CT), and the colour registers $C70F/$C71F/$C72F/$C73F − Bn
    // with data 15 − Cn. Colours are the lo-res table (chapter II).
    {
        // The lo-res colours the manual's examples use (Apple2Display's
        // table is private; these are its entries).
        const uint32_t kMagenta = 0xFFD11AAAu;   //  3 "violet"
        const uint32_t kBlue    = 0xFFB58A00u;   //  6
        const uint32_t kOrange  = 0xFF4772FFu;   //  9
        const uint32_t kPink    = 0xFFCF7AFFu;   // 11 "rose"
        const uint32_t kGreen   = 0xFF2CE66Fu;   // 12 "vert clair"
        const uint32_t kYellow  = 0xFF7BF6FFu;   // 13
        const uint32_t kDkBlue  = 0xFF830700u;   //  2
        const uint32_t kBrown   = 0xFF005F7Au;   //  8
        const uint32_t kRed     = 0xFF4C12ACu;   //  1

        Memory mem;                       // a ][+-class machine: no IIe mode
        Apple2Display display;
        auto card = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::RvbGraph);
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

        // The manual's subroutine: open with K = 0, write the four
        // registers, close with K = 8 (p. 15, lines 1000-1500).
        auto program = [&](int s, int u, int ct, const int c[4], const int b[4]) {
            mem.memWrite(static_cast<uint16_t>(0xC0F0 + s + u), static_cast<uint8_t>(16 * (15 - ct)));
            for (int n = 0; n < 4; ++n)
                mem.memWrite(static_cast<uint16_t>(0xC70F + 0x10 * n - b[n]),
                             static_cast<uint8_t>(15 - c[n]));
            mem.memWrite(static_cast<uint16_t>(0xC0F8 + s + u), static_cast<uint8_t>(16 * (15 - ct)));
        };
        auto hgrDot = [&](int x) { display.render(mem); return *pixelAt(display, 2 * x, 0); };

        (void)mem.memRead(0xC050); (void)mem.memRead(0xC057); (void)mem.memRead(0xC054);
        (void)mem.memRead(0xC052);
        clearHgrLine(mem, 0);

        // Power-on = the usual display: "POKE −16144, 0" (p. 11). Isolated
        // dots take the usual colours by position, a run of two is white.
        assert(raw->rvbGraph().textColour == 15 && !raw->rvbGraph().useRegs);
        writeHgrByte(mem, 0, 0, 0x01);  assert(hgrDot(0) == kMagenta);   // even, bit 7 = 0
        writeHgrByte(mem, 0, 0, 0x02);  assert(hgrDot(1) == kGreen);     // odd,  bit 7 = 0
        writeHgrByte(mem, 0, 0, 0x81);  assert(hgrDot(0) == kBlue);      // even, bit 7 = 1
        writeHgrByte(mem, 0, 0, 0x82);  assert(hgrDot(1) == kOrange);    // odd,  bit 7 = 1
        writeHgrByte(mem, 0, 0, 0x03);
        assert(hgrDot(0) == kWhite && hgrDot(1) == kWhite && hgrDot(2) == kBlack);

        // A read latches nothing: the register takes a DATA byte.
        (void)mem.memRead(0xC0F5);
        assert(raw->rvbGraph().s == 0 && !raw->rvbGraph().useRegs);

        // Exemple 1 (p. 10): the usual colours, pink text.
        {
            const int c[4] = { 3, 12, 6, 9 }, b[4] = { 15, 15, 15, 15 };
            program(0, 4, 11, c, b);
            const auto& g = raw->rvbGraph();
            assert(g.useRegs && g.locked && g.s == 0 && g.textColour == 11);
            assert(g.c[0] == 3 && g.c[1] == 12 && g.c[2] == 6 && g.c[3] == 9);
            assert(g.b[0] == 15 && g.b[3] == 15);
        }
        writeHgrByte(mem, 0, 0, 0x01);  assert(hgrDot(0) == kMagenta);
        writeHgrByte(mem, 0, 0, 0x03);  assert(hgrDot(0) == kWhite);

        // Exemple 2 (p. 10): C0 = 13, the violet dots turn yellow.
        {
            const int c[4] = { 13, 12, 6, 9 }, b[4] = { 15, 15, 15, 15 };
            program(0, 4, 15, c, b);
        }
        writeHgrByte(mem, 0, 0, 0x01);  assert(hgrDot(0) == kYellow);

        // K = 8 (p. 13): the registers are closed — a stray $C700 write
        // changes nothing — and RESET leaves the card alone.
        mem.memWrite(0xC700, 0x00);
        assert(raw->rvbGraph().c[0] == 13 && raw->rvbGraph().b[0] == 15);
        raw->onReset();
        assert(raw->rvbGraph().useRegs && raw->rvbGraph().locked);

        // Eight colours (chapter IV § II, p. 15): a run takes Bn of its
        // LEFTMOST dot's position.
        {
            const int c[4] = { 3, 12, 6, 9 }, b[4] = { 1, 2, 8, 13 };
            program(0, 4, 15, c, b);
        }
        writeHgrByte(mem, 0, 0, 0x03);                       // x0-1: starts even, bit 7 = 0
        assert(hgrDot(0) == kRed && hgrDot(1) == kRed);      // B0
        writeHgrByte(mem, 0, 0, 0x06);                       // x1-2: starts odd
        assert(hgrDot(1) == kDkBlue && hgrDot(2) == kDkBlue);// B1
        writeHgrByte(mem, 0, 0, 0x83);  assert(hgrDot(0) == kBrown);   // B2
        writeHgrByte(mem, 0, 0, 0x86);  assert(hgrDot(1) == kYellow);  // B3

        // S (p. 13). An alternating line ($55: dots 0, 2, 4, 6) is a C0 line:
        // continuous with S = 0, dotted with S = 1.
        {
            const int c[4] = { 3, 12, 6, 9 }, b[4] = { 15, 15, 15, 15 };
            writeHgrByte(mem, 0, 0, 0x55);
            program(0, 4, 15, c, b);
            assert(hgrDot(0) == kMagenta && hgrDot(1) == kMagenta && hgrDot(2) == kMagenta);
            program(1, 4, 15, c, b);
            assert(hgrDot(0) == kMagenta && hgrDot(1) == kBlack && hgrDot(2) == kMagenta);
            // S = 2: "la couleur B n'apparaît plus" — a run of three is
            // C0 C1 C0 by position, and still continuous between lines.
            program(2, 4, 15, c, b);
            writeHgrByte(mem, 0, 0, 0x07);
            assert(hgrDot(0) == kMagenta && hgrDot(1) == kGreen && hgrDot(2) == kMagenta);
            // "C1 sur fond C0": one odd dot set in a C0 line.
            writeHgrByte(mem, 0, 0, 0x5D);              // 1 0 1 1 1 0 1
            assert(hgrDot(1) == kMagenta && hgrDot(3) == kGreen && hgrDot(5) == kMagenta);
            // S = 3: the same, dotted.
            program(3, 4, 15, c, b);
            writeHgrByte(mem, 0, 0, 0x55);
            assert(hgrDot(1) == kBlack);
        }

        // U = 0 ties the white of HGR to CT (p. 13); POKE −16135,0 (p. 14)
        // = $C0F9: usual colours, dotted, locked, white text.
        mem.memWrite(0xC0F0, static_cast<uint8_t>(16 * (15 - 12)));   // U = 0, CT = 12
        writeHgrByte(mem, 0, 0, 0x03);  assert(hgrDot(0) == kGreen);
        mem.memWrite(0xC0F9, 0x00);
        assert(raw->rvbGraph().s == 1 && !raw->rvbGraph().useRegs &&
               raw->rvbGraph().locked && raw->rvbGraph().textColour == 15);
        writeHgrByte(mem, 0, 0, 0x55);  assert(hgrDot(1) == kBlack);

        // The black-and-white monitor of p. 11: $C0F5, every register 15,
        // $C0FD. Every lit dot white, every 0 black.
        {
            const int c[4] = { 15, 15, 15, 15 }, b[4] = { 15, 15, 15, 15 };
            program(1, 4, 15, c, b);
            writeHgrByte(mem, 0, 0, 0x55);
            assert(hgrDot(0) == kWhite && hgrDot(1) == kBlack && hgrDot(2) == kWhite);
        }
        // …and the green one: everything 12, text included.
        {
            const int c[4] = { 12, 12, 12, 12 }, b[4] = { 12, 12, 12, 12 };
            program(1, 4, 12, c, b);
            assert(hgrDot(0) == kGreen);
        }

        // Text in CT (p. 10): an inverse space is a block of text colour.
        (void)mem.memRead(0xC051);
        mem.memWrite(loresAddr(0), 0x20);
        display.render(mem);
        assert(*pixelAt(display, 0, 0) == kGreen);
        {
            const int c[4] = { 3, 12, 6, 9 }, b[4] = { 15, 15, 15, 15 };
            program(0, 4, 11, c, b);
        }
        display.render(mem);
        assert(*pixelAt(display, 0, 0) == kPink);

        // Snapshot v5 carries the whole register file.
        std::vector<uint8_t> blob;
        raw->appendSnapshotState(blob);
        LeChatMauveCard copy(7, LeChatMauveCard::Variant::RvbGraph);
        copy.loadSnapshotState(blob.data(), blob.size());
        assert(copy.rvbGraph().textColour == 11 && copy.rvbGraph().useRegs &&
               copy.rvbGraph().locked && copy.rvbGraph().c[1] == 12 &&
               copy.rvbGraph().b[2] == 15);

        // K = 0: RESET is "POKE −16144, 0" — usual display, white text. The
        // registers keep their bytes.
        mem.memWrite(0xC0F4, static_cast<uint8_t>(16 * (15 - 11)));
        raw->onReset();
        assert(raw->rvbGraph().textColour == 15 && !raw->rvbGraph().useRegs &&
               raw->rvbGraph().s == 0 && raw->rvbGraph().c[0] == 3);
        display.render(mem);
        assert(*pixelAt(display, 0, 0) == kWhite);
    }

    // ─── 14. ColorNTSC fallback when card not plugged ─────────────────────
    {
        Memory mem;     // no card plugged
        Apple2Display display;
        // Picking ChatMauveRGB without a card should silently fall back to
        // NTSC. Render a known $7F row and verify it lights up white
        // (the existing NTSC LUT does that for popcount-rich windows).
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        (void)mem.memRead(0xC050);
        (void)mem.memRead(0xC057);
        (void)mem.memRead(0xC054);

        for (int col = 0; col < 40; ++col) writeHgrByte(mem, 0, col, 0x7F);
        display.render(mem);
        for (int x = 0; x < 280; ++x) assert(*pixelAt(display, x, 0) == kWhite);
    }

    std::printf("Le Chat Mauve smoke: OK (FIFO, LCM HGR, BW560, lo-res grays, bit7 invert, "
                "Eve $C0B0-$C0BF + CPREG auto-write + TXT16 + TXTGREEN + table IX-1, "
                "AN3-off per variant, RVB Graph registers, snapshot v5, NTSC fallback)\n");
    // ── The card drives nothing on $C0Fx / $C7xx: the floating bus stays ───
    {
        Memory mem;
        const uint8_t devBefore = mem.memRead(0xC0F5);
        const uint8_t romBefore = mem.memRead(0xC740);
        auto card = std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::Feline);
        card->setMemory(&mem);
        mem.slotBus().plug(7, std::move(card));
        assert(mem.memRead(0xC0F5) == devBefore &&
               "an address the card does not decode must read the floating bus");
        assert(mem.memRead(0xC740) == romBefore &&
               "no slot EPROM: $C700-$C7FF stays the floating bus");
        std::printf("  Chat Mauve leaves the floating bus alone OK\n");
    }

    return 0;
}
