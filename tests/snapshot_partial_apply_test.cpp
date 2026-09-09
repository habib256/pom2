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

//
// snapshot_partial_apply_test — bug hunt #13.
//
// `card_snapshot_contract_test` asserts P1..P4 for every card in the
// CATALOG. The objects BELOW a card — the ones that travel nested inside a
// card blob or inside Memory's MEX trailer — had no such gate, and two of
// them broke the same two rules the cards are held to:
//
//   1. SmartPortBusDevice reaches a live state its own snapshot cannot
//      carry. `serveCommand`'s INIT arm recorded the packet's destination
//      byte as the unit's chain number WITHOUT excluding 0 — and 0 is the
//      host's own number on this wire, the value `unitFor` documents as
//      "never assigned" and refuses to match. `loadSnapshotState` then
//      reads `ids_[assigned_ - 1] == 0` as a truncated id table and
//      renumbers from scratch, so `assigned_` came back 0 where the capture
//      had 1 — and the restored chain answered the host's NEXT INIT scan
//      differently from the chain that was captured.
//
//   2. Two loaders applied part of a blob and then refused it, against
//      their own documented "false / 0 ⇒ nothing changed" contract:
//        * SmartPortBusDevice: `rx_` (and `reply_`) were assigned before
//          the reply framing could fail, so a blob truncated right after
//          the rx payload left the device holding the FILE's half-received
//          frame on an otherwise reset chain — the next host byte joined a
//          packet from another timeline.
//        * Scc8530Device: `wr9_`, the WR0 pointer bits and both 6-entry
//          interrupt vectors were written into the members before the
//          per-channel budget could refuse the blob, mixing file bytes with
//          two live channels.
//
// These are reachable from a FILE, not only from the rewind ring: the SCC
// rides in a WorkstationCard SLOTn blob and the bus device rides inside the
// //c external SmartPort port, which travels in MEX — and MEX is restored
// from `.pom2snap` files.

#include "Scc8530Device.h"
#include "SmartPortBusDevice.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Blob = std::vector<uint8_t>;

// The sender's encoding ($C800 in the Liron dump) — same helper as
// smartport_bus_device_test.cpp.
Blob frame(uint8_t dest, uint8_t type, const std::vector<uint8_t>& body)
{
    Blob w = { 0xFF, 0xFF, 0xC3 };
    const uint8_t odd = static_cast<uint8_t>(body.size() % 7);
    const uint8_t grp = static_cast<uint8_t>(body.size() / 7);
    const uint8_t hdr[7] = { dest, 0x00, type, 0x00, 0x00, odd, grp };
    uint8_t sum = 0;
    for (uint8_t h : hdr) { w.push_back(h | 0x80); sum ^= (h | 0x80); }
    auto section = [&](const uint8_t* p, int n) {
        uint8_t high = 0x80;
        for (int k = 0; k < n; ++k) if (p[k] & 0x80) high |= 0x40 >> k;
        w.push_back(high);
        for (int k = 0; k < n; ++k) { w.push_back(p[k] | 0x80); sum ^= p[k]; }
    };
    if (odd) section(body.data(), odd);
    for (int g = 0; g < grp; ++g) section(body.data() + odd + g * 7, 7);
    w.push_back(sum | 0xAA);
    w.push_back((sum >> 1) | 0xAA);
    w.push_back(0xC8);
    return w;
}

struct RamUnit final : pom2::SmartPortBusUnit {
    std::vector<uint8_t> blocks = std::vector<uint8_t>(512 * 4, 0);
    bool     hasMedia()       const override { return true; }
    uint32_t blockCount()     const override { return 4; }
    bool     writeProtected() const override { return false; }
    bool readBlock(uint32_t b, uint8_t out[512]) override
    { if (b >= 4) return false; std::memcpy(out, &blocks[b * 512], 512); return true; }
    bool writeBlock(uint32_t b, const uint8_t in[512]) override
    { if (b >= 4) return false; std::memcpy(&blocks[b * 512], in, 512); return true; }
};

RamUnit g_unit;

void sendFrame(pom2::SmartPortBusDevice& d, const Blob& w)
{
    d.reqChanged(true);
    for (uint8_t b : w) d.hostWrote(b);
    d.hostWrote(0x00);
    d.reqChanged(false);
}

Blob cap(const pom2::SmartPortBusDevice& d)
{ Blob b; d.appendSnapshotState(b); return b; }

