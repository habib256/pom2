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

#include "AppleParallelCard.h"

#include <algorithm>

AppleParallelCard::AppleParallelCard(int slot) : slot_(slot)
{
    // A held byte that finally prints is the /ACK edge the latch waits for.
    printer_.setAckHandler([this] { setAckLatch(); });
    onReset();
}

bool AppleParallelCard::loadProm(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != kPromBytes) return false;
    std::copy(bytes.begin(), bytes.end(), prom_.begin());
    promLoaded_ = true;
    return true;
}

uint8_t AppleParallelCard::deviceSelectRead(uint8_t low4)
{
    // a2pic.cpp:210-238.
    switch (low4 & 0x07) {
        case 3:
            return static_cast<uint8_t>(0x97 |
                                        (printer_.peLine()     ? 0x20 : 0) |
                                        (printer_.selectLine() ? 0x40 : 0) |
                                        (printer_.faultLine()  ? 0x08 : 0));
        case 4:
            // /ACK idles high; the SW1:5 polarity switch (negative, MAME's
            // default) passes it through unchanged.
            return static_cast<uint8_t>((ackLatch_ ? 0x80 : 0) |
                                        (openBus() & 0x7E) | 0x01);
        case 6:
            irqEnable_ = true;
            updateIrq();
            break;
        case 7:
            resetMode();
            break;
        default:
            break;
    }
    return openBus();
}

void AppleParallelCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // a2pic.cpp:240-275.
    switch (low4 & 0x07) {
        case 0:
            dataLatch_ = v;
            if (!autostrobeDisable_) strobe(v);
            break;
        case 2:
            strobe(dataLatch_);
            break;
        case 6:
            irqEnable_ = true;
            updateIrq();
            break;
        case 7:
            resetMode();
            break;
        default:
            break;
    }
}

uint8_t AppleParallelCard::slotRomRead(uint8_t low8)
{
    // "Standard (X2)" PROM addressing, a2pic.cpp:286-293.
    uint8_t off = low8;
    if (!(off & 0x40) || ((off & 0x80) && !ackLatch_))
        off = static_cast<uint8_t>(off | 0x40);
    else
        off = static_cast<uint8_t>(off & 0xBF);
    autostrobeDisable_ = false;                  // a2pic.cpp:294-299
    return promLoaded_ ? prom_[firmwareBase_ | off] : openBus();
}

void AppleParallelCard::slotRomWrite(uint8_t /*low8*/, uint8_t /*v*/)
{
    autostrobeDisable_ = false;                  // a2pic.cpp:305-311
}

void AppleParallelCard::onReset()
{
    // a2pic.cpp:358-371.
    firmwareBase_ = sw6ParallelPrinter_ ? 0x100 : 0x000;
    autostrobeDisable_ = true;
    printer_.dropPending();
    resetMode();
}

void AppleParallelCard::resetMode()
{
    autostrobeDisable_ = true;
    irqEnable_ = false;
    setAckLatch();
    updateIrq();
}

void AppleParallelCard::strobe(uint8_t byte)
{
    // The strobe clears the latch; a printer that takes the byte pulses /ACK
    // right away (no strobe timer here), one that cannot leaves it clear.
    clearAckLatch();
    if (printer_.strobe(byte)) setAckLatch();
}

void AppleParallelCard::setAckLatch()
{
    ackLatch_ = true;
    updateIrq();
}

void AppleParallelCard::clearAckLatch()
{
    ackLatch_ = false;
    updateIrq();
}

void AppleParallelCard::updateIrq()
{
    // SW1:7 "Interrupt" ships disabled (a2pic.cpp INPUT_PORTS), and POM2 does
    // not expose it, so the line never rises. Kept so the IRQ state machine
    // reads like MAME's if the switch is ever wired.
    assertIrq(false);
}

void AppleParallelCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back('P'); out.push_back('I'); out.push_back('C'); out.push_back(1);
    out.push_back(dataLatch_);
    out.push_back(autostrobeDisable_ ? 1 : 0);
    out.push_back(ackLatch_ ? 1 : 0);
    out.push_back(irqEnable_ ? 1 : 0);
    out.push_back(static_cast<uint8_t>(firmwareBase_ >> 8));
}

void AppleParallelCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (!data || len < 9 || data[0] != 'P' || data[1] != 'I' || data[2] != 'C' ||
        data[3] != 1)
        return;
    dataLatch_ = data[4];
    autostrobeDisable_ = data[5] != 0;
    ackLatch_ = data[6] != 0;
    irqEnable_ = data[7] != 0;
    firmwareBase_ = (data[8] & 1) ? 0x100 : 0x000;
    updateIrq();
}
