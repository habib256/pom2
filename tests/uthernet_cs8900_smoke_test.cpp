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

// Uthernet I / CS8900A smoke test — pins the chip model against MAME
// `src/devices/machine/cs8900a.cpp` (itself a VICE port) and the card
// wrapper against `src/devices/bus/a2bus/uthernet.cpp`.
//
// The card is driven exactly the way a 6502 driver would: through
// deviceSelectRead/Write on the slot's $C0nX window. The host side is a
// LoopbackNetworkBackend, so the test never touches a real network — a
// transmitted frame comes straight back as a received one.
//
// What this gates:
//
//   * Power-on register defaults — `cs8900a.cpp:286-342`. ProductID
//     reads back 0x0900630E (byte-reversed per the datasheet), and Self
//     Status has INITD set (0x0896) or drivers spin forever.
//   * PacketPage pointer semantics — `cs8900a.cpp:1399-1408`,
//     `:1249-1260`. Bits 0x3000 always read back set; bit 15 enables
//     auto-increment, which advances by ONE, not two.
//   * Register self-address enforcement — `cs8900a.cpp:1152-1175`. The
//     chip forces its own register number into the low 6 bits of every
//     control register write.
//   * Reserved ranges read 0x0300 — `cs8900a.cpp:1018-1103`.
//   * The four-step TX handshake — `cs8900a.cpp:839-904`, `:641-701`.
//     TxCMD → TxLength → observe Rdy4TxNOW in BusST → push bytes. A
//     write that skips a step must NOT emit a frame.
//   * TxLength bounds — `cs8900a.cpp:870-888`. < 4 is silently dropped;
//     > 1518 raises TxBidErr.
//   * The RX path — `cs8900a.cpp:938-980`, `:491-618`, `:703-765`.
//     Reading RxEvent stages a frame; RxStatus/RxLength/payload then read
//     out through RXTXDATA in the datasheet's H/L order.
//   * The address filter — `cs8900a.cpp:410-489`. A frame for somebody
//     else's MAC is dropped unless promiscuous mode is on.
//   * Snapshot round-trip through the SlotPeripheral hooks.

#include "NetworkBackend.h"
#include "UthernetCard.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// $C0nX offsets (`cs8900a.cpp:61-70`).
constexpr uint8_t kRxTxDataLo = 0x0;
constexpr uint8_t kRxTxDataHi = 0x1;
constexpr uint8_t kTxCmdLo    = 0x4;
constexpr uint8_t kTxCmdHi    = 0x5;
constexpr uint8_t kTxLenLo    = 0x6;
constexpr uint8_t kTxLenHi    = 0x7;
constexpr uint8_t kPpPtrLo    = 0xA;
constexpr uint8_t kPpPtrHi    = 0xB;
constexpr uint8_t kPpDataLo   = 0xC;
constexpr uint8_t kPpDataHi   = 0xD;

// PacketPage addresses used below.
constexpr uint16_t kPpProductId = 0x0000;
constexpr uint16_t kPpCcRxCtl   = 0x0104;
constexpr uint16_t kPpCcLineCtl = 0x0112;
constexpr uint16_t kPpSeRxEvent = 0x0124;
constexpr uint16_t kPpSeSelfSt  = 0x0136;
constexpr uint16_t kPpSeBusSt   = 0x0138;
constexpr uint16_t kPpMacAddr   = 0x0158;

// Auto-increment off: point the window at `addr` and leave it there.
void setPpPointer(pom2::UthernetCard& card, uint16_t addr)
{
    card.deviceSelectWrite(kPpPtrLo, static_cast<uint8_t>(addr & 0xFF));
    card.deviceSelectWrite(kPpPtrHi, static_cast<uint8_t>((addr >> 8) & 0xFF));
}

uint16_t readPpWord(pom2::UthernetCard& card, uint16_t addr)
{
    setPpPointer(card, addr);
    const uint8_t lo = card.deviceSelectRead(kPpDataLo);
    const uint8_t hi = card.deviceSelectRead(kPpDataHi);
    return static_cast<uint16_t>(lo | (hi << 8));
}

// Low byte first, then high — the order every CS8900A driver uses, and
// the one the odd-address side effects are written against.
void writePpWord(pom2::UthernetCard& card, uint16_t addr, uint16_t value)
{
    setPpPointer(card, addr);
    card.deviceSelectWrite(kPpDataLo, static_cast<uint8_t>(value & 0xFF));
    card.deviceSelectWrite(kPpDataHi, static_cast<uint8_t>((value >> 8) & 0xFF));
}

