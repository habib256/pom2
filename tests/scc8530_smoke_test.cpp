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

// Smoke test for Scc8530Device — the MAME port of `z80scc.{h,cpp}`.
//
// The cases are chosen to pin the surface the Apple II Workstation Card
// firmware actually touches (`docs/printer_plan_2.md` § 5.1), plus the
// chip behaviour that is easy to get subtly wrong in a register-file port:
//
//   1. Hardware-reset register values — MAME `z80scc.cpp:502` on top of
//      `z80scc.cpp:1123`.
//   2. The two-step register pointer, including Point High for WR8-WR15,
//      and the fact that the pointer self-clears after one access
//      (z80scc.cpp:1717 / 2334).
//   3. The card firmware's own interrupt-service idiom, read verbatim off
//      the 341-0358-A dump at $EE13: `LDA #$03 / STA $7502 / LDA $7502`
//      reads RR3, and `$10`/`$30` to WR0 are Reset External/Status and
//      Error Reset.
//   4. RR3 is channel A only; channel B returns zero (z80scc.cpp:1508).
//   5. Baud-rate generator arithmetic against the SCC user manual's
//      formula (z80scc.cpp:2789).
//   6. Local loopback: a transmitted byte reappears in the receive FIFO
//      after exactly one frame time, with the FIFO and RR0 bits the
//      manual describes.
//   7. Receive FIFO depth 3 (a 3 + 1 ring, MAME master z80scc.cpp:1062),
//      MAME's overrun handling, and the Error Reset step (z80scc.cpp:2566 /
//      2362 / 1811).
//   8. The interrupt block: MIE gating, IP bits in RR3, vector
//      modification into RR2, and the Reset Highest IUS command.
//   9. Zero Count external interrupt from the BRG timer (z80scc.cpp:1171).
//  10. CTS/DCD/SYNC, the External/Status latch and its release rule, and
//      Auto Enables (z80scc.cpp:2604 / 2641 / 2680 / 793).
//  11. The /RTS, /DTR and /W//REQ pin drivers, including WR14's DTR/REQ
//      repurposing (z80scc.cpp:1352 / 3015).
//  12. WR8 as the transmit buffer, and what a disabled transmitter does.
//  13. Both bus orderings; the ab_dc one is what the card wires at
//      $7500-$7503.
//  14. The interrupt-block fixes MAME master made after the 588eeb33 pin
//      (bug hunt 2026-09-29): per-channel reset of IP/IUS, WR1 withdrawing
//      a source's IP, MIE gating /INT and the acknowledge but not RR3,
//      Zero Count obeying WR1 D0, the special condition obeying the Rx
//      interrupt mode, and a v3 snapshot's 3-slot ring restoring.

#include "Scc8530Device.h"

#include <cassert>
#include <cstring>
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

using pom2::Scc8530Device;

namespace {

constexpr int A = Scc8530Device::CHAN_A;
constexpr int B = Scc8530Device::CHAN_B;

/// Write `value` into register `reg` of `ch` the way a 6502 driver does:
/// point the register pointer with one control write, then supply the data
/// with the next one. Registers 8-15 need the Point High command in D3.
void writeReg(Scc8530Device& scc, int ch, uint8_t reg, uint8_t value)
{
    if (reg < 8) {
        scc.controlWrite(ch, reg);
    } else {
        scc.controlWrite(ch, static_cast<uint8_t>(0x08 | (reg & 0x07)));
    }
    scc.controlWrite(ch, value);
}

uint8_t readReg(Scc8530Device& scc, int ch, uint8_t reg)
{
    if (reg < 8) {
        scc.controlWrite(ch, reg);
    } else {
        scc.controlWrite(ch, static_cast<uint8_t>(0x08 | (reg & 0x07)));
    }
    return scc.controlRead(ch);
}

/// The SCC user manual's time constant for a given bit rate:
/// TC = clock / (2 * rate * clockMode) - 2.
uint16_t timeConstant(uint32_t pclk, uint32_t rate, uint32_t clockMode)
{
    return static_cast<uint16_t>(pclk / (2 * rate * clockMode) - 2);
}

} // namespace

void testHardwareResetValues()
{
    Scc8530Device scc;

    // z80scc.cpp:1155 — RR0 &= 0xfc then |= 0x44, i.e. Tx Buffer Empty
    // (D2) plus Tx Underrun/EOM (D6).
    assert((scc.peekRr(A, 0) & 0x44) == 0x44);
    // RR1: ALL_SENT plus the two "required reset value" residue bits.
    assert((scc.peekRr(A, 1) & 0x07) == 0x07);
    // RR3 clear on both channels.
    assert(scc.peekRr(A, 3) == 0x00);
    assert(scc.peekRr(B, 3) == 0x00);
    // WR9 hardware reset: (wr9 & 0x3c) | 0xc0 — z80scc.cpp:508.
    assert(scc.peekWr(A, 9) == 0xC0);
    // WR11 = 0x08 and WR14 = 0x30 are hardware-only values that a channel
    // reset does not produce — z80scc.cpp:509-515.
    assert(scc.peekWr(A, 11) == 0x08);
    assert(scc.peekWr(B, 11) == 0x08);
    assert(scc.peekWr(A, 14) == 0x30);
    assert(scc.peekWr(A, 15) == 0xF8);
    assert(!scc.intAsserted());
    std::printf("  ok: hardware reset values match MAME z80scc.cpp:502 + 1123\n");
}

void testRegisterPointerProtocol()
{
    Scc8530Device scc;

    // A control write of a bare register number only moves the pointer.
    scc.controlWrite(A, 4);
    scc.controlWrite(A, 0x44);              // WR4: x16 clock, 1 stop bit
    assert(scc.peekWr(A, 4) == 0x44);

    // The pointer self-clears: the next control write is WR0 again, not WR4.
    scc.controlWrite(A, 0x00);
    assert(scc.peekWr(A, 4) == 0x44);

    // Point High reaches WR8-WR15. WR12 = 0x0A via $08|0x04.
    scc.controlWrite(A, 0x0C);
    scc.controlWrite(A, 0x0A);
    assert(scc.peekWr(A, 12) == 0x0A);

    // ...and a read through the pointer comes back from the right register:
    // RR12 is an image of WR12 (z80scc.cpp:1611).
    assert(readReg(scc, A, 12) == 0x0A);

    // Register 0 needs no pointer write at all.
    const uint8_t rr0 = scc.controlRead(A);
    assert((rr0 & 0x04) != 0);              // Tx Buffer Empty
    std::printf("  ok: register pointer + Point High (z80scc.cpp:1717/2334)\n");
}

void testFirmwareIsrIdiom()
{
    // The 341-0358-A dump's IRQ handler at $EE13 does exactly this:
    //   LDA #$03 / STA $7502 / NOP / NOP / LDA $7502   -> read RR3
    //   LDA #$10 / STA $7500 / LDA #$30 / STA $7500    -> WR0 commands
    // $7502 is channel A control and $7500 channel B control under the
    // card's ab_dc wiring.
    Scc8530Device scc;

    // Arm something so RR3 is non-zero: MIE + Rx interrupt on all characters.
    writeReg(scc, A, 9, 0x08);              // WR9 MIE, no reset command
    writeReg(scc, A, 3, 0xC1);              // WR3 Rx enable, 8 bits
    writeReg(scc, A, 1, 0x10);              // WR1 Rx int on all characters
    scc.receiveByte(A, 0x55);

    // RR3 bit 5 = channel A Rx IP.
    scc.writeAbDc(2, 0x03);                 // STA $7502 with #$03
    const uint8_t rr3 = scc.readAbDc(2);    // LDA $7502
    assert((rr3 & 0x20) != 0);
    assert(scc.intAsserted());

    // Channel B's control port answers 0 for RR3 — the register only
    // exists in channel A.
    scc.writeAbDc(0, 0x03);
    assert(scc.readAbDc(0) == 0x00);

    // Drain the character; the Rx IP bit goes away with the FIFO.
    assert(scc.readAbDc(3) == 0x55);        // LDA $7503 = channel A data
    scc.writeAbDc(2, 0x03);
    assert((scc.readAbDc(2) & 0x20) == 0);
    assert(!scc.intAsserted());
    std::printf("  ok: card firmware ISR idiom (RR3 poll + channel-A-only)\n");
}

