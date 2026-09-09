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

// Super Serial Card — the telnet CONSOLE seam, and two 6551 receiver rules
// the ACIA suite does not reach. Four defects found by the 2026-09-09 I/O
// bug hunt, each pinned here.
//
//  1. The telnet -> keyboard bridge must deliver CONTROL bytes. The sink in
//     MainWindow_SlotConfig.cpp / pom2_headless.cpp used Memory::pasteText,
//     whose filter drops everything below $20 except CR and HT — a CLIPBOARD
//     policy. A terminal is the opposite case, and through that filter a
//     telnet user could type but never interrupt (Ctrl-C), correct ($08, the
//     Apple II's own left-arrow) or escape ($1B). Memory::pasteKeyStream is
//     the terminal entry point; it keeps the FIFO, the cap and the ][/][+
//     case-fold, and drops nothing.
//
//  2. ENTER must land as ONE carriage return once the peer negotiates telnet
//     BINARY. BINARY switches off the card's own NVT filter (that is the
//     point of BINARY — the ACIA stays 8-bit clean), so the CR LF reaches the
//     sink intact; with a per-call CR state the LF became a second CR and
//     every line was submitted twice. pasteKeyStream's CR state is a member.
//
//  3. A new telnet client must not be answered with the PREVIOUS client's
//     unread typing. A real 6551 has a one-byte RDR: bytes the guest never
//     read are gone when the carrier drops. POM2's 4 KB host ring kept them.
//
//  4. RDR is a LATCH. MAME `mos6551.cpp::read_rdr()` clears the status bits
//     and then `return m_rdr;` — reading $C0n8 twice yields the same byte.
//     POM2 returned $00 on the second read, i.e. a NUL the peer never sent.

#include "Memory.h"
#include "SuperSerialCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr uint8_t kRdrAddr = 0x8;   // $C0n8

// The production sink, verbatim (MainWindow_SlotConfig.cpp / pom2_headless).
void wireKeyboardSink(SuperSerialCard& ssc, Memory& mem)
{
    ssc.setKeyboardSink([&mem](uint8_t b) {
        const char buf[1] = { static_cast<char>(b) };
        mem.pasteKeyStream(buf, 1);
    });
}

// Push a chunk the way the TCP worker does: text filter, then deliver.
void feedTelnet(SuperSerialCard& ssc, std::vector<uint8_t> chunk)
{
    const size_t n = ssc.processTransportTextRx(chunk.data(), chunk.size());
    ssc.deliverTransportBytes(chunk.data(), n, /*textMode=*/true);
}

// The Monitor's KEYIN idiom: poll $C000, take the byte, ack at $C010.
std::string drainKeyboard(Memory& mem, int polls)
{
    std::string got;
    for (int i = 0; i < polls; ++i) {
        const uint8_t v = mem.memRead(0xC000);
        if (v & 0x80) {
            got.push_back(static_cast<char>(v & 0x7F));
            mem.memRead(0xC010);
        }
    }
    return got;
}

// ── 1 : control bytes reach $C000 ─────────────────────────────────────────
void testControlBytesReachTheKeyboard()
{
    Memory mem;
    mem.setIIEMode(true);
    SuperSerialCard ssc(2);
    wireKeyboardSink(ssc, mem);
    ssc.onTransportConnected();

    // What a telnet client sends for:  A Ctrl-C B <left-arrow> C ESC [ Ctrl-D
    // Ctrl-X ENTER.  ENTER is CR LF on the wire (RFC 854).
    feedTelnet(ssc, { 'A', 0x03, 'B', 0x08, 'C', 0x1B, '[', 0x04, 0x18,
                      '\r', '\n' });

    const std::string expected = "A\x03" "B\x08" "C\x1B[\x04\x18\r";

    // The ACIA (a real IN#2) has always seen every byte.
    std::string acia;
    while (ssc.rxQueueDepth())
        acia.push_back(static_cast<char>(ssc.deviceSelectRead(kRdrAddr)));
    assert(acia == expected);

    // The keyboard bridge must see the same stream.
    assert(drainKeyboard(mem, 64) == expected);
}

