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

// POM2 — Super Serial transport seam.
//
// Pins that SuperSerialCard's bridge behaviour can be driven with NO socket,
// no listener and no worker thread: the test plays the far end on its own
// thread, so the assertions are deterministic instead of racing a worker.
//
// This is the case the seam was worth moving a thread for. Every assertion
// below previously required a real telnet client on loopback, and the ones
// about ordering could only be observed by sleeping and hoping.

#include "SuperSerialCard.h"

#include "fakes/FakeSuperSerialTransport.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace pom2;

// ACIA registers as the guest addresses them inside the slot window.
constexpr uint8_t kAciaData    = 0x8;
constexpr uint8_t kAciaStatus  = 0x9;
constexpr uint8_t kAciaCommand = 0xA;

// The 6551 only queues transmit data while DTR is asserted — the guest opens
// the port through the command register before it writes a byte. Skipping
// this makes every outbound write vanish, which is exactly what the card
// should do for a closed port.
void openPort(SuperSerialCard& card) { card.deviceSelectWrite(kAciaCommand, 0x01); }


// ── Text mode: telnet IAC negotiation never reaches the guest ────────────
//
// A telnet client opens by negotiating, and those three-byte IAC sequences
// are protocol, not data. Letting them through puts $FF $FB $01 into the
// guest's receive register, which is what a terminal program reads as noise.
void testTelnetNegotiationIsStripped()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));
    card.setRawMode(false);

    fake->connect();
    assert(card.clientConnected());

    // IAC WILL ECHO, then real data.
    fake->deliver({ 0xFF, 0xFB, 0x01, 'H', 'i' }, /*textMode=*/true);

    std::string got;
    for (int i = 0; i < 8; ++i) {
        const uint8_t status = card.deviceSelectRead(kAciaStatus);
        if ((status & 0x08) == 0) break;             // SR_RDRF clear: drained
        got.push_back(static_cast<char>(card.deviceSelectRead(kAciaData)));
    }
    assert(got == "Hi");

    std::printf("  telnet IAC negotiation never reaches the guest: OK\n");
}

// ── Raw mode: the stream is untouched, including $FF ─────────────────────
//
// Raw mode exists for binary transfers, where a byte that happens to equal
// IAC is data. Filtering it there corrupts the file — silently.
void testRawModePassesIacByteThrough()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));
    card.setRawMode(true);

    fake->connect();
    fake->deliver({ 0xFF, 0x00, 0x41 }, /*textMode=*/false);

    std::vector<uint8_t> got;
    for (int i = 0; i < 8; ++i) {
        if ((card.deviceSelectRead(kAciaStatus) & 0x08) == 0) break;
        got.push_back(card.deviceSelectRead(kAciaData));
    }
    assert((got == std::vector<uint8_t>{ 0xFF, 0x00, 0x41 }));

    std::printf("  raw mode passes an IAC byte through untouched: OK\n");
}

// ── Outbound: a literal $FF is escaped as IAC IAC in text mode ───────────
//
// The mirror of the case above. A guest that prints $FF must not be read by
// the peer as the start of a negotiation, so the escape is mandatory — and
// it is why drainTransportTx appends to a vector rather than filling a
// fixed buffer: escaping EXPANDS the stream.
void testOutboundIacIsEscapedInTextMode()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));
    card.setRawMode(false);
    fake->connect();
    openPort(card);

    card.deviceSelectWrite(kAciaData, 0xFF);
    const auto wire = fake->drain();
    assert((wire == std::vector<uint8_t>{ 0xFF, 0xFF }));
    // One guest byte produced two on the wire.
    assert(fake->lastDrainTaken == 1);

    std::printf("  outbound $FF is escaped as IAC IAC in text mode: OK\n");
}

// ── A paced peer holds the guest back, it does not lose bytes ───────────
//
// A real 6551 keeps TDRE low until its shift register has sent the byte at
// the programmed baud, so the guest waits at line speed. POM2 reported TDRE
// set whatever the backlog, and a LIST faster than the line overran the 4 KB
// ring: the oldest bytes went, from the middle of the stream. With the ring
// full and a peer connected, TDRE now reads 0; draining frees it again.
void testFullTxRingHoldsTdre()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));
    card.setRawMode(true);
    fake->connect();
    openPort(card);
    int written = 0;
    while ((card.deviceSelectRead(kAciaStatus) & 0x10) && written < 10000) {
        card.deviceSelectWrite(kAciaData, static_cast<uint8_t>(written));
        ++written;
    }
    assert(written == 4096 && "TDRE drops when the ring is full");
    const auto wire = fake->drain();
    assert(wire.size() == 4096 && wire[0] == 0 && wire[4095] == static_cast<uint8_t>(4095) &&
           "nothing was dropped");
    assert((card.deviceSelectRead(kAciaStatus) & 0x10) && "drained: TDRE is back");
    std::printf("  a full TX ring holds TDRE instead of dropping bytes: OK\n");
}

