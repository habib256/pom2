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

// passport_midi — the Passport MIDI card and its host side, against the
// numbers their sources print:
//   * MC6840 ([B] Motorola data sheet): N+1 clocks to a 16-bit Time Out,
//     Figure 10's dual 8-bit example (L = 4, M = 3 -> 20 clocks), the ÷8
//     prescaler, the RS-RT flag rule, CR10 and CRX4;
//   * MC6850 ([B] data sheet): the double-buffered transmitter — TDRE back
//     at once, the next character one frame later — and the word formats;
//   * the card on a real bus under a real 6502 (decode, the OR'd IRQ that
//     MAME's a2midi drops, three bytes 320 µs apart);
//   * the MIDI 1.0 stream rules (running status, real-time inside a
//     message, SysEx) and the Standard MIDI File bytes, exact.

#include "M6502.h"
#include "Mc6840Ptm.h"
#include "Mc6850Acia.h"
#include "Memory.h"
#include "MidiFileWriter.h"
#include "MidiMessageAssembler.h"
#include "MidiOutputCoordinator.h"
#include "PassportMidiCard.h"
#include "SlotBus.h"
#include "SlotCardFactory.h"
#include "SlotConfigurationCoordinator.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

using pom2::Mc6840Ptm;
using pom2::Mc6850Acia;