// ── 1. An INIT addressed to device 0 must not take a chain number ────────
void testInitZeroIsRoundTrippable()
{
    pom2::SmartPortBusDevice a;
    a.setUnit(0, &g_unit);
    a.setUnitCount(1);
    sendFrame(a, frame(0x00, 0x80, { 0x05, 0, 1, 0, 0, 0, 0 }));   // INIT dest 0

    const Blob ba = cap(a);
    pom2::SmartPortBusDevice b;
    b.setUnit(0, &g_unit);
    b.setUnitCount(1);
    const std::size_t used = b.loadSnapshotState(ba.data(), ba.size());
    assert(used == ba.size() && "the section must report its true length");
    const Blob bb = cap(b);
    assert(bb == ba &&
           "an INIT to device 0 left a chain state the snapshot cannot carry");

    // …and the behavioural consequence: the host's next INIT scan must get
    // the same answer from both. Before the fix the captured chain had its
    // one slot taken (so it recorded nothing more) while the restored one
    // had assigned_ == 0 and took the number.
    sendFrame(a, frame(0x01, 0x80, { 0x05, 0, 1, 0, 0, 0, 0 }));
    sendFrame(b, frame(0x01, 0x80, { 0x05, 0, 1, 0, 0, 0, 0 }));
    assert(cap(a) == cap(b) &&
           "a rewind changed how the chain answers the host's INIT scan");
    std::printf("  ok: INIT to device 0 leaves a round-trippable chain\n");
}

// ── 2. A truncated bus blob leaves the device RESET, never half-applied ──
void testBusBlobTruncationLeavesNoResidue()
{
    pom2::SmartPortBusDevice src;
    src.setUnit(0, &g_unit);
    src.setUnitCount(1);
    // A frame left HALF received, so `rx_` in the blob is non-empty.
    const Blob wire = frame(0x01, 0x80, { 0x01, 0, 1, 0, 0, 0, 0 });
    src.reqChanged(true);
    for (std::size_t k = 0; k < 9 && k < wire.size(); ++k) src.hostWrote(wire[k]);
    const Blob ba = cap(src);
    // magic(4) + rxLen(2) + rx payload — the first offset past `rx_`.
    const std::size_t rxLen = static_cast<std::size_t>(ba[4]) |
                              (static_cast<std::size_t>(ba[5]) << 8);
    assert(rxLen > 0 && "the fixture must capture a half-received frame");
    const std::size_t afterRx = 4 + 2 + rxLen;

    // The reference: a driven device that has simply been bus-reset.
    pom2::SmartPortBusDevice ref;
    ref.setUnit(0, &g_unit);
    ref.setUnitCount(1);
    sendFrame(ref, frame(0x01, 0x80, { 0x05, 0, 1, 0, 0, 0, 0 }));
    ref.busReset();
    const Blob bRef = cap(ref);

    for (std::size_t n = afterRx; n < afterRx + 2 && n <= ba.size(); ++n) {
        pom2::SmartPortBusDevice t;
        t.setUnit(0, &g_unit);
        t.setUnitCount(1);
        sendFrame(t, frame(0x01, 0x80, { 0x05, 0, 1, 0, 0, 0, 0 }));
        assert(t.loadSnapshotState(ba.data(), n) == 0 &&
               "a torn blob must be refused");
        assert(cap(t) == bRef &&
               "a refused blob left the file's half-received frame behind");
    }
    std::printf("  ok: a bus blob torn after its rx payload leaves no residue\n");
}

// ── 3. The SCC applies its device header only after both channels parse ──
void testSccRefusalChangesNothing()
{
    pom2::Scc8530Device live;
    // Programme the chip so its device-wide header is off the reset values:
    // WR9 (device-wide) and the WR0 pointer bits both move.
    live.writeAbDc(2, 0x09);        // A control: point at WR9
    live.writeAbDc(2, 0x5A);        // …write it
    live.writeAbDc(0, 0x03);        // B control: point at WR3
    live.writeAbDc(0, 0xC1);
    live.tick(100000);
    Blob before;
    live.appendSnapshot(before);

    // A DIFFERENT chip, captured with a different device header.
    pom2::Scc8530Device other;
    other.writeAbDc(2, 0x09);
    other.writeAbDc(2, 0xA5);
    other.writeAbDc(0, 0x05);
    other.writeAbDc(0, 0x3E);
    other.tick(50000);
    Blob foreign;
    other.appendSnapshot(foreign);
    assert(foreign != before);

    // Every truncation from just past the device header (5 + 3 + 12 = 20)
    // to the end must be refused AND leave `live` untouched.
    for (std::size_t n = 20; n < foreign.size(); ++n) {
        assert(!live.restoreSnapshot(foreign.data(), n) &&
               "a truncated SCC blob must be refused");
        Blob after;
        live.appendSnapshot(after);
        assert(after == before &&
               "a refused SCC blob half-applied its device header");
    }
    // The full blob still restores.
    assert(live.restoreSnapshot(foreign.data(), foreign.size()));
    Blob back;
    live.appendSnapshot(back);
    assert(back == foreign && "the SCC blob no longer round-trips");
    std::printf("  ok: a refused SCC blob changes nothing; a whole one round-trips\n");
}

}  // namespace

int main()
{
    std::printf("snapshot_partial_apply_test\n");
    testInitZeroIsRoundTrippable();
    testBusBlobTruncationLeavesNoResidue();
    testSccRefusalChangesNothing();
    std::printf("snapshot_partial_apply_test: all ok\n");
    return 0;
}