void testBaudRateGenerator()
{
    Scc8530Device scc;
    scc.setPclk(Scc8530Device::kDefaultPclk);   // 3.6864 MHz

    const uint16_t tc = timeConstant(Scc8530Device::kDefaultPclk, 9600, 16);
    assert(tc == 10);

    writeReg(scc, A, 4, 0x44);              // WR4: x16 clock, 1 stop bit
    writeReg(scc, A, 11, 0x50);                         // Rx + Tx clock from BRG
    writeReg(scc, A, 12, static_cast<uint8_t>(tc & 0xFF));
    writeReg(scc, A, 13, static_cast<uint8_t>(tc >> 8));
    writeReg(scc, A, 14, 0x03);                         // BRG source PCLK, enable

    assert(scc.txRate(A) == 9600);
    assert(scc.rxRate(A) == 9600);

    // Disabling the BRG stops both clocks (z80scc.cpp:2288).
    writeReg(scc, A, 14, 0x02);
    assert(scc.txRate(A) == 0);
    std::printf("  ok: BRG time constant 10 -> 9600 baud at 3.6864 MHz x16\n");
}

void testLoopbackTiming()
{
    Scc8530Device scc;
    scc.setPclk(Scc8530Device::kDefaultPclk);

    const uint16_t tc = timeConstant(Scc8530Device::kDefaultPclk, 9600, 16);
    writeReg(scc, A, 4, 0x44);      // x16, 1 stop bit, no parity
    writeReg(scc, A, 3, 0xC1);      // Rx enable, 8 bits
    writeReg(scc, A, 11, 0x50);     // Rx + Tx clock from BRG
    writeReg(scc, A, 12, static_cast<uint8_t>(tc & 0xFF));
    writeReg(scc, A, 13, static_cast<uint8_t>(tc >> 8));
    writeReg(scc, A, 14, 0x13);     // local loopback + BRG source PCLK + enable
    writeReg(scc, A, 5, 0x68);      // Tx enable, 8 bits

    std::vector<uint8_t> sent;
    scc.setTxCallback([&](int ch, uint8_t d) { assert(ch == A); sent.push_back(d); });

    scc.dataWrite(A, 0x41);
    assert(scc.txBusy(A));
    // The one-slot transmit buffer emptied into the shift register, so TBE
    // is set again straight away (z80scc.cpp:2519).
    assert((scc.peekRr(A, 0) & 0x04) != 0);
    assert((scc.peekRr(A, 1) & 0x01) == 0);     // ALL SENT cleared

    // A 10-bit frame at 9600 baud is 1041.6 us; one PCLK tick short of it
    // must not complete.
    const uint64_t frameCycles = (10ull * Scc8530Device::kDefaultPclk) / 9600ull;
    scc.tick(frameCycles - 1);
    assert(sent.empty());
    assert(scc.txBusy(A));

    scc.tick(2);
    assert(sent.size() == 1 && sent[0] == 0x41);
    assert(!scc.txBusy(A));
    assert((scc.peekRr(A, 1) & 0x01) != 0);     // ALL SENT

    // Loopback fed it straight back into the receiver.
    assert(scc.rxFifoCount(A) == 1);
    assert((scc.peekRr(A, 0) & 0x01) != 0);     // Rx Character Available
    assert(scc.dataRead(A) == 0x41);
    assert(scc.rxFifoCount(A) == 0);
    assert((scc.peekRr(A, 0) & 0x01) == 0);
    std::printf("  ok: local loopback delivers one byte per frame time\n");
}

void testReceiveFifoOverrun()
{
    Scc8530Device scc;
    writeReg(scc, A, 3, 0xC1);      // Rx enable

    // The NMOS 8530 has a 3-byte receive FIFO. MAME's ring keeps one slot
    // as the full/empty discriminator, so master sizes it 3 + 1
    // (z80scc.cpp:1060-1062): three bytes fit and the FOURTH overruns. The
    // 588eeb33 pin sized the ring 3 and overran on the chip's third byte.
    scc.receiveByte(A, 0x10);
    scc.receiveByte(A, 0x20);
    scc.receiveByte(A, 0x30);
    assert(scc.rxFifoCount(A) == 3);
    assert((scc.peekRr(A, 0) & 0x01) != 0);     // Rx Character Available

    scc.receiveByte(A, 0x40);
    assert(scc.rxFifoCount(A) == 3);            // dropped, not queued

    // The overrunning character overwrites the NEWEST entry, which carries
    // the Overrun flag: "only the character that has been written over is
    // flagged" (Zilog SCC/ESCC User Manual, RR1 D5; MAME master writes the
    // newest slot too). POM2 used to park it in the ring's unused sentinel,
    // where the next write cleared it — an overrun was never reported.
    assert(scc.dataRead(A) == 0x10);
    assert(scc.dataRead(A) == 0x20);
    // RR1 describes the character at the EXIT, before it is read (manual
    // p. 47) — so the flag shows here, ahead of the flagged read.
    assert((scc.peekRr(A, 1) & 0x20) != 0);
    assert(scc.dataRead(A) == 0x40);
    assert((scc.peekRr(A, 1) & 0x20) != 0 && "the overrun reached RR1");
    assert(scc.rxFifoCount(A) == 0);
    assert((scc.peekRr(A, 0) & 0x01) == 0);
    std::printf("  ok: 3-deep Rx FIFO; an overrun flags the overwritten character\n");
}

void testErrorResetStepsFifo()
{
    // WR0 Error Reset releases only a character the FIFO is HOLDING — one
    // with a special condition, in "Rx Int on First Character" / "Special
    // Condition only" (Zilog manual, Error Reset and WR1 D4-D3). It used to
    // step the FIFO whenever it was non-empty, throwing away a good,
    // unread character.
    Scc8530Device scc;
    writeReg(scc, A, 3, 0xC1);
    scc.receiveByte(A, 0x11);
    scc.receiveByte(A, 0x22);
    assert(scc.rxFifoCount(A) == 2);
    scc.controlWrite(A, 0x30);      // WR0 Error Reset — nothing held
    assert(scc.rxFifoCount(A) == 2 && "Error Reset dropped a good character");
    assert(scc.dataRead(A) == 0x11 && scc.dataRead(A) == 0x22);

    // Special Condition only (WR1 D4-D3 = 11): an overrun character is held
    // after its read, until Error Reset.
    writeReg(scc, A, 1, 0x18);
    for (uint8_t b : { 0x31, 0x32, 0x33, 0x34 }) scc.receiveByte(A, b);
    assert(scc.dataRead(A) == 0x31 && scc.dataRead(A) == 0x32);
    assert(scc.dataRead(A) == 0x34);            // the flagged one
    assert(scc.rxFifoCount(A) == 1 && "mode 11 holds the special character");
    scc.controlWrite(A, 0x30);
    assert(scc.rxFifoCount(A) == 0);
    assert((scc.peekRr(A, 1) & 0x20) == 0 && "Error Reset clears RR1's error bits");
    std::printf("  ok: WR0 Error Reset releases only a held character\n");
}