const std::array<uint8_t, 6> kOurMac = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

void programMac(pom2::UthernetCard& card, const std::array<uint8_t, 6>& mac)
{
    for (int w = 0; w < 3; ++w) {
        const uint16_t word = static_cast<uint16_t>(
            mac[static_cast<size_t>(w * 2)] |
            (static_cast<uint16_t>(mac[static_cast<size_t>(w * 2 + 1)]) << 8));
        writePpWord(card, static_cast<uint16_t>(kPpMacAddr + w * 2), word);
    }
}

/// Enable the transmitter (bit 7) and receiver (bit 6). The low 6 bits
/// must be LineCtl's own register number 0x13 — the chip forces it
/// anyway (`cs8900a.cpp:1159-1165`), which the next test asserts.
void enableTxRx(pom2::UthernetCard& card)
{
    writePpWord(card, kPpCcLineCtl, 0x00D3);
}

/// RxCtl: accept correct frames (0x0100) addressed to our IA (0x0400).
void acceptOwnMac(pom2::UthernetCard& card)
{
    writePpWord(card, kPpCcRxCtl, 0x0505);
}

/// RxCtl: promiscuous (0x0080) on top of the above.
void acceptEverything(pom2::UthernetCard& card)
{
    writePpWord(card, kPpCcRxCtl, 0x0585);
}

/// Run the documented four-step transmit handshake and push `frame`.
/// Returns without pushing anything if TxLength was rejected.
void transmitFrame(pom2::UthernetCard& card, const std::vector<uint8_t>& frame)
{
    // Step 1: TxCMD. 0x00C9 = "transmit after all bytes are in the
    // buffer" + the register's own number (0x09).
    writePpWord(card, 0x0144, 0x00C9);
    // Step 2: TxLength.
    writePpWord(card, 0x0146, static_cast<uint16_t>(frame.size()));
    // Step 3: observe Rdy4TxNOW. The side effect fires on the ODD
    // (high-byte) read of BusST (`cs8900a.cpp:982-1005`).
    setPpPointer(card, kPpSeBusSt);
    (void)card.deviceSelectRead(kPpDataLo);
    (void)card.deviceSelectRead(kPpDataHi);
    // Step 4: push the payload, low byte then high byte per word.
    for (size_t i = 0; i < frame.size(); ++i) {
        card.deviceSelectWrite((i & 1) ? kRxTxDataHi : kRxTxDataLo, frame[i]);
    }
}

std::vector<uint8_t> makeFrame(const std::array<uint8_t, 6>& dest,
                               const std::array<uint8_t, 6>& source,
                               size_t totalLen, uint8_t payloadSeed)
{
    std::vector<uint8_t> f(totalLen, 0);
    std::memcpy(f.data(), dest.data(), 6);
    std::memcpy(f.data() + 6, source.data(), 6);
    f[12] = 0x08; f[13] = 0x00;          // EtherType IPv4
    for (size_t i = 14; i < totalLen; ++i)
        f[i] = static_cast<uint8_t>(payloadSeed + i);
    return f;
}

// ── Tests ─────────────────────────────────────────────────────────────

void testPowerOnDefaults()
{
    pom2::UthernetCard card(3);

    // ProductID is stored byte-reversed: 0x0900630E → low word 0x630E.
    assert(readPpWord(card, kPpProductId) == 0x630E);
    assert(readPpWord(card, kPpProductId + 2) == 0x0900);

    // INITD (bit 7) must be set or drivers wait forever for the chip.
    assert(readPpWord(card, kPpSeSelfSt) == 0x0896);

    // Reserved bus-interface range reads 0x0300 on real hardware.
    assert(readPpWord(card, 0x0010) == 0x0300);
    // Reserved control register (0x11).
    assert(readPpWord(card, 0x0110) == 0x0300);

    // Nothing is enabled out of reset.
    assert(!card.chip().receiverEnabled());
    assert(!card.chip().transmitterEnabled());

    std::printf("  power-on defaults OK\n");
}

