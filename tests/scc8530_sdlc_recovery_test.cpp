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

// Bug hunt #17 — the two SDLC seams a LocalTalk frame actually crosses.
//
//  1. **WR0 Send Abort left the transmit buffer loaded.** The command means
//     "flush the transmitter", and POM2 flushed the frame and the shift
//     register but not the one-byte buffer behind them: RR0 D2 (Tx Buffer
//     Empty) stayed CLEAR with the aborted frame's next byte still in the
//     slot. `data_write`'s full test on a one-slot FIFO is exactly !TBE, so
//     the first byte of the RETRANSMISSION was discarded and the stale byte
//     was loaded in its place. A LocalTalk frame resent after a collision
//     therefore opened with a byte from the frame that collided.
//
//  2. **`receiveFrame` dropped everything past the second byte, in silence.**
//     The 3-slot receive FIFO signals full one slot early (MAME
//     `receive_data`, z80scc.cpp:2566), so it holds two bytes — and
//     `receiveFrame` pushed the whole frame in with no chance for the driver
//     to drain. The overrun bit landed on the slot the write pointer never
//     left and End Of Frame was marked there too, so NEITHER reached RR1: a
//     3-byte LLAP control frame arrived as two bytes, with no error and no
//     end of frame, and a 603-byte data frame lost 601 of them. The tail is
//     now held and fed as the reader makes room, which is what the wire does.

#include "Scc8530Device.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using pom2::Scc8530Device;
constexpr int A = Scc8530Device::CHAN_A;

void writeReg(Scc8530Device& s, int ch, int reg, uint8_t v)
{
    if (reg != 0)
        s.controlWrite(ch, static_cast<uint8_t>((reg & 7) | (reg >= 8 ? 0x08 : 0)));
    s.controlWrite(ch, v);
}

/// The Workstation Card's own SDLC setup: WR4 = $20 (SDLC, x1 clock),
/// WR12/13 = 6 for 230400 bit/s off the 3.6864 MHz crystal.
void setupSdlc(Scc8530Device& s)
{
    s.setPclk(Scc8530Device::kDefaultPclk);
    s.setRtxc(A, Scc8530Device::kDefaultPclk);
    writeReg(s, A, 4, 0x20);
    writeReg(s, A, 11, 0x52);
    writeReg(s, A, 12, 0x06);
    writeReg(s, A, 13, 0x00);
    writeReg(s, A, 14, 0x01);
    writeReg(s, A, 3, 0xD9);    // Rx enable, 8 bits, Rx CRC, enter hunt
    writeReg(s, A, 5, 0x69);    // Tx enable, 8 bits, Tx CRC
}

const uint64_t kByteCycles = (8ull * Scc8530Device::kDefaultPclk) / 230400ull;

// ─── 1. Send Abort flushes the transmit buffer ───────────────────────────
void testSendAbortEmptiesTheTransmitBuffer()
{
    Scc8530Device scc;
    setupSdlc(scc);
    std::vector<std::vector<uint8_t>> frames;
    scc.setFrameCallback([&](int, const std::vector<uint8_t>& f) { frames.push_back(f); });

    scc.controlWrite(A, 0xC0);          // Reset Tx Underrun/EOM: arm the close
    scc.dataWrite(A, 0x11);             // -> shift register, TBE set again
    scc.dataWrite(A, 0x22);             // -> the one buffer slot, TBE CLEAR
    assert((scc.peekRr(A, 0) & 0x04) == 0 && "the one-slot buffer is full");

    scc.controlWrite(A, 0x18);          // WR0 Send Abort — the collision path
    assert(scc.txFrameSize(A) == 0 && "the frame in flight is destroyed");
    assert(!scc.txBusy(A) && "the shift register is flushed");
    assert((scc.peekRr(A, 0) & 0x04) != 0 &&
           "Send Abort must leave the transmit buffer EMPTY (RR0 D2)");

    // The driver retransmits. The new frame must be the new bytes.
    scc.controlWrite(A, 0xC0);
    scc.dataWrite(A, 0x33);
    scc.tick(kByteCycles * 6);
    assert(frames.size() == 1);
    assert(frames[0] == (std::vector<uint8_t>{ 0x33 }) &&
           "the retransmission carried a byte from the aborted frame");
    std::printf("  ok: Send Abort empties the transmit buffer, not just the frame\n");
}