void testInterruptVectorAndIus()
{
    Scc8530Device scc;

    bool line = false;
    scc.setIntCallback([&](bool s) { line = s; });

    writeReg(scc, A, 2, 0x30);      // WR2 interrupt vector base
    writeReg(scc, A, 9, 0x09);      // WR9 MIE + VIS (vector includes status)
    writeReg(scc, A, 3, 0xC1);      // Rx enable
    writeReg(scc, A, 1, 0x10);      // Rx interrupt on all characters

    assert(!line);
    scc.receiveByte(A, 0x7E);
    assert(line);
    assert(scc.intAsserted());

    // Channel A Receive Character Available is status 110 -> V3-V1 = 6,
    // so the modified vector is 0x30 | (6 << 1) = 0x3C. RR2 read from
    // channel B carries the modification (z80scc.cpp:1465).
    assert(scc.peekRr(B, 2) == 0x3C);
    assert(scc.intAck() == 0x3C);

    // The ack set IUS, which blocks lower-priority requests but leaves
    // /INT asserted for this one.
    assert(scc.peekRr(A, 3) == 0x20);

    // Reset Highest IUS is the last act of the service routine.
    scc.controlWrite(A, 0x38);
    assert(scc.intAsserted());      // the character is still in the FIFO
    assert(scc.dataRead(A) == 0x7E);
    assert(!scc.intAsserted());
    assert(!line);

    // With MIE clear nothing may interrupt at all — but the IP bit still
    // lands in RR3 for a polling driver (MAME master z80scc.cpp:750-754).
    writeReg(scc, A, 9, 0x01);
    scc.receiveByte(A, 0x22);
    assert(!scc.intAsserted());
    assert(!line);
    assert(scc.peekRr(A, 3) == 0x20);
    std::printf("  ok: vector modification, IUS, MIE gate\n");
}

void testZeroCountInterrupt()
{
    Scc8530Device scc;
    scc.setPclk(Scc8530Device::kDefaultPclk);

    int extInts = 0;
    scc.setIntCallback([&](bool s) { if (s) extInts++; });

    writeReg(scc, A, 9, 0x08);      // MIE
    writeReg(scc, A, 1, 0x01);      // external/status interrupt enable
    writeReg(scc, A, 4, 0x44);      // x16
    writeReg(scc, A, 12, 0x0A);
    writeReg(scc, A, 13, 0x00);
    writeReg(scc, A, 15, 0xFA);     // WR15 with Zero Count armed
    writeReg(scc, A, 14, 0x03);     // BRG source PCLK + enable

    // The Zero Count timer runs at the undivided BRG output:
    // 3686400 / (2 + 10) = 307200 Hz. One tick of PCLK/307200 cycles.
    const uint64_t period = Scc8530Device::kDefaultPclk / 307200ull;
    assert(period == 12);
    scc.tick(period - 1);
    assert(extInts == 0);
    scc.tick(1);
    assert(extInts == 1);
    assert((scc.peekRr(A, 3) & 0x08) != 0);     // channel A Ext/Status IP
    std::printf("  ok: Zero Count fires at the undivided BRG rate\n");
}

void testModemPinsAndExtStatusLatch()
{
    Scc8530Device scc;
    writeReg(scc, A, 9, 0x08);      // MIE
    writeReg(scc, A, 1, 0x01);      // external/status interrupts enabled
    writeReg(scc, A, 15, 0xF8);     // DCD, SYNC, CTS, Tx EOM, Break/Abort armed

    // /DCD is active low: dcdW(false) asserts it and sets RR0 D3.
    assert((scc.peekRr(A, 0) & 0x08) == 0);
    scc.dcdW(A, false);
    assert((scc.peekRr(A, 0) & 0x08) != 0);
    assert(scc.intAsserted());
    assert((scc.peekRr(A, 3) & 0x08) != 0);     // channel A Ext/Status IP

    // While the latch is up, RR0 reports the LATCHED state, not the pin:
    // a second change is captured but not shown until the latch is released
    // (z80scc.cpp:1434).
    scc.ctsW(A, false);
    assert((scc.peekRr(A, 0) & 0x20) == 0);     // CTS change hidden by the latch

    // Reset External/Status Interrupt only releases the latch when nothing
    // else has changed meanwhile. CTS moved while the latch was up, so
    // `update_extint` reports work outstanding and the latch STAYS
    // (z80scc.cpp:1178-1188) — but it also advances the latched copy of RR0 to
    // the current pin state ("Update latched value to match current status",
    // z80scc.cpp:1189-1197), which is what makes the CTS change visible and
    // the NEXT reset able to finish.
    scc.controlWrite(A, 0x10);
    assert((scc.peekRr(A, 0) & 0x20) != 0);     // the CTS pin shows through now
    assert((scc.peekRr(A, 3) & 0x08) != 0);     // …but the IP bit is still up
    assert(scc.intAsserted());

    // …and the SECOND Reset Ext/Status DOES release it. That advance is the
    // half POM2 was missing: without the else branch `extint_states` stayed at
    // the pre-change value for ever, so the difference never went away, the
    // Ext/Status IP bit never cleared, and an ISR written as "Reset
    // Ext/Status, re-read RR0, are we done?" — the Workstation Card ROM's, at
    // $EE13 — never left the loop.
    scc.controlWrite(A, 0x10);
    assert((scc.peekRr(A, 3) & 0x08) == 0);     // Ext/Status IP released
    assert(!scc.intAsserted());

    // With a single condition, the same command does release it and clears
    // the Ext/Status IP bit in RR3.
    Scc8530Device scc1;
    writeReg(scc1, A, 9, 0x08);
    writeReg(scc1, A, 1, 0x01);
    writeReg(scc1, A, 15, 0xF8);
    scc1.dcdW(A, false);
    assert(scc1.intAsserted());
    assert((scc1.peekRr(A, 3) & 0x08) != 0);
    scc1.controlWrite(A, 0x10);
    assert((scc1.peekRr(A, 3) & 0x08) == 0);
    assert(!scc1.intAsserted());

    // Auto Enables: /DCD low turns the receiver on, /CTS low the transmitter.
    Scc8530Device scc2;
    writeReg(scc2, A, 3, 0x20);     // WR3 Auto Enables, Rx disabled
    assert((scc2.peekWr(A, 3) & 0x01) == 0);
    scc2.dcdW(A, false);
    assert((scc2.peekWr(A, 3) & 0x01) != 0);    // Rx enabled by DCD
    scc2.ctsW(A, false);
    assert((scc2.peekWr(A, 5) & 0x08) != 0);    // Tx enabled by CTS

    // /SYNC drives RR0 D4 unless WR11 D7 has claimed the pin for the crystal.
    Scc8530Device scc3;
    scc3.syncW(A, false);
    assert((scc3.peekRr(A, 0) & 0x10) != 0);
    writeReg(scc3, B, 11, 0x80);    // crystal on /RTxC-/SYNC for channel B
    scc3.syncW(B, false);
    assert((scc3.peekRr(B, 0) & 0x10) == 0);
    std::printf("  ok: CTS/DCD/SYNC pins, Ext/Status latch, Auto Enables\n");
}

