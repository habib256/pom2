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

// FASTChip //e — see FastChip.h.

#include "FastChip.h"

#include "ByteIO.h"

#include <algorithm>

namespace pom2 {
namespace {
constexpr uint32_t kSnapMagic   = 0x50484346u;   // 'FCHP'
constexpr uint16_t kSnapVersion = 1;
constexpr std::size_t kSnapBytes = 4 + 2 + 5 + 8 + 8 + 1;

// User's Guide p. 22, values 1-40.
constexpr double kMHz[41] = {
    0.0,  0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90, 1.00,
    1.10, 1.20, 1.30, 1.40, 1.50, 1.60, 1.70, 1.80, 1.90, 2.00,
    2.10, 2.20, 2.30, 2.50, 2.60, 2.70, 2.90, 3.10, 3.30, 3.50,
    3.80, 4.10, 4.55, 5.00, 5.50, 6.20, 7.10, 8.30, 10.00, 12.50,
    16.60,
};
} // namespace

double FastChip::speedMHz(uint8_t value)
{
    return value <= kSpeedMax ? kMHz[value] : kMHz[kSpeedMax];
}

// "Off and 1 Mhz are both the normal Apple //e speed" (p. 11): 1.00 is the
// stock clock, and the table scales from it.
double FastChip::multiplier() const
{
    const uint8_t v = config_[0];
    return v == kSpeedOff ? 1.0 : speedMHz(v) / speedMHz(kSpeed1MHz);
}

void FastChip::slowFor(int ms, uint64_t now)
{
    slowUntil_ = now + static_cast<uint64_t>(ms * stockHz_ / 1000.0);
}

double FastChip::cpuSpeedMultiplier(uint64_t now) const
{
    return now < slowUntil_ ? 1.0 : multiplier();
}

uint64_t FastChip::stockCyclesBetween(uint64_t from, uint64_t to) const
{
    if (to <= from) return 0;
    const double m = multiplier();
    const uint64_t slowEnd = std::clamp(slowUntil_, from, to);
    return (slowEnd - from) + static_cast<uint64_t>(static_cast<double>(to - slowEnd) / m);
}

void FastChip::onReset(uint64_t /*now*/)
{
    unlocked_ = false;
    unlockCount_ = 0;
    enabled_ = false;
    index_ = 0;
    config_.fill(0);
    config_[0] = powerOnSpeed_;
    config_[2] = 2;          // speaker delay NORMAL
    config_[3] = 2;          // joystick delay LONG, the manual's advice (p. 14)
    slowUntil_ = 0;
}

CpuSocketAccelerator::ReadResult FastChip::ioRead(uint8_t low, uint64_t now, uint8_t /*stateByte*/)
{
    ReadResult r;
    if ((low & 0xF0) == 0x30) { if (config_[2]) slowFor(kShortDelayMs, now); return r; }
    if (low == 0x70)           { if (config_[3]) slowFor(kShortDelayMs, now); return r; }
    if (low >= 0x90) {
        if (!((config_[1] >> (low >> 4 & 7)) & 1)) slowFor(kSlotDelayMs, now);
        return r;
    }
    if (!enabled_ || low < 0x6B || low > 0x6F) return r;
    r.answer = true;
    switch (low) {
        case 0x6B: r.value = 0x80; break;
        case 0x6C: r.value = config_[1]; break;
        case 0x6D: r.value = config_[0]; break;
        case 0x6E: r.value = index_; break;
        case 0x6F: r.value = config_[index_ & 7]; break;
        default: break;
    }
    return r;
}

void FastChip::ioWrite(uint8_t low, uint8_t value, uint64_t now)
{
    if ((low & 0xF0) == 0x30) { if (config_[2]) slowFor(kShortDelayMs, now); return; }
    if (low == 0x70)           { if (config_[3]) slowFor(kShortDelayMs, now); return; }
    if (low >= 0x90) {
        if (!((config_[1] >> (low >> 4 & 7)) & 1)) slowFor(kSlotDelayMs, now);
        return;
    }
    switch (low) {
        case 0x6A:
            if (value == 0x6A) {
                if (++unlockCount_ >= 4) unlocked_ = true;
            } else {                     // $A6 locks; anything else too (see the header)
                unlockCount_ = 0;
                unlocked_ = false;
                enabled_ = false;
            }
            break;
        case 0x6B: if (unlocked_) enabled_ = true; break;
        case 0x6C: if (enabled_) config_[1] = value; break;
        case 0x6D: if (enabled_) config_[0] = std::min<uint8_t>(value, kSpeedMax); break;
        case 0x6E: if (enabled_) index_ = value; break;
        case 0x6F:
            if (enabled_ && index_ < config_.size())
                config_[index_] = index_ == 0 ? std::min<uint8_t>(value, kSpeedMax) : value;
            break;
        default: break;
    }
}

void FastChip::appendSnapshotState(std::vector<uint8_t>& out) const
{
    byteio::putU32(out, kSnapMagic);
    byteio::putU16(out, kSnapVersion);
    out.push_back(unlocked_ ? 1 : 0);
    out.push_back(static_cast<uint8_t>(std::min(unlockCount_, 4)));
    out.push_back(enabled_ ? 1 : 0);
    out.push_back(index_);
    out.push_back(powerOnSpeed_);
    out.insert(out.end(), config_.begin(), config_.end());
    byteio::putU64(out, slowUntil_);
    out.push_back(0);                    // reserved
}

bool FastChip::validateSnapshotState(const uint8_t* data, std::size_t len) const
{
    if (!data || len < kSnapBytes) return false;
    byteio::Reader r(data, len);
    return r.u32() == kSnapMagic && r.u16() == kSnapVersion &&
           data[6] <= 1 && data[7] <= 4 && data[8] <= 1 && data[10] <= kSpeedMax &&
           data[11] <= kSpeedMax;
}

bool FastChip::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (!validateSnapshotState(data, len)) return false;
    unlocked_ = data[6] != 0;
    unlockCount_ = data[7];
    enabled_ = data[8] != 0;
    index_ = data[9];
    powerOnSpeed_ = data[10];
    std::copy_n(data + 11, config_.size(), config_.begin());
    byteio::Reader r(data + 19, len - 19);
    slowUntil_ = r.u64();
    return true;
}

} // namespace pom2
