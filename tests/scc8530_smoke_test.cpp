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
//   7. Receive FIFO depth 3, MAME's overrun handling, and the Error Reset
//      step (z80scc.cpp:2566 / 2362 / 1811).
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

#include "Scc8530Device.h"

#include <cassert>
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

    // The NMOS 8530 has a 3-byte receive FIFO, and MAME's ring keeps one
    // slot as the full/empty discriminator, so two bytes fit and the third
    // overruns (z80scc.cpp:2566).
    scc.receiveByte(A, 0x10);
    scc.receiveByte(A, 0x20);
    assert(scc.rxFifoCount(A) == 2);
    assert((scc.peekRr(A, 0) & 0x01) != 0);     // Rx Character Available

    scc.receiveByte(A, 0x30);
    assert(scc.rxFifoCount(A) == 2);            // dropped, not queued

    assert(scc.dataRead(A) == 0x10);
    assert(scc.dataRead(A) == 0x20);
    assert(scc.rxFifoCount(A) == 0);
    assert((scc.peekRr(A, 0) & 0x01) == 0);

    // Worth pinning because it is a MAME divergence POM2 inherits on
    // purpose: MAME writes the overrunning byte into the slot the write
    // pointer is parked on and flags RR1 D5 in the *error* FIFO, but never
    // advances the pointer past it — so that slot is unreachable and the
    // Overrun status never reaches RR1 through a data read. On real
    // silicon the byte is discarded and the error surfaces. Reproducing
    // MAME rather than the datasheet keeps the oracle usable; a driver
    // that depends on seeing Overrun would be the reason to revisit it.
    assert((scc.peekRr(A, 1) & 0x20) == 0);
    std::printf("  ok: 3-deep Rx FIFO with MAME's overrun handling\n");
}

void testErrorResetStepsFifo()
{
    // WR0's Error Reset command unlocks the FIFO by stepping the read
    // pointer one slot (z80scc.cpp:1811) — the half of the lock/unlock
    // pair that is observable on this part.
    Scc8530Device scc;
    writeReg(scc, A, 3, 0xC1);
    scc.receiveByte(A, 0x11);
    scc.receiveByte(A, 0x22);
    assert(scc.rxFifoCount(A) == 2);

    scc.controlWrite(A, 0x30);      // WR0 Error Reset
    assert(scc.rxFifoCount(A) == 1);
    assert(scc.dataRead(A) == 0x22);
    assert(scc.rxFifoCount(A) == 0);
    std::printf("  ok: WR0 Error Reset steps the receive FIFO\n");
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

    // With MIE clear nothing may interrupt at all (z80scc.cpp:747).
    writeReg(scc, A, 9, 0x01);
    scc.receiveByte(A, 0x22);
    assert(!scc.intAsserted());
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
    // `update_extint` reports work outstanding and the latch stays
    // (z80scc.cpp:793) — RR0 keeps hiding the CTS pin.
    scc.controlWrite(A, 0x10);
    assert((scc.peekRr(A, 0) & 0x20) == 0);

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

    // Channel B reset leaves channel A alone (z80scc.cpp:2024).
    writeReg(scc, A, 9, 0x40);
    assert(scc.peekWr(B, 4) == 0x04);   // channel reset value
    assert(scc.peekWr(A, 4) == 0x44);

    // Force Hardware Reset restores the device-wide values and takes the
    // MIE / Status-High / DLC bits from the command byte itself.
    writeReg(scc, A, 4, 0x44);
    writeReg(scc, A, 9, 0xC8);          // hardware reset + MIE
    assert(scc.peekWr(A, 4) == 0x04);
    assert(scc.peekWr(A, 11) == 0x08);
    assert((scc.peekWr(A, 9) & 0x08) != 0);
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
    scc.receiveFrame(A, frame, 2);          // only two bytes fit the FIFO
    assert((scc.peekRr(A, 0) & 0x10) == 0 && "the opening flag clears hunt");
    assert(scc.rxFifoCount(A) == 2);

    assert(scc.dataRead(A) == 0x21);
    assert((scc.peekRr(A, 1) & 0x80) == 0 && "not the last byte");

    // The last byte carries End Of Frame with the no-residue code, and is a
    // special receive condition: the FIFO locks until Error Reset.
    assert(scc.dataRead(A) == 0x01);
    assert((scc.peekRr(A, 1) & 0x80) != 0 && "End Of Frame");
    assert((scc.peekRr(A, 1) & 0x0E) == 0x06 && "residue: no residue, 8 bits");
    assert(scc.rxFifoCount(A) == 1 && "the FIFO is locked on the special condition");

    scc.controlWrite(A, 0x30);              // WR0 Error Reset
    assert((scc.peekRr(A, 1) & 0x80) == 0);
    assert(scc.rxFifoCount(A) == 0);
    std::printf("  ok: End Of Frame locks the FIFO until Error Reset\n");

    // A frame received with a bad FCS raises the CRC error bit.
    Scc8530Device bad;
    writeReg(bad, A, 4, 0x20);
    writeReg(bad, A, 3, 0xC1);
    const uint8_t one[] = { 0x55 };
    bad.receiveFrame(A, one, 1, /*crcError=*/true);
    assert(bad.dataRead(A) == 0x55);
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
    assert(addr.rxFifoCount(A) == 2);
    assert(addr.dataRead(A) == 0x42);
    addr.controlWrite(A, 0x30);
    addr.receiveFrame(A, bcast, 2);
    assert(addr.rxFifoCount(A) == 2);
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
    testSnapshotRoundTrip();
    std::printf("OK scc8530_smoke\n");
    return 0;
}