void testRtsDtrAndWreqPins()
{
    Scc8530Device scc;
    std::vector<std::pair<int, bool>> rts, dtr, wreq;
    scc.setRtsCallback([&](int c, bool st) { rts.emplace_back(c, st); });
    scc.setDtrCallback([&](int c, bool st) { dtr.emplace_back(c, st); });
    scc.setWreqCallback([&](int c, bool st) { wreq.emplace_back(c, st); });

    // WR5 D1 sets /RTS low, D7 sets /DTR low — both inverted on the pin.
    writeReg(scc, A, 5, 0x82);
    assert(!rts.empty() && rts.back().first == A && rts.back().second == false);
    assert(!dtr.empty() && dtr.back().second == false);

    writeReg(scc, A, 5, 0x00);
    assert(rts.back().second == true);
    assert(dtr.back().second == true);

    // WR14 D2 repurposes /DTR as the DMA request pin, which then follows
    // Tx Buffer Empty instead of WR5 D7 (z80scc.cpp:3015).
    writeReg(scc, A, 14, 0x34);
    dtr.clear();
    writeReg(scc, A, 1, 0x00);
    assert(!dtr.empty() && dtr.back().second == false);   // TBE set -> /DTR low

    // WR1 D7|D6 arm /W//REQ as a request line on the transmitter.
    writeReg(scc, A, 5, 0x08);      // Tx enable
    wreq.clear();
    writeReg(scc, A, 1, 0xC0);
    assert(!wreq.empty() && wreq.back().second == false);
    std::printf("  ok: /RTS, /DTR and /W//REQ pin drivers\n");
}

void testWr8IsTheTransmitBuffer()
{
    // WR8 is the transmit buffer: writing it through the register pointer
    // is the same as writing the data port (z80scc.cpp:2007).
    Scc8530Device scc;
    scc.setPclk(Scc8530Device::kDefaultPclk);
    const uint16_t tc = timeConstant(Scc8530Device::kDefaultPclk, 9600, 16);
    writeReg(scc, A, 4, 0x44);
    writeReg(scc, A, 3, 0xC1);
    writeReg(scc, A, 11, 0x50);
    writeReg(scc, A, 12, static_cast<uint8_t>(tc & 0xFF));
    writeReg(scc, A, 13, 0x00);
    writeReg(scc, A, 14, 0x03);
    writeReg(scc, A, 5, 0x68);

    std::vector<uint8_t> sent;
    scc.setTxCallback([&](int, uint8_t d) { sent.push_back(d); });

    writeReg(scc, A, 8, 0x5A);
    assert(scc.txBusy(A));
    scc.tick(10ull * Scc8530Device::kDefaultPclk / 9600ull);
    assert(sent.size() == 1 && sent[0] == 0x5A);

    // A byte written with the transmitter disabled sits in the buffer and
    // clears TBE; nothing leaves the chip.
    writeReg(scc, A, 5, 0x60);      // Tx disable
    sent.clear();
    scc.dataWrite(A, 0x11);
    assert((scc.peekRr(A, 0) & 0x04) == 0);     // TBE clear: buffer full
    scc.tick(100000);
    assert(sent.empty());
    std::printf("  ok: WR8 is the transmit buffer; a disabled Tx holds it\n");
}

void testBusOrderings()
{
    // ab_dc (Workstation Card): 0 = B ctl, 1 = B data, 2 = A ctl, 3 = A data.
    Scc8530Device scc;
    writeReg(scc, A, 3, 0xC1);
    writeReg(scc, B, 3, 0xC1);
    scc.receiveByte(A, 0xAA);
    scc.receiveByte(B, 0xBB);
    assert(scc.readAbDc(3) == 0xAA);
    assert(scc.readAbDc(1) == 0xBB);

    // dc_ab: 0 = B ctl, 1 = A ctl, 2 = B data, 3 = A data.
    Scc8530Device scc2;
    writeReg(scc2, A, 3, 0xC1);
    writeReg(scc2, B, 3, 0xC1);
    scc2.receiveByte(A, 0xAA);
    scc2.receiveByte(B, 0xBB);
    assert(scc2.readDcAb(3) == 0xAA);
    assert(scc2.readDcAb(2) == 0xBB);
    std::printf("  ok: ab_dc and dc_ab pin orderings (z80scc.cpp:903/946)\n");
}

void testWr9Resets()
{
    Scc8530Device scc;
    writeReg(scc, A, 4, 0x44);
    writeReg(scc, B, 4, 0x44);

    // Channel B reset leaves channel A alone (z80scc.cpp:2024). WR4 on a
    // reset is XXXXX1XX — only D2 is forced (Zilog manual, "Register reset
    // value" table, p. 35); POM2 used to write $04 over the whole register.
    writeReg(scc, A, 4, 0x40);
    writeReg(scc, B, 4, 0x40);
    writeReg(scc, A, 9, 0x40);
    assert(scc.peekWr(B, 4) == 0x44);   // D2 forced, the rest kept
    assert(scc.peekWr(A, 4) == 0x40);   // channel A untouched

    // Force Hardware Reset restores the device-wide values and takes the
    // MIE / Status-High / DLC bits from the command byte itself; NV and VIS
    // (D1/D0) are KEPT — WR9 resets to 110000xx — so $C9 keeps VIS.
    writeReg(scc, A, 4, 0x40);
    writeReg(scc, A, 9, 0xC9);          // hardware reset + MIE + VIS
    assert(scc.peekWr(A, 4) == 0x44);
    assert(scc.peekWr(A, 11) == 0x08);
    assert((scc.peekWr(A, 9) & 0x08) != 0);
    assert((scc.peekWr(A, 9) & 0x01) != 0 && "a hardware reset cleared VIS");
    std::printf("  ok: WR9 channel and hardware reset commands\n");
}

