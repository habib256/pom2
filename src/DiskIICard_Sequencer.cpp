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

// DiskIICard_Sequencer — POM2's own sequencer, the Disk II's read/write
// logic when there is no P6 table. A sibling of DiskIICard.cpp (whose
// lssSync calls it once per LSS cycle while lssTable_ is false), kept in its
// own translation unit by R0 (docs/decisions/project-scope.md).

#include "DiskIICard.h"
#include "DiskImage.h"

#include <cstdint>

// POM2's own sequencer — what the controller does when there is no P6
// table (Apple's 341-0028-A is not redistributed). Not a re-encoding of that
// table: a data separator and a bit shifter written from the behaviour the
// guest and the disk can observe, measured on Apple's sequencer as a black
// box (lss_window_probe, lss_write_probe).
//
// One cell clock, 8 LSS cycles (4 us), runs in every mode, so writing picks
// up the phase reading left off, as the hardware's does.
//
// Reading (Q7 low, Q6 low):
//   * A flux pulse marks its bit cell as a 1 and re-times the clock: the bit
//     is decided 4 LSS cycles (a half cell) after the pulse, and every 8
//     cycles after that a cell with no pulse is a 0.
//   * Bits shift into the data register until its bit 7 is set: a byte.
//   * The byte is then HELD. Zeros are ignored; the first 1 is kept aside,
//     and at the decision after it the register restarts as that 1 followed
//     by the new bit. So a byte stays readable for two bit cells (8 CPU
//     cycles), and a sync byte's two trailing zeros keep it for four (16) —
//     the windows `LDA $C08C,X / BPL` polling loops are timed against.
// Sensing (Q7 low, Q6 high): the register shifts right with the WPT line in
// bit 7.
// Writing (Q7 high): the register takes the bus every cycle while Q6 is high
// (a load). Once per cell, where a read decision would fall, it shifts left
// if Q6 is low, and its bit 7 becomes the next bit out; five LSS cycles
// later that bit goes out — a 1 flips the write line, which becomes a flux
// transition. So a byte written every 32 CPU cycles is one load cell and
// seven shift cells, and a self-sync $FF written every 40 is eight 1s and
// two 0s. With the load-then-shift spacing every RWTS uses (STA $C0nD,X then
// ORA $C0nC,X: 4 CPU cycles) it lays down Apple's sequencer's flux exactly,
// whatever the phase; a shift started 5-7 cycles after the load — which no
// standard RWTS does — can differ from it by one sync bit
// (lss_write_probe).
//
// Its counters are in the snapshot (D2S1 v5), so a rewind or a session
// restored mid-read serves the same nibbles as the machine it was saved from.
void DiskIICard::ownSequencerStep(DiskImage& img, int qt, int64_t revStart)
{
    const bool q6 = (address & 0x04) != 0;
    const bool q7 = (address & 0x08) != 0;
    if (q7) {
        if (q6) lssData = writeLatch;            // load: the bus, every cycle
        if (seqCount_ > 0) --seqCount_;
        if (seqCount_ == 0) seqCount_ = 8;
        const bool prev = writeLineActive;
        bool curr = prev;
        if (seqCount_ == 3 && seqWriteBit_) curr = !prev;   // a 1 goes out
        if (seqCount_ == 8) {                    // 5 LSS cycles before: the
            if (!q6)                             // next bit is decided
                lssData = static_cast<uint8_t>(lssData << 1);
            seqWriteBit_ = (lssData & 0x80) != 0;
        }
        // The write line, recorded as the table path records it.
        if (prev != curr) {
            writeLineActive = curr;
            if (writePosition < kWriteBufferSize)
                writeBuffer[writePosition++] = static_cast<int64_t>(lssCycle);
        } else if (writePosition >= 30) {
            const int64_t now = static_cast<int64_t>(lssCycle);
            if (writeBackEnabled && !writingInhibited()) {
                img.writeFlux(qt, writeStartTime, now, writePosition,
                              writeBuffer, revStart);
                ++writeFlushCount;
            }
            writeStartTime = now;
            writePosition  = 0;
        }
        return;
    }
    if (q6) {                                    // sense write-protect
        lssData = static_cast<uint8_t>((lssData >> 1)
                | (senseWriteProtect() ? 0x80 : 0x00));
        return;
    }
    if ((address & 0x10) == 0) {                 // PULSE (active low)
        seqSeen_  = true;
        seqCount_ = 4;
    }
    if (seqCount_ > 0 && --seqCount_ > 0) return;
    const int bit = seqSeen_ ? 1 : 0;
    seqSeen_  = false;
    seqCount_ = 8;
    if (!(lssData & 0x80)) {
        lssData = static_cast<uint8_t>((lssData << 1) | bit);
    } else if (seqPending_) {
        lssData = static_cast<uint8_t>(0x02 | bit);
        seqPending_ = false;
    } else if (bit) {
        seqPending_ = true;
    }
}