namespace {

// Timer 1 programmed through the register map, as a driver does it:
// CR2 bit 0 to reach CR1, the MSB buffer, the LSB with the latch write,
// then CR1 (which releases the CR10 hold).
void programT1(Mc6840Ptm& p, uint16_t latch, uint8_t cr1)
{
    p.write(1, 0x01);                 // CR2: CR20 = 1, RS0 writes CR1
    p.write(2, uint8_t(latch >> 8));  // MSB buffer
    p.write(3, uint8_t(latch));       // timer 1 latches
    p.write(0, cr1);                  // CR1
}

void testPtm16Bit()
{
    Mc6840Ptm p;
    assert(p.peekStatus() == 0 && (p.control(0) & 1));       // RESET: CR10 held
    assert(p.latch(0) == 0xFFFF && p.latch(2) == 0xFFFF);
    programT1(p, 9, 0x42);            // internal clock, IRQ enabled, continuous
    p.tick(9);
    assert(p.peekStatus() == 0);      // N clocks: the counter is at zero
    p.tick(1);                        // N + 1: Time Out
    assert(p.peekStatus() == 0x81 && p.irq());
    assert(p.counter(0) == 9);        // reloaded from the latch
    // RS-RT: a counter read clears the flag only after a status read saw it.
    (void)p.read(2);
    assert(p.peekStatus() == 0x81);
    assert(p.read(1) == 0x81);
    assert(p.read(2) == 0x00 && p.read(3) == 9);              // MSB, then the LSB buffer
    assert(p.peekStatus() == 0 && !p.irq());
    // Cyclic: a skip of 3 periods + 4 clocks lands 4 into the next one.
    p.tick(10 * 3 + 4);
    assert((p.peekStatus() & 1) && p.counter(0) == 5);
    // Write Timer Latches clears its flag (and initialises, CRX4 = 0).
    p.write(2, 0x00); p.write(3, 0x20);
    assert(p.peekStatus() == 0 && p.counter(0) == 0x20);
    assert(p.clocksToNextTimeOut() == 0x21);
    std::printf("  ok: 6840 16-bit: N+1 clocks, RS-RT, latch write clears\n");
}

void testPtmDual8AndPrescaler()
{
    // [B] data sheet Figure 10: MSB = 03 = M, LSB = 04 = L -> (M+1)(L+1) = 20.
    Mc6840Ptm p;
    programT1(p, 0x0304, 0x46);       // dual 8-bit, internal clock, IRQ enabled
    p.tick(19);
    assert(p.peekStatus() == 0);
    p.tick(1);
    assert(p.peekStatus() == 0x81);
    assert(p.counter(0) == 0x0304);
    p.tick(7);                         // LSB 4..0 (5 clocks), reload, MSB 3 -> 2, then 2 more
    assert(p.counter(0) == 0x0202);
    // L = 0 is the 16-bit-like case: M + 1 clocks.
    Mc6840Ptm q;
    programT1(q, 0x0500, 0x46);
    q.tick(5); assert(q.peekStatus() == 0);
    q.tick(1); assert(q.peekStatus() == 0x81);

    // Timer 3 with its ÷8 prescaler: latch 1 -> 2 counts -> 16 E clocks.
    Mc6840Ptm t;
    t.write(1, 0x00);                  // CR2: CR20 = 0, RS0 writes CR3
    t.write(0, 0x43);                  // CR3: ÷8, internal clock, IRQ enabled
    t.write(1, 0x01);                  // now CR1
    t.write(0, 0x00);                  // release the hold
    t.write(6, 0x00); t.write(7, 0x01);
    t.tick(15); assert(t.peekStatus() == 0);
    t.tick(1);  assert(t.peekStatus() == 0x84);
    std::printf("  ok: 6840 dual 8-bit = (L+1)(M+1) (Figure 10), timer 3 / 8\n");
}

void testPtmHoldAndModes()
{
    Mc6840Ptm p;
    programT1(p, 3, 0x42);
    p.tick(2);
    p.write(1, 0x01); p.write(0, 0x43);   // CR10 set: held, preset
    assert(p.counter(0) == 3);
    p.tick(100);
    assert(p.peekStatus() == 0 && p.counter(0) == 3);
    // CRX4 = 1: a latch write does not initialise while running.
    p.write(0, 0x52);
    p.tick(1);
    assert(p.counter(0) == 2);
    p.write(2, 0x00); p.write(3, 0x40);
    assert(p.latch(0) == 0x40 && p.counter(0) == 2);
    // A measurement mode (CRX3 = 1) waits for a gate fall: it stays still.
    Mc6840Ptm m;
    programT1(m, 3, 0x4A);
    m.tick(1000);
    assert(m.peekStatus() == 0 && m.counter(0) == 3);
    // Snapshot round trip.
    std::vector<uint8_t> blob;
    p.appendState(blob);
    Mc6840Ptm r;
    assert(r.loadState(blob.data(), blob.size()));
    std::vector<uint8_t> again;
    r.appendState(again);
    assert(blob == again);
    std::printf("  ok: 6840 CR10 hold, CRX4, measurement modes idle, snapshot\n");
}

void testAciaTiming()
{
    Mc6850Acia a;
    a.setTiming(2.0);                  // 2 bus ticks a TX clock: ÷16 -> 32-tick bits
    a.powerOn();
    assert(a.status() == 0);           // held in master reset
    a.writeData(0x55);                 // ignored in reset
    std::vector<Mc6850Acia::Sent> out;
    a.writeControl(0x15);              // ÷16, 8N1, no interrupt
    assert(a.status() == 0x02);        // TDRE
    a.writeData(0x90);
    assert(a.status() & 0x02);         // transferred at once: TDRE again
    a.writeData(0x3C);
    assert(!(a.status() & 0x02));      // the second waits in the TDR
    a.tick(319.0, out);
    assert(out.empty());
    a.tick(1.0, out);
    assert(out.size() == 1 && out[0].byte == 0x90 && out[0].at == 320.0 && out[0].framed);
    assert(a.status() & 0x02);         // the second moved into the shift register
    a.tick(320.0, out);
    assert(out.size() == 2 && out[1].byte == 0x3C && out[1].at == 640.0);
    // Transmit interrupt (CR6:5 = 01) while TDRE.
    a.writeControl(0x35);
    assert(a.irq());
    a.writeControl(0x15);
    assert(!a.irq());
    // 7 bits + even parity + 2 stop: 11 bits; parity lands on bit 7.
    out.clear();
    a.writeControl(0x01);
    a.writeData(0x41);                 // two ones: parity 0
    a.tick(a.frameTicks(), out);
    a.writeData(0x43);                 // three ones: parity 1
    a.tick(a.frameTicks(), out);
    assert(a.frameTicks() == 11 * 32.0);
    assert(out.size() == 2 && out[0].byte == 0x41 && out[1].byte == 0xC3);
    // 8 bits + even parity: the parity bit sits where an 8N1 receiver wants
    // its stop bit, so a 0 there is a framing error.
    out.clear();
    a.writeControl(0x19);
    a.writeData(0x01); a.tick(a.frameTicks(), out);   // parity 1: framed
    a.writeData(0x03); a.tick(a.frameTicks(), out);   // parity 0: lost
    assert(out.size() == 2 && out[0].framed && !out[1].framed);
    // Master reset drops what was waiting.
    a.writeData(0x11); a.writeData(0x22);
    a.writeControl(0x03);
    out.clear();
    a.tick(10000.0, out);
    assert(out.empty() && a.status() == 0);
    std::printf("  ok: 6850 double buffer, frame timing, parity on the wire, reset\n");
}

void testCardOnABus()
{
    // Factory, catalog key, live key.
    auto made = pom2::SlotCardFactory{}.create({"passportmidi", 2});
    assert(made && dynamic_cast<pom2::PassportMidiCard*>(made.card.get()));
    SlotBus bare;
    bare.plug(2, std::move(made.card));
    assert(pom2::SlotConfigurationCoordinator{}.captureLive(bare).keys[2] == "passportmidi");

    // A 6502 sends Note On C4 velocity 100 through slot 2, polling TDRE:
    //   LDA #$03 / STA $C0A8      master reset
    //   LDA #$15 / STA $C0A8      ÷16, 8N1
    //   LDX #0
    // L LDA $C0A8 / AND #2 / BEQ L
    //   LDA T,X / STA $C0A9 / INX / CPX #3 / BNE L
    //   JMP *
    // T .byte $90,$3C,$64
    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.setIIEMode(true);
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    auto owned = std::make_unique<pom2::PassportMidiCard>(2);
    auto* card = owned.get();
    mem.slotBus().plug(2, std::move(owned));
    const uint8_t prog[] = {
        0xA9, 0x03, 0x8D, 0xA8, 0xC0,
        0xA9, 0x15, 0x8D, 0xA8, 0xC0,
        0xA2, 0x00,
        0xAD, 0xA8, 0xC0, 0x29, 0x02, 0xF0, 0xF9,     // $030C: poll TDRE
        0xBD, 0x21, 0x03, 0x8D, 0xA9, 0xC0, 0xE8, 0xE0, 0x03, 0xD0, 0xEE,
        0x4C, 0x1E, 0x03,                             // $031E: JMP *
        0x90, 0x3C, 0x64,                             // $0321
    };
    for (std::size_t i = 0; i < sizeof prog; ++i)
        mem.writeRamUnchecked(uint16_t(0x0300 + i), prog[i]);
    cpu.setProgramCounter(0x0300);
    for (int64_t t = 0; t < 5000;) t += cpu.run(64);
    std::vector<pom2::MidiOutByte> sent;
    card->drainMidiOut(sent);
    assert(sent.size() == 3);
    assert(sent[0].byte == 0x90 && sent[1].byte == 0x3C && sent[2].byte == 0x64);
    // 10 bits at 31 250 baud = 320 µs = 327.3 cycles at 1.0227 MHz: the
    // second and third bytes follow one frame apart (the card stamps each
    // at the instruction that sees it end, so within one instruction).
    const auto gap1 = sent[1].cycle - sent[0].cycle, gap2 = sent[2].cycle - sent[1].cycle;
    assert(gap1 >= 320 && gap1 <= 335 && gap2 >= 320 && gap2 <= 335);

    // The IRQ is the OR of both chips (a2midi's ptm_irq_w loses it).
    card->deviceSelectWrite(8, 0x35);             // ACIA: transmit interrupt on
    assert(cpu.getIrqSourceMask() & (1u << M6502::IRQ_SRC_SLOT2));
    card->deviceSelectWrite(1, 0x01);             // PTM timer 1, 4 clocks, IRQ on
    card->deviceSelectWrite(2, 0x00);
    card->deviceSelectWrite(3, 0x03);
    card->deviceSelectWrite(0, 0x42);
    card->advanceCycles(10);
    assert(card->ptm().irq());
    card->deviceSelectWrite(8, 0x15);             // the ACIA lets go…
    assert(cpu.getIrqSourceMask() & (1u << M6502::IRQ_SRC_SLOT2));   // …the PTM still pulls
    (void)card->deviceSelectRead(1);
    (void)card->deviceSelectRead(2);              // RS-RT clears the timer flag
    assert(!card->ptm().irq());
    assert(!(cpu.getIrqSourceMask() & (1u << M6502::IRQ_SRC_SLOT2)));

    // Ctrl-Reset reaches the PTM, not the ACIA.
    card->onReset();
    assert(card->ptm().control(0) & 1);
    assert(card->acia().control() == 0x15);
    // Unused registers read 0.
    assert(card->deviceSelectRead(0xA) == 0 && card->deviceSelectRead(0xF) == 0);
    std::printf("  ok: card on a bus: a 6502 sends a Note On 320 µs a byte, OR'd IRQ, reset\n");
}

void testAssembler()
{
    pom2::MidiMessageAssembler m;
    std::vector<pom2::MidiMessage> out;
    for (uint8_t b : {0x90, 0x3C, 0x64, 0x3E, 0x64}) m.feed(b, 1, out);   // running status
    assert(out.size() == 2 && out[1].bytes == std::vector<uint8_t>({0x90, 0x3E, 0x64}));
    out.clear();
    for (uint8_t b : {0x80, 0xF8, 0x3C, 0x00}) m.feed(b, 2, out);        // clock inside
    assert(out.size() == 2 && out[0].bytes == std::vector<uint8_t>({0xF8}));
    assert(out[1].bytes == std::vector<uint8_t>({0x80, 0x3C, 0x00}));
    out.clear();
    for (uint8_t b : {0xC0, 0x05, 0x06}) m.feed(b, 3, out);              // 1 data byte
    assert(out.size() == 2 && out[1].bytes == std::vector<uint8_t>({0xC0, 0x06}));
    out.clear();
    for (uint8_t b : {0xF0, 0x41, 0x10, 0xF7}) m.feed(b, 4, out);        // SysEx
    assert(out.size() == 1 && out[0].bytes == std::vector<uint8_t>({0xF0, 0x41, 0x10, 0xF7}));
    out.clear();
    for (uint8_t b : {0xF0, 0x7E, 0x90, 0x40, 0x7F}) m.feed(b, 5, out);  // SysEx cut by a status
    assert(out.size() == 2 && out[0].bytes == std::vector<uint8_t>({0xF0, 0x7E, 0xF7}));
    out.clear();
    for (uint8_t b : {0xF2, 0x10, 0x20, 0x41}) m.feed(b, 6, out);        // common cancels running
    assert(out.size() == 1 && out[0].bytes.size() == 3);
    std::printf("  ok: MIDI 1.0 stream: running status, real-time, SysEx, system common\n");
}

void testMidiFile()
{
    pom2::MidiFileWriter w;
    w.start(1000, 1000.0);                                 // 1000 cycles = 1 s
    w.add({1000, {0x90, 0x3C, 0x64}});
    w.add({1000, {0xF8}});                                  // not stored
    w.add({2000, {0x80, 0x3C, 0x00}});                     // 1 s later = 960 ticks
    w.add({2000, {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}});
    const std::vector<uint8_t> want = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0x01,0xE0,
        'M','T','r','k', 0,0,0,28,
        0x00, 0xFF,0x51,0x03, 0x07,0xA1,0x20,
        0x00, 0x90,0x3C,0x64,
        0x87,0x40, 0x80,0x3C,0x00,
        0x00, 0xF0,0x05, 0x7E,0x7F,0x09,0x01,0xF7,
        0x00, 0xFF,0x2F,0x00,
    };
    assert(w.bytes() == want);
    std::printf("  ok: Standard MIDI File bytes, timed on the emulated clock\n");
}