void testSdlcFraming()
{
    // SDLC is the one part of this device that is NOT a MAME port — MAME
    // logs "SDLC - not implemented" — so it gets pinned harder, from the
    // Zilog manual's own description of how a frame opens and closes.
    Scc8530Device scc;
    scc.setPclk(Scc8530Device::kDefaultPclk);
    scc.setRtxc(A, Scc8530Device::kDefaultPclk);

    // What the Workstation Card's firmware actually programs: WR4 = $20 is
    // SDLC with the x1 clock, WR12/13 = 6 gives 230400 from a 3.6864 MHz
    // crystal, WR11 = $F2 takes the receive clock from the DPLL.
    writeReg(scc, A, 4, 0x20);
    assert(scc.sdlcMode(A));
    assert(!scc.sdlcMode(B));
    writeReg(scc, A, 11, 0x52);     // Rx from BRG (the DPLL is not modelled)
    writeReg(scc, A, 12, 0x06);
    writeReg(scc, A, 13, 0x00);
    writeReg(scc, A, 14, 0x01);     // BRG on, source /RTxC
    assert(scc.txRate(A) == 230400);
    writeReg(scc, A, 3, 0xC9);      // Rx enable, 8 bits, Rx CRC enable
    writeReg(scc, A, 5, 0x69);      // Tx enable, 8 bits, Tx CRC enable

    std::vector<std::vector<uint8_t>> frames;
    scc.setFrameCallback([&](int ch, const std::vector<uint8_t>& f) {
        assert(ch == A); frames.push_back(f);
    });
    // Bytes must NOT come out one at a time in a framed mode.
    scc.setTxCallback([](int, uint8_t) { assert(false && "SDLC emits frames"); });

    // A byte on an SDLC line is exactly 8 bit times at 230400 with the x1
    // clock — no start bit and no stop bit, unlike the async path.
    const uint64_t byteCycles = (8ull * Scc8530Device::kDefaultPclk) / 230400ull;
    scc.dataWrite(A, 0x00);
    scc.tick(byteCycles - 1);
    assert(scc.txBusy(A) && "one tick short of a byte time");
    scc.tick(1);
    assert(!scc.txBusy(A) && "an SDLC byte is 8 bit times exactly");
    scc.controlWrite(A, 0x18);      // abort the probe frame
    assert(scc.txFrameSize(A) == 0);

    // A driver loads the first byte, then clears the Tx Underrun/EOM latch
    // so the underrun at the END of the frame means End Of Message, then
    // feeds the rest one byte per Tx Buffer Empty.
    scc.dataWrite(A, 0xAA);
    scc.controlWrite(A, 0xC0);      // Reset Tx Underrun/EOM — arms the close
    assert((scc.peekRr(A, 0) & 0x40) == 0);

    for (uint8_t b : { 0xBB, 0xCC }) {
        // The one-slot buffer has to drain first; writing early is discarded
        // by the chip, which is exactly what a real driver must avoid.
        while (!(scc.peekRr(A, 0) & 0x04)) scc.tick(1);
        scc.dataWrite(A, b);
    }
    scc.tick(byteCycles * 4);       // let it underrun

    assert(frames.size() == 1);
    assert(frames[0] == (std::vector<uint8_t>{0xAA, 0xBB, 0xCC}));
    assert((scc.peekRr(A, 0) & 0x40) != 0 && "Tx Underrun/EOM should be set");
    assert(scc.txFrameSize(A) == 0);
    std::printf("  ok: SDLC frame closes on the armed transmit underrun\n");

    // Send Abort destroys the frame in flight rather than delivering it.
    // The abort has to land while a byte is still in the shift register:
    // once the transmitter underruns with the latch armed, the frame is
    // already gone — which is itself the behaviour a one-byte frame relies
    // on.
    frames.clear();
    scc.controlWrite(A, 0xC0);
    scc.dataWrite(A, 0x11);
    scc.dataWrite(A, 0x22);         // the one-slot buffer, filled behind it
    scc.tick(byteCycles);           // $11 shifted out, $22 now in flight
    assert(scc.txFrameSize(A) == 1);
    assert(scc.txBusy(A));
    scc.controlWrite(A, 0x18);      // WR0 Send Abort
    assert(scc.txFrameSize(A) == 0);
    assert(!scc.txBusy(A));
    scc.tick(byteCycles * 4);
    assert(frames.empty());
    std::printf("  ok: Send Abort destroys the frame in flight\n");

    // A driver that never lets the transmitter underrun must not grow POM2's
    // heap without bound. The chip buffers nothing — its bytes are already on
    // the wire — but this seam delivers a frame whole, so the buffer needs a
    // ceiling, and the ceiling is past any legal LocalTalk frame (3-byte LLAP
    // header + 600 data bytes). Reaching it aborts the frame the way WR0's
    // Send Abort does, rather than delivering a frame nothing could have sent.
    frames.clear();
    scc.controlWrite(A, 0xC0);          // arm the close, then never allow it
    for (std::size_t i = 0; i < Scc8530Device::kMaxTxFrameBytes + 64; ++i) {
        while (!(scc.peekRr(A, 0) & 0x04)) scc.tick(1);
        scc.dataWrite(A, static_cast<uint8_t>(i & 0xFF));
        assert(scc.txFrameSize(A) <= Scc8530Device::kMaxTxFrameBytes);
    }
    for (const auto& f : frames)
        assert(f.size() <= Scc8530Device::kMaxTxFrameBytes);
    assert((scc.peekRr(A, 0) & 0x40) != 0 &&
           "the abort sets Tx Underrun/EOM, as Send Abort does");
    // Clean up before the next case: drain the byte still in the shift
    // register (and the one behind it) so the transmit buffer is free again,
    // then abort whatever frame those bytes opened.
    scc.tick(byteCycles * 4);
    scc.controlWrite(A, 0x18);
    assert(scc.txFrameSize(A) == 0);
    std::printf("  ok: an unending SDLC frame is capped, not grown for ever\n");

    // And a one-byte frame is legal: the underrun that closes it arrives
    // the moment the byte leaves the shift register.
    frames.clear();
    scc.controlWrite(A, 0xC0);
    scc.dataWrite(A, 0x5A);
    scc.tick(byteCycles * 2);
    assert(frames.size() == 1 && frames[0] == (std::vector<uint8_t>{0x5A}));
    std::printf("  ok: a one-byte SDLC frame closes on its own underrun\n");
}