// ─── 2. a whole SDLC frame survives the 2-byte FIFO ──────────────────────
void testWholeFrameReachesTheDriver()
{
    Scc8530Device scc;
    writeReg(scc, A, 9, 0x08);          // MIE
    writeReg(scc, A, 4, 0x20);          // SDLC
    writeReg(scc, A, 3, 0xD1);          // Rx enable, 8 bits, enter hunt
    writeReg(scc, A, 1, 0x10);          // Rx interrupt on all characters

    // A real LLAP control frame: destination, source, type ($81 = lapENQ).
    const uint8_t llap[] = { 0x0B, 0x2A, 0x81 };
    scc.receiveFrame(A, llap, sizeof llap);
    assert(scc.rxFifoCount(A) == 2 && "the FIFO takes two and holds the rest");

    std::vector<uint8_t> got;
    bool eofOnLast = false;
    for (int guard = 0; guard < 16 && scc.rxFifoCount(A) > 0; ++guard) {
        got.push_back(scc.dataRead(A));
        if (scc.peekRr(A, 1) & 0x80) {          // End Of Frame: FIFO locked
            eofOnLast = (got.size() == sizeof llap);
            assert((scc.peekRr(A, 1) & 0x0E) == 0x06 &&
                   "residue: no residue, 8-bit character");
            scc.controlWrite(A, 0x30);          // WR0 Error Reset unlocks it
        }
    }
    assert(got == std::vector<uint8_t>(llap, llap + sizeof llap) &&
           "the frame lost bytes on the way to the driver");
    assert(eofOnLast && "End Of Frame never reached RR1 D7");
    assert(scc.rxFifoCount(A) == 0);

    // And a frame far past the FIFO: LLAP header + 32 data bytes.
    std::vector<uint8_t> big{ 0x0B, 0x2A, 0x01 };
    for (int i = 0; i < 32; ++i) big.push_back(static_cast<uint8_t>(0xA0 + i));
    scc.receiveFrame(A, big.data(), big.size());
    std::vector<uint8_t> back;
    for (std::size_t guard = 0; guard < big.size() + 8 && scc.rxFifoCount(A) > 0; ++guard) {
        back.push_back(scc.dataRead(A));
        if (scc.peekRr(A, 1) & 0x80) scc.controlWrite(A, 0x30);
    }
    assert(back == big && "a 35-byte SDLC frame did not survive the 2-byte FIFO");
    std::printf("  ok: a whole SDLC frame reaches the driver, End Of Frame included\n");
}

// ─── 3. a bad FCS marks the LAST byte, and only it ───────────────────────
void testBadFcsMarksTheClosingByteOnly()
{
    Scc8530Device scc;
    writeReg(scc, A, 4, 0x20);
    writeReg(scc, A, 3, 0xC1);
    const uint8_t frame[] = { 0x11, 0x22, 0x33, 0x44 };
    scc.receiveFrame(A, frame, sizeof frame, /*crcError=*/true);

    for (std::size_t i = 0; i < sizeof frame; ++i) {
        const uint8_t b = scc.dataRead(A);
        assert(b == frame[i]);
        const bool last = (i + 1 == sizeof frame);
        assert(((scc.peekRr(A, 1) & 0x40) != 0) == last &&
               "the FCS verdict belongs to the closing byte");
        if (scc.peekRr(A, 1) & 0x80) scc.controlWrite(A, 0x30);
    }
    std::printf("  ok: a bad FCS lands on the frame's closing byte\n");
}

// ─── 4. the held tail round-trips through a snapshot ─────────────────────
void testPendingTailRoundTrips()
{
    Scc8530Device scc;
    writeReg(scc, A, 4, 0x20);
    writeReg(scc, A, 3, 0xC1);
    const uint8_t frame[] = { 0x0B, 0x2A, 0x81, 0x5A };
    scc.receiveFrame(A, frame, sizeof frame);
    assert(scc.rxFifoCount(A) == 2);

    std::vector<uint8_t> blob;
    scc.appendSnapshot(blob);

    Scc8530Device other;
    writeReg(other, A, 4, 0x20);
    writeReg(other, A, 3, 0xC1);
    assert(other.restoreSnapshot(blob.data(), blob.size()));

    std::vector<uint8_t> back;
    for (std::size_t guard = 0; guard < 16 && other.rxFifoCount(A) > 0; ++guard) {
        back.push_back(other.dataRead(A));
        if (other.peekRr(A, 1) & 0x80) other.controlWrite(A, 0x30);
    }
    assert(back == std::vector<uint8_t>(frame, frame + sizeof frame) &&
           "the frame still arriving was lost across the snapshot");
    // A blob truncated inside the new tail must be refused whole.
    assert(!other.restoreSnapshot(blob.data(), blob.size() - 1));
    std::printf("  ok: a frame still arriving survives a rewind\n");
}

} // namespace

int main()
{
    testSendAbortEmptiesTheTransmitBuffer();
    testWholeFrameReachesTheDriver();
    testBadFcsMarksTheClosingByteOnly();
    testPendingTailRoundTrips();
    std::printf("OK scc8530_sdlc_recovery\n");
    return 0;
}