void testCoordinatorRecording()
{
    pom2::MidiOutputCoordinator c;          // no port: nothing is played
    c.startRecording();
    std::vector<pom2::MidiOutByte> frame = {{5000, 0x90}, {5000, 0x3C}, {5327, 0x64}};
    c.feed(frame, true, 1022727.0);
    assert(frame.empty() && c.recording() && c.recordedEvents() == 1);
    std::vector<pom2::MidiOutByte> later = {{1027727, 0x80}, {1027727, 0x3C}, {1028054, 0x00}};
    c.feed(later, true, 1022727.0);
    const auto path = std::filesystem::temp_directory_path() / "pom2_passport_midi_test.mid";
    std::string error;
    assert(c.stopRecording(path, error));
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::filesystem::remove(path);
    // 1 022 727 cycles after the first byte's last = 1 s = 960 ticks.
    const std::vector<uint8_t> tail = {0x87, 0x40, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00};
    assert(bytes.size() > tail.size() &&
           std::equal(tail.begin(), tail.end(), bytes.end() - std::ptrdiff_t(tail.size())));
    assert(!c.recording());
    c.feed(frame, false, 1022727.0);       // the card went away: no crash, no output
    std::printf("  ok: coordinator records a .mid from the card's stamps\n");
}

} // namespace

int main()
{
    testPtm16Bit();
    testPtmDual8AndPrescaler();
    testPtmHoldAndModes();
    testAciaTiming();
    testCardOnABus();
    testAssembler();
    testMidiFile();
    testCoordinatorRecording();
    std::printf("passport_midi: all assertions passed\n");
    return 0;
}