void testSdlcReceive()
{
    Scc8530Device scc;
    writeReg(scc, A, 9, 0x08);      // MIE
    writeReg(scc, A, 4, 0x20);      // SDLC
    // Enter Hunt (WR3 D4) is how a driver arms the receiver for the next
    // opening flag — the Workstation Card's firmware writes WR3 = $DD, which
    // carries exactly that bit.
    writeReg(scc, A, 3, 0xD1);      // Rx enable, 8 bits, enter hunt
    writeReg(scc, A, 1, 0x10);      // Rx interrupt on all characters
    assert((scc.peekRr(A, 0) & 0x10) != 0 && "Enter Hunt sets Sync/Hunt");

    const uint8_t frame[] = { 0x21, 0x01, 0x81 };
    scc.receiveFrame(A, frame, 2);          // a two-byte frame
    assert((scc.peekRr(A, 0) & 0x10) == 0 && "the opening flag clears hunt");
    // Data, then the two FCS bytes (Zilog manual pp. 123-124): four bytes
    // through a 3-deep FIFO, the last one waiting for room.
    assert(scc.rxFifoCount(A) == 3);

    assert(scc.dataRead(A) == 0x21);
    assert((scc.peekRr(A, 1) & 0x80) == 0 && "not the end yet");
    assert(scc.dataRead(A) == 0x01);
    const uint8_t fcs1 = scc.dataRead(A);
    // RR1 describes the EXIT character before it is read: the second FCS
    // byte, with End Of Frame and the no-residue code.
    assert((scc.peekRr(A, 1) & 0x80) != 0 && "End Of Frame, ahead of the read");
    assert((scc.peekRr(A, 1) & 0x0E) == 0x06 && "residue: no residue, 8 bits");
    const uint8_t fcs2 = scc.dataRead(A);
    {
        // A valid FCS: the CRC over data + FCS is the CCITT residue $F0B8.
        uint16_t crc = 0xFFFF;
        for (uint8_t b : { uint8_t(0x21), uint8_t(0x01), fcs1, fcs2 }) {
            crc ^= b;
            for (int k = 0; k < 8; ++k) crc = uint16_t((crc >> 1) ^ ((crc & 1) ? 0x8408 : 0));
        }
        assert(crc == 0xF0B8 && "the FCS bytes are the line's CRC-CCITT");
    }
    // "Rx Int on All Characters" (WR1 D4-D3 = 10) does not HOLD a special
    // condition (Zilog manual, WR1 / Error Reset); RR1 keeps End Of Frame
    // until Error Reset.
    assert(scc.rxFifoCount(A) == 0 && "mode 10 does not hold the character");
    assert((scc.peekRr(A, 1) & 0x80) != 0);
    scc.controlWrite(A, 0x30);              // WR0 Error Reset
    assert((scc.peekRr(A, 1) & 0x80) == 0);

    // "Special Condition only" (11) HOLDS it until Error Reset.
    writeReg(scc, A, 1, 0x18);
    scc.receiveFrame(A, frame, 2);
    for (int k = 0; k < 4; ++k) (void)scc.dataRead(A);
    assert(scc.rxFifoCount(A) == 1 && "mode 11 holds the special character");
    scc.controlWrite(A, 0x30);
    assert(scc.rxFifoCount(A) == 0);
    std::printf("  ok: SDLC frames carry their FCS; End Of Frame is held in 01/11 only\n");

    // A frame received with a bad FCS raises the CRC error bit — on the
    // frame's last character, the second FCS byte.
    Scc8530Device bad;
    writeReg(bad, A, 4, 0x20);
    writeReg(bad, A, 3, 0xC1);
    const uint8_t one[] = { 0x55 };
    bad.receiveFrame(A, one, 1, /*crcError=*/true);
    assert(bad.dataRead(A) == 0x55);
    assert((bad.peekRr(A, 1) & 0x40) == 0 && "the error is on the frame's END");
    (void)bad.dataRead(A);
    assert((bad.peekRr(A, 1) & 0x40) != 0 && "CRC/framing error");
    std::printf("  ok: a bad FCS reaches RR1\n");

    // Address search: only WR6 or the $FF broadcast opens the receiver.
    Scc8530Device addr;
    writeReg(addr, A, 4, 0x20);
    writeReg(addr, A, 6, 0x42);             // this node's SDLC address
    writeReg(addr, A, 3, 0xD5);             // Rx enable + address search + hunt
    const uint8_t other[]  = { 0x99, 0x01 };
    const uint8_t mine[]   = { 0x42, 0x02 };
    const uint8_t bcast[]  = { 0xFF, 0x03 };
    addr.receiveFrame(A, other, 2);
    assert(addr.rxFifoCount(A) == 0 && "a frame for another node is ignored");
    assert((addr.peekRr(A, 0) & 0x10) != 0 && "and it stays in hunt");
    addr.receiveFrame(A, mine, 2);
    assert(addr.rxFifoCount(A) == 3);       // 2 data + FCS, 3-deep
    assert(addr.dataRead(A) == 0x42);
    for (int k = 0; k < 3; ++k) (void)addr.dataRead(A);   // the rest + FCS
    addr.controlWrite(A, 0x30);
    addr.receiveFrame(A, bcast, 2);
    assert(addr.rxFifoCount(A) == 3);
    std::printf("  ok: SDLC address search accepts WR6 and the broadcast\n");

    // Outside SDLC, and with the receiver off, frames are not accepted.
    Scc8530Device async;
    writeReg(async, A, 4, 0x44);            // async
    writeReg(async, A, 3, 0xC1);
    async.receiveFrame(A, one, 1);
    assert(async.rxFifoCount(A) == 0);
    Scc8530Device off;
    writeReg(off, A, 4, 0x20);
    off.receiveFrame(A, one, 1);
    assert(off.rxFifoCount(A) == 0);
    std::printf("  ok: frames need SDLC and an enabled receiver\n");
}

// Bug hunt #8: one bad-FCS frame poisoned a receive-FIFO slot for the rest
// of the session. `receiveFrame` OR'd RR1_CRC_FRAMING_ERROR into the slot
// and nothing ever cleared it — not the next character, not Error Reset —
// so on the Workstation Card's 3-deep FIFO every third clean byte after one
// corrupt LocalTalk frame reported a CRC error and re-locked the FIFO.
void testSdlcCrcErrorDoesNotPoisonTheSlot()
{
    Scc8530Device scc;
    writeReg(scc, A, 4, 0x20);
    writeReg(scc, A, 3, 0xC1);
    const uint8_t one[] = { 0xAA };
    scc.receiveFrame(A, one, 1, /*crcError=*/true);
    assert(scc.dataRead(A) == 0xAA);
    (void)scc.dataRead(A);                  // FCS 1
    assert((scc.peekRr(A, 1) & 0x40) != 0 && "the bad frame reports CRC");
    (void)scc.dataRead(A);                  // FCS 2, End Of Frame
    scc.controlWrite(A, 0x30);              // WR0 Error Reset
    assert(scc.rxFifoCount(A) == 0);

    for (uint8_t b : { uint8_t{0x11}, uint8_t{0x22}, uint8_t{0x33}, uint8_t{0x44} }) {
        scc.receiveByte(A, b);
        assert(scc.dataRead(A) == b);
        if ((scc.peekRr(A, 1) & 0x40) != 0 || scc.rxFifoCount(A) != 0) {
            std::printf("  clean byte $%02X reports RR1=$%02X fifo=%d: the slot "
                        "kept the CRC error of a frame long gone\n",
                        b, scc.peekRr(A, 1), scc.rxFifoCount(A));
            assert(false && "a bad FCS poisoned the FIFO slot");
        }
    }
    std::printf("  ok: a bad FCS does not outlive its own character\n");
}

// Bug hunt #8 (ASan): `restoreSnapshot` took one up-front budget of
// 2 x kPerChannel that reserved nothing for either channel's variable-length
// SDLC frame, so a blob whose channel-A frame ran to the last byte let
// channel B read its whole fixed part off the end of the buffer before the
// frame-length check could refuse it. Reached from any truncated .p2s or
// rewind blob through WorkstationCard::loadSnapshotState. The blob is placed
// against a PROT_NONE guard page so the over-read is a crash, not a silent
// read, on every POSIX runner.
void testSnapshotShortBlobIsRefused()
{
    Scc8530Device scc;
    std::vector<uint8_t> blob;
    scc.appendSnapshot(blob);
    // Header 5 + global 15 + channel A's fixed part, whose last two bytes are
    // its frame length; the frame runs to the end of the blob, so channel B
    // has nothing left.
    // (81 since the v4 receive ring: 3 x 4 FIFO bytes where v3 had 3 x 3.)
    const size_t lenAt = 5 + 15 + 81;
    assert(blob.size() > lenAt + 2);
    const size_t frameLen = blob.size() - (lenAt + 2);
    blob[lenAt]     = static_cast<uint8_t>(frameLen & 0xFF);
    blob[lenAt + 1] = static_cast<uint8_t>(frameLen >> 8);

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    const long pageL = sysconf(_SC_PAGESIZE);
    const size_t page = pageL > 0 ? static_cast<size_t>(pageL) : 4096;
    void* region = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region != MAP_FAILED);
    assert(mprotect(static_cast<uint8_t*>(region) + page, page, PROT_NONE) == 0);
    uint8_t* at = static_cast<uint8_t*>(region) + page - blob.size();
    std::memcpy(at, blob.data(), blob.size());
    Scc8530Device victim;
    assert(!victim.restoreSnapshot(at, blob.size()) &&
           "a blob with no room for channel B was accepted");
    munmap(region, 2 * page);
#else
    Scc8530Device victim;
    assert(!victim.restoreSnapshot(blob.data(), blob.size()));
#endif
    std::printf("  ok: a short snapshot blob is refused without reading past it\n");
}