void testPacketPagePointer()
{
    pom2::UthernetCard card(3);

    // Bits 0x3000 always read back set (`cs8900a.cpp:1399-1408`).
    setPpPointer(card, 0x0000);
    assert(card.chip().packetPagePointer() == 0x3000);

    // Auto-increment advances by ONE per access, not two.
    card.deviceSelectWrite(kPpPtrLo, 0x00);
    card.deviceSelectWrite(kPpPtrHi, 0x80);   // 0x8000 → | 0x3000 = 0xB000
    assert(card.chip().packetPagePointer() == 0xB000);
    (void)card.deviceSelectRead(kPpDataLo);
    assert(card.chip().packetPagePointer() == 0xB001);
    (void)card.deviceSelectRead(kPpDataHi);
    assert(card.chip().packetPagePointer() == 0xB002);

    std::printf("  PacketPage pointer OK\n");
}

void testRegisterSelfAddress()
{
    pom2::UthernetCard card(3);

    // Write LineCtl with a deliberately wrong register number in the low
    // 6 bits; the chip must rewrite them to 0x13.
    writePpWord(card, kPpCcLineCtl, 0x00C0);
    assert((readPpWord(card, kPpCcLineCtl) & 0x3F) == 0x13);
    // ...and the enable bits survived the fixup.
    assert(card.chip().transmitterEnabled());
    assert(card.chip().receiverEnabled());

    // Status registers are read-only (`cs8900a.cpp:1177-1182`).
    const uint16_t before = readPpWord(card, kPpSeSelfSt);
    writePpWord(card, kPpSeSelfSt, 0x0000);
    assert(readPpWord(card, kPpSeSelfSt) == before);

    std::printf("  register self-address + read-only OK\n");
}

void testTransmitHandshake()
{
    pom2::UthernetCard card(3);
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    auto* raw = backend.get();
    card.setBackend(std::move(backend));

    programMac(card, kOurMac);
    enableTxRx(card);

    const std::vector<uint8_t> frame = makeFrame(kOurMac, kOurMac, 64, 0x10);

    // Skipping the BusST step must NOT emit anything: without
    // Rdy4TxNOW observed, txState never reaches READ_BUSST and every
    // RXTXDATA write is discarded (`cs8900a.cpp:644-651`).
    writePpWord(card, 0x0144, 0x00C9);
    writePpWord(card, 0x0146, static_cast<uint16_t>(frame.size()));
    for (size_t i = 0; i < frame.size(); ++i)
        card.deviceSelectWrite((i & 1) ? kRxTxDataHi : kRxTxDataLo, frame[i]);
    assert(raw->queued() == 0);

    // The full handshake does emit.
    transmitFrame(card, frame);
    assert(raw->queued() == 1);

    // And the bytes on the wire are exactly what was pushed.
    std::vector<uint8_t> out(pom2::kMaxEthFrame);
    const int n = raw->receive(out.data(), static_cast<int>(out.size()));
    assert(n == static_cast<int>(frame.size()));
    assert(std::memcmp(out.data(), frame.data(), frame.size()) == 0);

    std::printf("  TX handshake OK (%d bytes on the wire)\n", n);
}

void testTransmitLengthBounds()
{
    pom2::UthernetCard card(3);
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    auto* raw = backend.get();
    card.setBackend(std::move(backend));
    enableTxRx(card);

    // < 4 bytes: rejected, and the chip reports "space available, no
    // error" (`cs8900a.cpp:870-878`).
    writePpWord(card, 0x0144, 0x00C9);
    writePpWord(card, 0x0146, 0x0002);
    uint16_t busSt = readPpWord(card, kPpSeBusSt);
    assert((busSt & 0x100) == 0x100);   // Rdy4TxNOW
    assert((busSt & 0x080) == 0);       // no TxBidErr

    // > 1518: rejected WITH TxBidErr (`cs8900a.cpp:879-888`).
    writePpWord(card, 0x0144, 0x00C9);
    writePpWord(card, 0x0146, 0x0800);   // 2048
    busSt = readPpWord(card, kPpSeBusSt);
    assert((busSt & 0x080) == 0x080);   // TxBidErr
    assert((busSt & 0x100) == 0);       // not ready

    assert(raw->queued() == 0);
    std::printf("  TX length bounds OK\n");
}

