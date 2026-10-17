// POM2 — GPL-3.0-or-later
#include "SnesMaxCard.h"
#include <cstring>
#include <cmath>
namespace pom2 {
void SnesMaxCard::setButtons(int player, uint16_t pressed)
{
    if (player < 0 || player >= 2) return;
    pressed &= 0x0FFF;
    // MAME joypad.cpp:84-95: suppress contradictory directions.
    if (pressed & Left) pressed &= ~Right;
    if (pressed & Up) pressed &= ~Down;
    buttons_[player].store(pressed, std::memory_order_relaxed);
}
void SnesMaxCard::clock()
{
    data_ = 0x3F;
    for (int player = 0; player < 2; ++player) {
        if (!(shift_[player] & 1)) data_ |= static_cast<uint8_t>(0x80 >> player);
        shift_[player] >>= 1;
    }
}
void SnesMaxCard::deviceSelectWrite(uint8_t offset, uint8_t)
{
    if (offset & 1) clock();
    else latchRemaining_ = 7;
}
void SnesMaxCard::advanceCycles(int cycles)
{
    if (cycles <= 0 || !latchRemaining_) return;
    latchRemaining_ -= static_cast<double>(cycles) * busHz_ / cpuHz_;
    if (latchRemaining_ > 0.0) return;
    latchRemaining_ = 0;
    for (int player = 0; player < 2; ++player)
        shift_[player] = buttons_[player].load(std::memory_order_relaxed) | 0xFFFF0000u;
    clock();
}
void SnesMaxCard::setCpuClock(double hz)
{
    if (std::isfinite(hz) && hz > 0.0) cpuHz_ = hz;
}
void SnesMaxCard::setStandardClock(double hz)
{
    if (std::isfinite(hz) && hz > 0.0) busHz_ = hz;
}
void SnesMaxCard::onPowerCycle()
{
    shift_.fill(0);
    latchRemaining_ = 0;
    data_ = 0xFF;
}
void SnesMaxCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), {'S', 'M', 'X', '2', data_});
    uint64_t remaining = 0;
    static_assert(sizeof(remaining) == sizeof(latchRemaining_));
    std::memcpy(&remaining, &latchRemaining_, sizeof(remaining));
    for (unsigned bit = 0; bit < 64; bit += 8)
        out.push_back(static_cast<uint8_t>(remaining >> bit));
    for (const uint32_t value : shift_)
        for (unsigned bit = 0; bit < 32; bit += 8)
            out.push_back(static_cast<uint8_t>(value >> bit));
}
void SnesMaxCard::loadSnapshotState(const uint8_t* data, std::size_t size)
{
    if (!data || size < 14 || (data[4] & 0x3F) != 0x3F) return;
    size_t shiftOffset = 6;
    double remaining = 0.0;
    if (std::memcmp(data, "SMX2", 4) == 0) {
        if (size < 21) return;
        uint64_t bits = 0;
        for (unsigned byte = 0; byte < 8; ++byte)
            bits |= uint64_t(data[5 + byte]) << (byte * 8);
        std::memcpy(&remaining, &bits, sizeof(remaining));
        shiftOffset = 13;
    } else if (std::memcmp(data, "SMX1", 4) == 0) {
        remaining = data[5];
    } else return;
    if (!std::isfinite(remaining) || remaining < 0.0 || remaining > 7.0) return;
    std::array<uint32_t, 2> shifts{};
    for (int player = 0; player < 2; ++player)
        for (unsigned byte = 0; byte < 4; ++byte)
            shifts[player] |= uint32_t(data[shiftOffset + player * 4 + byte]) << (byte * 8);
    shift_ = shifts;
    data_ = data[4];
    latchRemaining_ = remaining;
}
}