void testSnapshotRoundTrip()
{
    Scc8530Device scc;
    scc.setPclk(Scc8530Device::kDefaultPclk);
    scc.setRtxc(A, Scc8530Device::kDefaultPclk);

    const uint16_t tc = timeConstant(Scc8530Device::kDefaultPclk, 9600, 16);
    writeReg(scc, A, 4, 0x44);
    writeReg(scc, A, 3, 0xC1);
    writeReg(scc, A, 11, 0x50);
    writeReg(scc, A, 12, static_cast<uint8_t>(tc & 0xFF));
    writeReg(scc, A, 13, 0x00);
    writeReg(scc, A, 14, 0x13);     // local loopback + BRG
    writeReg(scc, A, 5, 0x68);
    writeReg(scc, A, 9, 0x09);      // MIE + VIS
    writeReg(scc, A, 2, 0x30);
    writeReg(scc, A, 1, 0x10);
    scc.receiveByte(A, 0x33);       // something in the FIFO and an IP bit
    scc.dataWrite(A, 0x44);         // and a byte in the shift register

    std::vector<uint8_t> blob;
    scc.appendSnapshot(blob);
    assert(!blob.empty());

    const uint8_t  rr3  = scc.peekRr(A, 3);
    const uint32_t rate = scc.txRate(A);
    const bool     intL = scc.intAsserted();
    assert(rate == 9600 && rr3 != 0 && intL);

    scc.reset();
    assert(scc.txRate(A) != rate);
    assert(scc.rxFifoCount(A) == 0);

    bool line = intL;
    scc.setIntCallback([&](bool st) { line = st; });
    assert(scc.restoreSnapshot(blob.data(), blob.size()));
    assert(scc.txRate(A) == rate);
    assert(scc.peekRr(A, 3) == rr3);
    assert(scc.rxFifoCount(A) == 1);
    assert(scc.txBusy(A));
    assert(scc.intAsserted() == intL);
    assert(line == intL && "restore must republish the /INT line");
    assert(scc.dataRead(A) == 0x33);

    // Foreign, short and truncated blobs change nothing.
    std::vector<uint8_t> foreign(blob.size(), 0x5A);
    assert(!scc.restoreSnapshot(foreign.data(), foreign.size()));
    assert(!scc.restoreSnapshot(blob.data(), 4));
    assert(!scc.restoreSnapshot(blob.data(), blob.size() - 1));
    assert(scc.txRate(A) == rate);
    std::printf("  ok: snapshot round-trips and rejects foreign blobs\n");
}


// ─── 14. MAME master's interrupt-block fixes (bug hunt 2026-09-29) ───────

namespace {

/// Leave channel `ch` with a Transmit Buffer Empty interrupt pending: Tx
/// interrupts on, transmitter on, one byte loaded straight into the shift
/// register so the buffer is empty again and TBE raises the IP.
void armTxIp(Scc8530Device& scc, int ch)
{
    writeReg(scc, ch, 4, 0x44);     // x16 clock, 1 stop bit
    writeReg(scc, ch, 5, 0x68);     // Tx enable, 8 bits
    writeReg(scc, ch, 11, 0x50);    // Tx/Rx clocks from the BRG
    writeReg(scc, ch, 12, 0x00);
    writeReg(scc, ch, 13, 0x00);
    writeReg(scc, ch, 14, 0x03);    // BRG from PCLK, enabled
    writeReg(scc, ch, 1, 0x02);     // Tx interrupt enable
    scc.dataWrite(ch, 0x55);
}

} // namespace

// F1. WR9's channel reset "resets all IPs and IUSs and disables all
// interrupts in that channel" — its own, not the other one's. The pin ran
// a device-wide wipe from channel A's reset only: Channel A Reset dropped
// channel B's request, and Channel B Reset left its own /INT asserted.
// MAME master `reset_interrupts(int index)`, z80scc.cpp:671-684 / 1176.
void testChannelResetClearsOnlyItsOwnInterrupts()
{
    {
        Scc8530Device scc;
        writeReg(scc, A, 9, 0x08);      // MIE
        armTxIp(scc, B);
        assert(scc.intAsserted());
        assert(scc.peekRr(A, 3) == 0x02);   // channel B Tx IP
        writeReg(scc, A, 9, 0x48);      // Channel B Reset, MIE kept
        assert(!scc.intAsserted() && "channel B reset left its /INT asserted");
        assert(scc.peekRr(A, 3) == 0x00);
    }
    {
        Scc8530Device scc;
        writeReg(scc, A, 9, 0x08);
        armTxIp(scc, A);
        armTxIp(scc, B);
        assert(scc.peekRr(A, 3) == 0x12);   // A Tx + B Tx
        writeReg(scc, A, 9, 0x88);      // Channel A Reset, MIE kept
        assert(scc.peekRr(A, 3) == 0x02 && "channel A reset wiped channel B's IP");
        assert(scc.intAsserted());
        // The acknowledge now serves channel B's request.
        assert(scc.intAck() >= 0);
    }
    {
        // IUS too: an in-service channel B interrupt survives channel A's
        // reset and still blocks the LOWER-priority sources behind it until
        // its own Reset Highest IUS. The Zilog order is Rx > Tx > Ext/Status
        // per channel (manual p. 36), so Rx B in service blocks Tx B.
        Scc8530Device scc;
        writeReg(scc, A, 9, 0x08);
        writeReg(scc, B, 3, 0xC1);
        writeReg(scc, B, 1, 0x10);      // Rx on all chars
        scc.receiveByte(B, 0x99);
        assert(scc.intAck() >= 0);      // channel B Rx now in service
        assert(!scc.intAsserted());
        writeReg(scc, A, 9, 0x88);      // Channel A Reset
        armTxIp(scc, B);                // Tx B: lower than the Rx B in service
        writeReg(scc, B, 1, 0x12);
        assert(!scc.intAsserted() && "channel A's reset dropped channel B's IUS");
        scc.controlWrite(B, 0x38);      // Reset Highest IUS
        assert(scc.intAsserted() && "channel B Tx is served once the IUS clears");
    }
    std::printf("  ok: a WR9 channel reset clears only that channel's IP/IUS\n");
}

// F2. "If the corresponding IE bit is not set, the IP for that source of
// interrupt will never be set": clearing an enable in WR1 withdraws the
// request; the latched conditions come back with the enable. MAME master
// `do_sccreg_wr1`, z80scc.cpp:1913-1932.
void testWr1DisableWithdrawsPendingIp()
{
    Scc8530Device scc;
    writeReg(scc, A, 9, 0x08);
    armTxIp(scc, B);
    assert(scc.intAsserted());
    writeReg(scc, B, 1, 0x00);
    assert(!scc.intAsserted() && "Tx IE off left the request pending");
    assert(scc.peekRr(A, 3) == 0x00);

    // Receive: withdrawn with the mode, back while a character waits.
    writeReg(scc, A, 3, 0xC1);
    writeReg(scc, A, 1, 0x10);
    scc.receiveByte(A, 0x42);
    assert(scc.peekRr(A, 3) == 0x20 && scc.intAsserted());
    writeReg(scc, A, 1, 0x00);
    assert(scc.peekRr(A, 3) == 0x00 && !scc.intAsserted());
    writeReg(scc, A, 1, 0x10);
    assert(scc.peekRr(A, 3) == 0x20 && scc.intAsserted());
    assert(scc.dataRead(A) == 0x42);
    assert(!scc.intAsserted());

    // External/status: withdrawn with D0, re-raised from the held latch.
    writeReg(scc, A, 15, 0x08);     // DCD
    writeReg(scc, A, 1, 0x01);
    scc.dcdW(A, false);
    assert(scc.peekRr(A, 3) == 0x08);
    writeReg(scc, A, 1, 0x00);
    assert(scc.peekRr(A, 3) == 0x00 && !scc.intAsserted());
    writeReg(scc, A, 1, 0x01);
    assert(scc.peekRr(A, 3) == 0x08 && scc.intAsserted() &&
           "the latched ext/status condition comes back with its enable");
    std::printf("  ok: WR1 turning a source off withdraws its IP\n");
}