void testReceivePath()
{
    pom2::UthernetCard card(3);
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    auto* raw = backend.get();
    card.setBackend(std::move(backend));

    programMac(card, kOurMac);
    enableTxRx(card);
    acceptOwnMac(card);

    const std::vector<uint8_t> frame = makeFrame(kOurMac, kOurMac, 64, 0xA0);
    raw->transmit(frame.data(), static_cast<int>(frame.size()));

    // The card services the backend on its cycle hook.
    card.advanceCycles(pom2::UthernetCard::kPollIntervalCycles);
    assert(card.chip().queuedFrames() == 1);

    // Reading RxEvent stages the frame (`cs8900a.cpp:942-980`).
    setPpPointer(card, kPpSeRxEvent);
    const uint8_t evLo = card.deviceSelectRead(kPpDataLo);
    const uint8_t evHi = card.deviceSelectRead(kPpDataHi);
    const uint16_t rxEvent = static_cast<uint16_t>(evLo | (evHi << 8));
    assert((rxEvent & 0x0100) != 0);   // RxOK
    assert((rxEvent & 0x0400) != 0);   // IA match
    assert(card.chip().queuedFrames() == 0);

    // Drain through RXTXDATA in the datasheet's order: RxStatus H then L,
    // RxLength H then L, then payload L/H per word.
    const uint8_t statusHi = card.deviceSelectRead(kRxTxDataHi);
    const uint8_t statusLo = card.deviceSelectRead(kRxTxDataLo);
    const uint8_t lenHi    = card.deviceSelectRead(kRxTxDataHi);
    const uint8_t lenLo    = card.deviceSelectRead(kRxTxDataLo);
    const uint16_t status = static_cast<uint16_t>(statusLo | (statusHi << 8));
    const uint16_t length = static_cast<uint16_t>(lenLo | (lenHi << 8));
    assert(status == rxEvent);
    assert(length == frame.size());

    std::vector<uint8_t> got;
    got.reserve(length);
    for (uint16_t i = 0; i < length; ++i)
        got.push_back(card.deviceSelectRead((i & 1) ? kRxTxDataHi : kRxTxDataLo));
    assert(std::memcmp(got.data(), frame.data(), frame.size()) == 0);

    std::printf("  RX path OK (%u bytes recovered)\n", length);
}

void testAddressFilter()
{
    pom2::UthernetCard card(3);
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    auto* raw = backend.get();
    card.setBackend(std::move(backend));

    programMac(card, kOurMac);
    enableTxRx(card);
    acceptOwnMac(card);

    // A frame for somebody else must not queue at all — the pre-filter in
    // pumpBackend mirrors MAME's recv_start_cb (`cs8900a.cpp:1496`).
    const std::array<uint8_t, 6> otherMac = { 0x02, 0x99, 0x88, 0x77, 0x66, 0x55 };
    const std::vector<uint8_t> stranger = makeFrame(otherMac, otherMac, 64, 0x30);
    raw->transmit(stranger.data(), static_cast<int>(stranger.size()));
    card.advanceCycles(pom2::UthernetCard::kPollIntervalCycles);
    assert(card.chip().queuedFrames() == 0);
    assert(card.chip().framesFiltered() == 1);

    // Broadcast is accepted only with the broadcast bit set — which
    // acceptOwnMac() did not set.
    const std::array<uint8_t, 6> bcast = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const std::vector<uint8_t> broadcast = makeFrame(bcast, otherMac, 64, 0x40);
    raw->transmit(broadcast.data(), static_cast<int>(broadcast.size()));
    card.advanceCycles(pom2::UthernetCard::kPollIntervalCycles);
    assert(card.chip().queuedFrames() == 0);

    // Promiscuous takes everything.
    acceptEverything(card);
    assert(card.chip().promiscuous());
    raw->transmit(stranger.data(), static_cast<int>(stranger.size()));
    card.advanceCycles(pom2::UthernetCard::kPollIntervalCycles);
    assert(card.chip().queuedFrames() == 1);

    std::printf("  address filter OK\n");
}