// ── 2 : ENTER is one CR, with and without telnet BINARY ───────────────────
void testEnterIsOneCarriageReturn()
{
    for (bool binary : { false, true }) {
        Memory mem;
        mem.setIIEMode(true);
        SuperSerialCard ssc(2);
        wireKeyboardSink(ssc, mem);
        ssc.onTransportConnected();

        if (binary) {
            // `telnet -8`: the peer offers and requests BINARY both ways.
            feedTelnet(ssc, { 0xFF, 0xFB, 0x00,     // IAC WILL BINARY
                              0xFF, 0xFD, 0x00 });  // IAC DO   BINARY
        }
        feedTelnet(ssc, { 'R', 'U', 'N', '\r', '\n' });

        // Exactly one CR, whichever mode the peer negotiated.
        assert(drainKeyboard(mem, 64) == "RUN\r");
    }
}

// A CR LF split across two recv() chunks must still collapse — the sink is
// fed one byte at a time, so this is the same state, exercised end to end.
void testSplitCrLfCollapses()
{
    Memory mem;
    mem.setIIEMode(true);
    SuperSerialCard ssc(2);
    wireKeyboardSink(ssc, mem);
    ssc.onTransportConnected();
    feedTelnet(ssc, { 0xFF, 0xFB, 0x00, 0xFF, 0xFD, 0x00 });   // BINARY
    feedTelnet(ssc, { 'H', 'I', '\r' });
    feedTelnet(ssc, { '\n', 'X' });
    assert(drainKeyboard(mem, 64) == "HI\rX");
}

// ── 3 : a reconnect does not deliver the dead peer's bytes ────────────────
void testReconnectDropsUnreadRx()
{
    SuperSerialCard ssc(2);
    ssc.onTransportConnected();
    const uint8_t stale[] = { 'O', 'L', 'D' };
    ssc.deliverRxBytes(stale, sizeof stale);
    assert(ssc.rxQueueDepth() == 3);

    ssc.onTransportDisconnected();      // the peer hangs up, bytes unread
    ssc.onTransportConnected();         // a different peer dials in
    assert(ssc.rxQueueDepth() == 0);

    // The TX ring is deliberately NOT cleared: those bytes are the guest's,
    // and it has already been told they went out (TDRE is pinned high).
    SuperSerialCard tx(2);
    tx.deviceSelectWrite(0xA, 0x01);    // DTR on
    tx.deviceSelectWrite(0x8, 'Z');     // guest writes with nobody connected
    assert(tx.txQueueDepth() == 1);
    tx.onTransportConnected();
    assert(tx.txQueueDepth() == 1);
}

// ── 4 : RDR is a latch (MAME mos6551.cpp read_rdr) ────────────────────────
void testRdrIsALatch()
{
    SuperSerialCard ssc(2);
    ssc.onTransportConnected();
    const uint8_t one[] = { 0x5A };
    ssc.deliverRxBytes(one, 1);

    assert(ssc.deviceSelectRead(kRdrAddr) == 0x5A);
    assert(ssc.rxQueueDepth() == 0);
    // Re-reading an empty receiver hands back the same byte, not $00.
    assert(ssc.deviceSelectRead(kRdrAddr) == 0x5A);
    assert(ssc.deviceSelectRead(kRdrAddr) == 0x5A);

    // A fresh byte replaces it.
    const uint8_t two[] = { 0x41 };
    ssc.deliverRxBytes(two, 1);
    assert(ssc.deviceSelectRead(kRdrAddr) == 0x41);
    assert(ssc.deviceSelectRead(kRdrAddr) == 0x41);

    // A hardware reset clears the register.
    ssc.onReset();
    assert(ssc.deviceSelectRead(kRdrAddr) == 0x00);
}

// A clipboard paste keeps its filter — the fix must not widen pasteText.
void testClipboardPasteStillFilters()
{
    Memory mem;
    mem.setIIEMode(true);
    const char clip[] = { 'A', 0x03, 'B', 0x1B, 'C' };
    mem.pasteText(clip, sizeof clip);
    assert(drainKeyboard(mem, 32) == "ABC");
}

}  // namespace

int main()
{
    testControlBytesReachTheKeyboard();
    testEnterIsOneCarriageReturn();
    testSplitCrLfCollapses();
    testReconnectDropsUnreadRx();
    testRdrIsALatch();
    testClipboardPasteStillFilters();
    std::printf("ssc_keyboard_bridge: OK\n");
    return 0;
}