// F3. MIE gates the REQUEST and the acknowledge, not the IP bits — Zilog's
// documented polling mode reads them in RR3 with MIE clear. And clearing
// MIE drops a /INT already asserted. MAME master z80scc.cpp:579-583,
// 605-606, 750-754, 2087-2089.
void testMieGatesLineNotPending()
{
    Scc8530Device scc;
    bool line = false;
    scc.setIntCallback([&](bool s) { line = s; });
    writeReg(scc, A, 9, 0x08);
    armTxIp(scc, B);
    assert(line && scc.intAsserted());

    writeReg(scc, A, 9, 0x00);      // MIE off
    assert(!line && !scc.intAsserted() && "clearing MIE left /INT asserted");
    assert(scc.peekRr(A, 3) == 0x02 && "the IP stays pollable");
    assert(scc.intAck() == -1 && "no acknowledge with MIE clear");

    writeReg(scc, A, 9, 0x08);      // MIE back on
    assert(line && scc.intAsserted() && "no IUS was set by the refused ack");

    // Polling from reset: MIE never set, IP bits still appear.
    Scc8530Device poll;
    writeReg(poll, A, 3, 0xC1);
    writeReg(poll, A, 1, 0x10);
    poll.receiveByte(A, 0x41);
    assert(poll.peekRr(A, 3) == 0x20 && "RR3 must show the IP in polled mode");
    assert(!poll.intAsserted());
    assert(poll.dataRead(A) == 0x41);
    assert(poll.peekRr(A, 3) == 0x00);
    std::printf("  ok: MIE gates /INT and the ack, RR3 still polls\n");
}

// F4. The BRG Zero Count is an external/status source like the others:
// WR1 D0 must be set for it to raise an IP. MAME master `brg_tick`,
// z80scc.cpp:1183-1188.
void testZeroCountObeysWr1ExtEnable()
{
    Scc8530Device scc;
    writeReg(scc, A, 9, 0x08);
    writeReg(scc, A, 12, 10);
    writeReg(scc, A, 13, 0);
    writeReg(scc, A, 15, 0x02);     // Zero Count IE
    writeReg(scc, A, 14, 0x03);
    scc.tick(1000);
    assert(!scc.intAsserted() && scc.peekRr(A, 3) == 0x00 &&
           "Zero Count interrupted with WR1 D0 clear");
    writeReg(scc, A, 1, 0x01);
    scc.tick(1000);
    assert(scc.intAsserted() && scc.peekRr(A, 3) == 0x08);
    std::printf("  ok: Zero Count needs WR1's ext/status enable\n");
}

// The Special Receive Condition still locks the FIFO with receive
// interrupts disabled, but raises no interrupt. MAME master `data_read`,
// z80scc.cpp:2424-2428.
void testSpecialConditionObeysRxIntMode()
{
    Scc8530Device scc;
    writeReg(scc, A, 9, 0x08);
    writeReg(scc, A, 4, 0x20);      // SDLC: End Of Frame is a special condition
    writeReg(scc, A, 3, 0xC1);
    const uint8_t one[] = { 0x7E };
    scc.receiveFrame(A, one, 1);
    assert(scc.dataRead(A) == 0x7E);
    (void)scc.dataRead(A);                  // FCS 1
    (void)scc.dataRead(A);                  // FCS 2 — End Of Frame
    assert((scc.peekRr(A, 1) & 0x80) != 0);
    // Polled mode (WR1 D4-D3 = 00) does not HOLD the character either — only
    // modes 01 and 11 do (Zilog manual, WR1 / Error Reset).
    assert(scc.rxFifoCount(A) == 0 && "polled mode does not hold the character");
    assert(!scc.intAsserted() && scc.peekRr(A, 3) == 0x00 &&
           "a special condition interrupted with Rx interrupts disabled");
    std::printf("  ok: the special condition obeys WR1's Rx interrupt mode\n");
}

// The receive ring grew from 3 to 4 slots, and the snapshot to v4. A v3
// blob — a 3-slot ring whose read pointer may sit anywhere — restores with
// its characters in the same read order.
void testSnapshotV3FifoRestores()
{
    Scc8530Device src;
    std::vector<uint8_t> blob;
    src.appendSnapshot(blob);
    // 4 magic + 1 version + 3 + 6 + 6 device bytes; per channel (empty SDLC
    // buffers) 17 registers + 3 x 4 FIFO + 2 pointers + 3 + 6 + 2 + 16 + 1
    // + 2 + 8 + 4 + 8 + 2 + 1 + 2 = 86 bytes.
    constexpr std::size_t kHead = 20, kChan = 86, kFifo = 17;
    assert(blob.size() == kHead + 2 * kChan);
    assert(blob[4] == 4);
    // Down-convert: drop the fourth slot of data, error and EOF in each
    // channel, highest offset first.
    for (int ch = 1; ch >= 0; --ch) {
        const std::size_t f = kHead + static_cast<std::size_t>(ch) * kChan + kFifo;
        blob.erase(blob.begin() + static_cast<long>(f + 11));
        blob.erase(blob.begin() + static_cast<long>(f + 7));
        blob.erase(blob.begin() + static_cast<long>(f + 3));
    }
    blob[4] = 3;
    // Channel A's 3-slot ring: rp = 2, wp = 1, so the read order is slot 2
    // then slot 0; slot 1 is the parked write slot.
    const std::size_t fa = kHead + kFifo;
    blob[fa + 0] = 0xB2;
    blob[fa + 1] = 0xCC;
    blob[fa + 2] = 0xA1;
    blob[fa + 9]  = 2;      // rp
    blob[fa + 10] = 1;      // wp

    Scc8530Device dst;
    assert(dst.restoreSnapshot(blob.data(), blob.size()));
    assert(dst.rxFifoCount(A) == 2);
    assert(dst.dataRead(A) == 0xA1);
    assert(dst.dataRead(A) == 0xB2);
    assert(dst.rxFifoCount(A) == 0);
    std::printf("  ok: a v3 snapshot's 3-slot receive ring restores in order\n");
}

int main()
{
    testHardwareResetValues();
    testRegisterPointerProtocol();
    testFirmwareIsrIdiom();
    testBaudRateGenerator();
    testLoopbackTiming();
    testReceiveFifoOverrun();
    testErrorResetStepsFifo();
    testInterruptVectorAndIus();
    testZeroCountInterrupt();
    testModemPinsAndExtStatusLatch();
    testRtsDtrAndWreqPins();
    testWr8IsTheTransmitBuffer();
    testBusOrderings();
    testWr9Resets();
    testSdlcFraming();
    testSdlcReceive();
    testSnapshotShortBlobIsRefused();
    testSdlcCrcErrorDoesNotPoisonTheSlot();
    testSnapshotRoundTrip();
    testChannelResetClearsOnlyItsOwnInterrupts();
    testWr1DisableWithdrawsPendingIp();
    testMieGatesLineNotPending();
    testZeroCountObeysWr1ExtEnable();
    testSpecialConditionObeysRxIntMode();
    testSnapshotV3FifoRestores();
    std::printf("OK scc8530_smoke\n");
    return 0;
}