void testSnapshotRoundTrip()
{
    pom2::UthernetCard card(3);
    card.setBackend(std::make_unique<pom2::LoopbackNetworkBackend>());

    programMac(card, kOurMac);
    enableTxRx(card);
    acceptEverything(card);
    setPpPointer(card, 0x0412);

    std::vector<uint8_t> blob;
    card.appendSnapshotState(blob);
    assert(!blob.empty());

    // A fresh card is nothing like the configured one...
    pom2::UthernetCard restored(3);
    assert(!restored.chip().transmitterEnabled());

    restored.loadSnapshotState(blob.data(), blob.size());
    assert(restored.chip().transmitterEnabled());
    assert(restored.chip().receiverEnabled());
    assert(restored.chip().promiscuous());
    assert(restored.chip().packetPagePointer() == card.chip().packetPagePointer());
    assert(restored.chip().macAddress() == card.chip().macAddress());

    // A blob from a different card in that slot must be ignored, not
    // misparsed (SlotPeripheral.h contract).
    std::vector<uint8_t> foreign(blob.size(), 0xAB);
    pom2::UthernetCard untouched(3);
    untouched.loadSnapshotState(foreign.data(), foreign.size());
    assert(!untouched.chip().transmitterEnabled());

    std::printf("  snapshot round-trip OK (%zu bytes)\n", blob.size());
}

// A snapshot is a FILE. A corrupt or hand-edited one must leave the transmit
// handshake COHERENT, because `transmitByte` releases the frame on
// `txCount_ == txLength_` exactly: restore a count ABOVE the length and the
// equality never comes round, so every byte the driver pushes for the frame
// it was in the middle of disappears and it waits for a completion that can
// never arrive. Same for a state byte outside the four-step enum — it is
// compared for equality at each step, so a fifth value simply never advances.
void testSnapshotCorruptTxStateIsClamped()
{
    pom2::UthernetCard card(3);
    card.setBackend(std::make_unique<pom2::LoopbackNetworkBackend>());
    programMac(card, kOurMac);
    enableTxRx(card);

    std::vector<uint8_t> blob;
    card.appendSnapshotState(blob);

    // Field offsets inside the blob. The CARD writes its own magic(4) +
    // version(2) first, then the CHIP writes magic(4), version(2), mac(6),
    // hash(8), I/O regs, PacketPage, ppPtr(2), recvControl(2), flags(1), and
    // then txBuffer/rxBuffer/txCount/rxCount/txLength/rxLength (2 each)
    // followed by txState/rxState (1 each).
    const size_t base = 6 +                       // the card's own header
                        4 + 2 + 6 + 8 +
                        pom2::Cs8900aDevice::kIoRegisterCount +
                        pom2::Cs8900aDevice::kPacketPageSize + 2 + 2 + 1;
    const size_t txCountAt  = base + 4;
    const size_t txLengthAt = base + 8;
    const size_t txStateAt  = base + 12;
    const size_t rxStateAt  = base + 13;
    // Everything after the states: rxEventReadMask(2) + three counters (8
    // each). Asserted so a layout change fails HERE rather than silently
    // corrupting some other field and passing.
    assert(blob.size() == base + 14 + 2 + 24 &&
           "snapshot layout moved — fix the offsets above");

    auto putU16At = [&blob](size_t at, uint16_t v) {
        blob[at]     = static_cast<uint8_t>(v & 0xFF);
        blob[at + 1] = static_cast<uint8_t>(v >> 8);
    };
    putU16At(txLengthAt, 64);
    putU16At(txCountAt, 1000);        // > txLength: unreachable equality
    blob[txStateAt] = 3;              // READ_BUSST — "pushing payload now"
    blob[rxStateAt] = 0x7F;           // outside the two-value enum

    pom2::UthernetCard restored(3);
    restored.setBackend(std::make_unique<pom2::LoopbackNetworkBackend>());
    restored.loadSnapshotState(blob.data(), blob.size());

    assert(restored.chip().txCount() <= restored.chip().txLength());
    assert(restored.chip().txState() == 0 && "an impossible count restores to idle");
    assert(restored.chip().rxState() <= 1);

    // And the card still transmits: the restore left a startable machine, not
    // a wedged one.
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    auto* raw = backend.get();
    restored.setBackend(std::move(backend));
    const std::vector<uint8_t> frame = makeFrame(kOurMac, kOurMac, 64, 0x20);
    transmitFrame(restored, frame);
    assert(raw->queued() == 1);

    // A txState outside the enum is refused the same way.
    blob[txStateAt] = 9;
    putU16At(txCountAt, 0);
    pom2::UthernetCard restored2(3);
    restored2.loadSnapshotState(blob.data(), blob.size());
    assert(restored2.chip().txState() == 0);

    std::printf("  corrupt transmit state restores coherent\n");
}