// The peer hangs up while the full ring holds TDRE low: no drain comes, but
// TDRE reads 1 again, and on a 6551 the bit going high IS the transmit
// interrupt. It was deferred to a drain that never ran, so an IRQ-driven
// driver (command $05) sat in WFI for good.
void testTxIrqWhenPeerDropsWithFullRing()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));
    card.setRawMode(true);
    card.setModemLinesTied(true);                  // no DCD/DSR edge
    fake->connect();
    card.deviceSelectWrite(kAciaCommand, 0x05);    // DTR, RX + TX IRQ on
    int written = 0;
    while ((card.deviceSelectRead(kAciaStatus) & 0x10) && written < 10000)
        card.deviceSelectWrite(kAciaData, static_cast<uint8_t>(written++));
    assert(written == 4096);
    card.advanceCycles(1);
    assert(!(card.deviceSelectRead(kAciaStatus) & 0x80) &&
           "ring full: TDRE low, no transmit interrupt");
    fake->disconnect();
    card.advanceCycles(1);
    const uint8_t st = card.deviceSelectRead(kAciaStatus);
    assert((st & 0x10) && "peer gone: TDRE reads 1 again");
    assert((st & 0x80) && "...and that edge is the transmit interrupt");
    std::printf("  a hang-up on a full ring raises the TX interrupt: OK\n");
}

// ── RFC 854: only a BARE CR is followed by NUL ───────────────────────────
// CR LF goes out as CR LF; it used to be CR NUL LF.
void testCrNulOnlyAfterBareCr()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));
    card.setRawMode(false);
    fake->connect();
    openPort(card);
    for (uint8_t b : { uint8_t('\r'), uint8_t('\n'), uint8_t('\r'), uint8_t('A') })
        card.deviceSelectWrite(kAciaData, b);
    const auto wire = fake->drain();
    assert((wire == std::vector<uint8_t>{ '\r', '\n', '\r', 0x00, 'A' }));
    std::printf("  CR LF stays CR LF, a bare CR gets its NUL: OK\n");
}

// ── A connection is no DCD/DSR edge when the tap already holds them up ──
// A real 6551 interrupts on a pin change only. With the printer tap armed
// the Auto cable holds DCD/DSR active, so a telnet peer arriving changes
// neither — the //c printer port's default used to take an interrupt.
void testNoDcdIrqWhenTapHoldsTheLines()
{
    for (const bool tap : { false, true }) {
        SuperSerialCard card(2);
        auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
        auto* fake = transport.get();
        card.setTransport(std::move(transport));
        card.setPrinterTap(tap);
        openPort(card);
        (void)card.deviceSelectRead(kAciaStatus);    // clears any IRQ
        fake->connect();
        const bool irq = (card.deviceSelectRead(kAciaStatus) & 0x80) != 0;
        assert(irq == !tap);
    }
    std::printf("  no DCD/DSR interrupt without a pin change: OK\n");
}

void testOutboundIacIsVerbatimInRawMode()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));
    card.setRawMode(true);
    fake->connect();
    openPort(card);

    card.deviceSelectWrite(kAciaData, 0xFF);
    assert((fake->drain() == std::vector<uint8_t>{ 0xFF }));

    std::printf("  outbound $FF is verbatim in raw mode: OK\n");
}

// ── The keyboard sink is a text-mode path only ───────────────────────────
//
// The sink is how a telnet session types at the Apple II. A binary transfer
// is not typing, and feeding it to the keyboard would inject the file into
// whatever the guest is running.
void testKeyboardSinkOnlyInTextMode()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));

    std::string typed;
    card.setKeyboardSink([&typed](uint8_t b) {
        typed.push_back(static_cast<char>(b));
    });

    card.setRawMode(false);
    fake->connect();
    fake->deliver({ 'a' }, /*textMode=*/true);
    assert(typed == "a");

    card.setRawMode(true);
    fake->deliver({ 'b' }, /*textMode=*/false);
    assert(typed == "a");   // unchanged: raw bytes are not keystrokes

    std::printf("  keyboard sink is a text-mode path only: OK\n");
}

