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

#include "GrapplerClassicCard.h"

#include <algorithm>

GrapplerClassicCard::GrapplerClassicCard(int slot) : slot_(slot)
{
    // A held byte that finally prints: /ACK low clears the latch
    // (grappler.cpp:424-435).
    printer_.setAckHandler([this] { ackLatch_ = false; });
}

bool GrapplerClassicCard::loadRom(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != kRomBytes) return false;
    std::copy(bytes.begin(), bytes.end(), rom_.begin());
    romLoaded_ = true;
    return true;
}

uint8_t GrapplerClassicCard::deviceSelectRead(uint8_t low4)
{
    // grappler.cpp:278-302: the strobe side effects happen on reads too.
    if (low4 & 0x02)      setStrobe(false);
    else if (low4 & 0x04) setStrobe(true);
    if (low4 & 0x01) {
        return static_cast<uint8_t>((openBus() & 0xF0) |
                                    (printer_.busyLine()   ? 0x08 : 0) |
                                    (printer_.peLine()     ? 0x04 : 0) |
                                    (printer_.selectLine() ? 0x02 : 0) |
                                    (ackLatch_ ? 0x01 : 0));
    }
    return openBus();
}

void GrapplerClassicCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // grappler.cpp:305-316.
    if (low4 & 0x01) dataLatch_ = v;
    if (low4 & 0x02)      setStrobe(false);
    else if (low4 & 0x04) setStrobe(true);
}

void GrapplerClassicCard::setStrobe(bool level)
{
    // grappler.cpp:397-419: the falling edge sets the latch ("waiting") while
    // /ACK is idle. The printer takes the byte on the rising edge; taking it
    // is its /ACK pulse, which clears the latch.
    if (strobe_ && !level) ackLatch_ = true;
    if (!strobe_ && level) {
        if (printer_.strobe(dataLatch_)) ackLatch_ = false;
    }
    strobe_ = level;
}

uint8_t GrapplerClassicCard::slotRomRead(uint8_t low8)
{
    // grappler.cpp:319-322: one page per slot.
    if (!romLoaded_) return openBus();
    return rom_[(static_cast<std::size_t>(slot_ & 7) << 8) | low8];
}

uint8_t GrapplerClassicCard::expansionRomRead(uint16_t offset)
{
    return romLoaded_ ? rom_[offset & 0x7FF] : 0xFF;   // grappler.cpp:121-124
}

void GrapplerClassicCard::onReset()
{
    setStrobe(true);                                  // grappler.cpp:369-372
    printer_.dropPending();
}

void GrapplerClassicCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back('G'); out.push_back('P'); out.push_back('C'); out.push_back(1);
    out.push_back(dataLatch_);
    out.push_back(strobe_ ? 1 : 0);
    out.push_back(ackLatch_ ? 1 : 0);
    // Tail (2026-09-29): the byte the printer is holding, with its flag.
    out.push_back(printer_.hasPending() ? 1 : 0);
    out.push_back(printer_.pendingByte());
}

void GrapplerClassicCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (!data || len < 7 || data[0] != 'G' || data[1] != 'P' || data[2] != 'C' ||
        data[3] != 1)
        return;
    dataLatch_ = data[4];
    strobe_ = data[5] != 0;
    ackLatch_ = data[6] != 0;
    if (len >= 9) printer_.restorePending(data[7] != 0, data[8]);
    else          printer_.dropPending();   // older blob: nothing was held
}