// The chip keeps its programmed IA across a bus reset — MAME
// `cs8900a.cpp` device_reset does not touch it, and the uthernet.cpp
// shim does not reprogram it. UthernetCard::onReset used to re-stamp
// kDefaultMac, silently reverting the guest's address on every
// Ctrl-Reset (bug-hunt 2026-07-28).
void testMacSurvivesCardReset()
{
    pom2::UthernetCard card(3);
    programMac(card, kOurMac);
    assert(card.chip().macAddress() == kOurMac);

    card.onReset();
    assert(card.chip().macAddress() == kOurMac);

    std::printf("  programmed MAC survives a bus reset\n");
}

// ── Bug-hunt pins (2026-09-06) ────────────────────────────────────────

constexpr uint16_t kPpSeIsq      = 0x0120;
constexpr uint16_t kPpSeTxEvent  = 0x0128;
constexpr uint16_t kPpSeBufEvent = 0x012C;
constexpr uint16_t kPpSeRxMiss   = 0x0130;

// The Interrupt Status Queue must actually report events.
//
// $C0n8/9 is the whole interrupt-driven idiom on a card whose INTn the
// Uthernet I never wired: a driver reads the ISQ in a loop until it comes
// back 0 and acts on what it got. It was hard-wired to 0, so the loop exited
// immediately every time and no frame was ever noticed — the driver's own
// RxEvent read is what pops one, and it never got that far.
// Datasheet §4.4.1.
void testInterruptStatusQueue()
{
    pom2::UthernetCard card(3);
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    auto* raw = backend.get();
    card.setBackend(std::move(backend));

    programMac(card, kOurMac);
    enableTxRx(card);
    acceptOwnMac(card);

    // An idle chip's queue is empty — "read until 0" must still terminate.
    assert(readPpWord(card, kPpSeIsq) == 0x0000);

    // A received frame surfaces as RxEvent, with RxOK and the IA match.
    const std::vector<uint8_t> frame = makeFrame(kOurMac, kOurMac, 64, 0xA0);
    raw->transmit(frame.data(), static_cast<int>(frame.size()));
    card.advanceCycles(pom2::UthernetCard::kPollIntervalCycles);
    assert(card.chip().queuedFrames() == 1);

    const uint16_t isq = readPpWord(card, kPpSeIsq);
    assert((isq & 0x003F) == 0x04);    // RxEvent's own register number
    assert(isq & 0x0100);              // RxOK
    assert(isq & 0x0400);              // IA match
    assert(card.chip().queuedFrames() == 0);
    // ...and the frame is staged, not skipped: the payload is still there.
    assert(card.chip().rxState() == 1);

    // Nothing else pending once it has been reported.
    assert(readPpWord(card, kPpSeIsq) == 0x0000);

    std::printf("  ISQ reports RxEvent (was hard-wired to 0)\n");
}

// A completed transmit must set TxOK. A driver that waits for it before
// staging the next frame stalled after its first packet, forever.
// Datasheet §4.4.15.
void testTxEventReportsTxOk()
{
    pom2::UthernetCard card(3);
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    card.setBackend(std::move(backend));

    programMac(card, kOurMac);
    enableTxRx(card);
    assert((readPpWord(card, kPpSeTxEvent) & 0xFFC0) == 0);

    transmitFrame(card, makeFrame(kOurMac, kOurMac, 64, 0x10));
    assert(card.chip().framesSent() == 1);
    assert(readPpWord(card, kPpSeTxEvent) & 0x0100);      // TxOK

    // It reaches the driver through the ISQ, which clears it.
    const uint16_t isq = readPpWord(card, kPpSeIsq);
    assert((isq & 0x003F) == 0x08 && (isq & 0x0100));     // TxEvent + TxOK
    assert(readPpWord(card, kPpSeIsq) == 0x0000);

    std::printf("  TxEvent reports TxOK on release\n");
}