// ── Connection edges reset the parser ────────────────────────────────────
//
// The IAC parser and the CR state persist across recv() chunks by design, so
// a client that drops mid-sequence would otherwise poison the next session.
void testReconnectResetsTelnetParser()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));
    card.setRawMode(false);

    fake->connect();
    fake->deliver({ 0xFF }, /*textMode=*/true);   // half an IAC sequence
    fake->disconnect();
    assert(!card.clientConnected());

    // A fresh client sends plain data. If the parser had kept its state it
    // would swallow this byte as the second half of the previous sequence.
    fake->connect();
    fake->deliver({ 'Z' }, /*textMode=*/true);

    std::string got;
    for (int i = 0; i < 4; ++i) {
        if ((card.deviceSelectRead(kAciaStatus) & 0x08) == 0) break;
        got.push_back(static_cast<char>(card.deviceSelectRead(kAciaData)));
    }
    assert(got == "Z");

    std::printf("  a reconnect resets the telnet parser: OK\n");
}

void testBinaryTransitionsRespectByteOrder()
{
    // RFC 856: BINARY changes the interpretation of subsequent data. A TCP
    // read boundary must not decide whether earlier CR/LF/NUL bytes survive.
    const std::vector<uint8_t> wire{
        'A', '\r', '\n', 0,             // NVT: collapse CRLF, discard NUL
        0xFF, 0xFB, 0,                  // WILL BINARY
        'B', '\r', '\n', 0, 0xFF, 0xFF, // binary: preserve data, unescape IAC
        0xFF, 0xFC, 0,                  // WONT BINARY
        'C', '\r', '\n', 0};
    const std::vector<uint8_t> expected{'A', '\r', 'B', '\r', '\n', 0, 0xFF, 'C', '\r'};
    // Exercise every two-chunk split, including inside the negotiations.
    for (size_t split = 0; split <= wire.size(); ++split) {
        SuperSerialCard card(2);
        std::vector<uint8_t> got;
        auto consume = [&](size_t begin, size_t end) {
            std::vector<uint8_t> chunk(wire.begin() + begin, wire.begin() + end);
            const size_t n = card.processTransportTextRx(chunk.data(), chunk.size());
            got.insert(got.end(), chunk.begin(), chunk.begin() + n);
        };
        consume(0, split);
        consume(split, wire.size());
        assert(got == expected && "Telnet decoding depends on TCP read boundaries");
    }
    std::printf("  BINARY transitions preserve byte order across every split: OK\n");
}


// ── SW2-6 gates the IRQ line, independently of the ACIA ──────────────────
//
// On a real Super Serial Card this DIP sits between the 6551's IRQ output and
// the slot's IRQ pin. With it OFF an interrupt-driven driver never fires
// however the command register is programmed — the two are independent, and
// that independence is exactly what makes the switch confusing on real
// hardware. Modelling the source but not the gate made a card configured for
// polling behave like one configured for interrupts. MAME `a2ssc.cpp:373`.
void testIrqDipGatesTheSlotLine()
{
    SuperSerialCard card(2);
    auto transport = std::make_unique<test::FakeSuperSerialTransport>(card);
    auto* fake = transport.get();
    card.setTransport(std::move(transport));
    card.setRawMode(false);
    fake->connect();

    // Enable receive interrupts in the ACIA: command register with DTR set
    // and the RX-IRQ-disable bit CLEAR.
    card.deviceSelectWrite(kAciaCommand, 0x01);
    assert(card.irqDipEnabled());          // shipped default: SW2-6 on

    fake->deliver({ 'x' }, /*textMode=*/true);
    // The transport raises the source off the CPU thread and marks the line
    // dirty; advanceCycles is where the card applies it, because assertIrq
    // mutates non-atomic SlotPeripheral state.
    card.advanceCycles(1);
    assert(card.slotIrqAsserted());        // the byte raised the line

    // Flip the switch: the line drops at once, as it does on a powered card.
    card.setIrqDipEnabled(false);
    assert(!card.irqDipEnabled());
    assert(!card.slotIrqAsserted());

    // The ACIA is untouched — its own source is still pending, so flipping
    // the switch back re-asserts without the guest doing anything.
    card.setIrqDipEnabled(true);
    assert(card.slotIrqAsserted());

    std::printf("  SW2-6 gates the slot IRQ line, ACIA untouched: OK\n");
}

} // namespace

int main()
{
    std::printf("Super Serial transport seam\n");
    testTelnetNegotiationIsStripped();
    testRawModePassesIacByteThrough();
    testOutboundIacIsEscapedInTextMode();
    testOutboundIacIsVerbatimInRawMode();
    testFullTxRingHoldsTdre();
    testTxIrqWhenPeerDropsWithFullRing();
    testCrNulOnlyAfterBareCr();
    testNoDcdIrqWhenTapHoldsTheLines();
    testKeyboardSinkOnlyInTextMode();
    testReconnectResetsTelnetParser();
    testBinaryTransitionsRespectByteOrder();
    testIrqDipGatesTheSlotLine();
    std::printf("OK\n");
    return 0;
}