// The inbound queue is bounded in BYTES, drops the frame ARRIVING, and says
// so in RxMISS + BufEvent.
//
// The chip holds its receive frames in 4 KB of on-chip buffer (datasheet
// §3.2) and counts what it could not take in RxMISS (§4.4.20). A 4096-ENTRY
// queue that dropped the OLDEST was ~6 MB of host memory and minutes of
// backlog, and it replayed packets whose senders had long given up.
void testRxQueueIsByteBoundedAndCountsMisses()
{
    pom2::UthernetCard card(3);
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    auto* raw = backend.get();
    card.setBackend(std::move(backend));

    programMac(card, kOurMac);
    enableTxRx(card);
    acceptOwnMac(card);

    // Six MTU-ish frames is well past 4 KB. Each one is distinguishable by
    // its payload seed, so "which ones survived" is checkable.
    constexpr int kFrames = 6;
    constexpr size_t kLen = 1024;
    for (int i = 0; i < kFrames; ++i) {
        const auto f = makeFrame(kOurMac, kOurMac, kLen,
                                 static_cast<uint8_t>(0x10 * (i + 1)));
        raw->transmit(f.data(), static_cast<int>(f.size()));
        card.advanceCycles(pom2::UthernetCard::kPollIntervalCycles);
    }

    // Four fit in 4 KB; the rest were missed, not silently substituted.
    assert(card.chip().queuedFrames() == 4);
    assert(card.chip().framesMissed() == static_cast<uint64_t>(kFrames - 4));

    // RxMISS carries the count in bits 6-15 with its own register number in
    // the low 6 (§4.4.20), and BufEvent flags that it moved (§4.4.17).
    const uint16_t miss = readPpWord(card, kPpSeRxMiss);
    assert((miss & 0x003F) == 0x10);
    assert((miss >> 6) == (kFrames - 4));
    assert(readPpWord(card, kPpSeBufEvent) & 0x0200);   // RxMiss

    // The FIRST frame is the one still at the head — the oldest survived,
    // the newest were dropped. Reading RxEvent pops it.
    setPpPointer(card, kPpSeRxEvent);
    (void)card.deviceSelectRead(kPpDataLo);
    (void)card.deviceSelectRead(kPpDataHi);
    (void)card.deviceSelectRead(kRxTxDataHi);   // RxStatus H
    (void)card.deviceSelectRead(kRxTxDataLo);   // RxStatus L
    (void)card.deviceSelectRead(kRxTxDataHi);   // RxLength H
    (void)card.deviceSelectRead(kRxTxDataLo);   // RxLength L
    std::vector<uint8_t> got;
    for (size_t i = 0; i < kLen; ++i)
        got.push_back(card.deviceSelectRead((i & 1) ? kRxTxDataHi : kRxTxDataLo));
    assert(got[14] == static_cast<uint8_t>(0x10 + 14));   // seed 0x10 = frame 0

    std::printf("  RX queue byte-bounded, drops newest, counts RxMISS\n");
}

// A reset must re-decode the receive filter, not just reload the register it
// decodes from. The booleans survived a reset carrying the PREVIOUS
// configuration, and the write path only re-decodes when CC_RXCTL CHANGES —
// so a card left promiscuous by one driver stayed promiscuous for the next.
void testResetClearsTheDecodedFilter()
{
    pom2::UthernetCard card(3);
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    auto* raw = backend.get();
    card.setBackend(std::move(backend));

    programMac(card, kOurMac);
    enableTxRx(card);
    acceptEverything(card);
    assert(card.chip().promiscuous());

    card.onReset();
    assert(!card.chip().promiscuous());

    // And it behaves that way: a frame for somebody else is filtered out
    // even though CC_RXCTL is back at its power-on value.
    enableTxRx(card);
    const std::array<uint8_t, 6> other = { 0x02, 0x99, 0x99, 0x99, 0x99, 0x99 };
    const auto f = makeFrame(other, kOurMac, 64, 0x77);
    raw->transmit(f.data(), static_cast<int>(f.size()));
    card.advanceCycles(pom2::UthernetCard::kPollIntervalCycles);
    assert(card.chip().queuedFrames() == 0);

    std::printf("  reset re-decodes the RX filter\n");
}

} // namespace

int main()
{
    std::printf("Uthernet I / CS8900A smoke test\n");
    testPowerOnDefaults();
    testPacketPagePointer();
    testRegisterSelfAddress();
    testTransmitHandshake();
    testTransmitLengthBounds();
    testReceivePath();
    testAddressFilter();
    testSnapshotRoundTrip();
    testSnapshotCorruptTxStateIsClamped();
    testMacSurvivesCardReset();
    testInterruptStatusQueue();
    testTxEventReportsTxOk();
    testRxQueueIsByteBoundedAndCountsMisses();
    testResetClearsTheDecodedFilter();
    std::printf("PASS\n");
    return 0;
}
