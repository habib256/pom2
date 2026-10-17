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

// ZipChip — see ZipChip.h. Line numbers cite MAME src/mame/apple/apple2e.cpp.

#include "ZipChip.h"

#include "ByteIO.h"

namespace pom2 {
namespace {
constexpr uint32_t kSnapMagic   = 0x5049505Au;   // 'ZPIP'
constexpr uint16_t kSnapVersion = 1;
constexpr std::size_t kSnapBytes = 4 + 2 + 7 + 8;
} // namespace

// accel_temp_delay (:1547-1556). "C05B status bit toggles even when the
// accelerator is disabled" — the window opens whether or not we are fast.
void ZipChip::tempDelay(int ms, bool condition, uint64_t now)
{
    if (disableDelay_ || !condition) return;
    const uint64_t until = now + static_cast<uint64_t>(ms * stockHz_ / 1000.0);
    if (until > slowUntil_) slowUntil_ = until;
}

// accel_update_speed (:1515-1525).
double ZipChip::cpuSpeedMultiplier(uint64_t now) const
{
    if (!fast_ || now < slowUntil_) return 1.0;
    return kFastHz / stockHz_;
}

// accel_reset (:1527-1545), from reset_w (:1438-1439).
void ZipChip::onReset(uint64_t /*now*/)
{
    unlocked_ = false;
    stage_ = 0;
    slotSpk_ = kResetSlotSpk;
    gameIo_ = kResetGameIo;
    disableDelay_ = false;
    fast_ = bootFast_;
    slowUntil_ = 0;
}

CpuSocketAccelerator::ReadResult
ZipChip::ioRead(uint8_t low, uint64_t now, uint8_t stateByte)
{
    ReadResult r;
    if ((low & 0xF0) == 0x30) {                       // speaker, :1914
        tempDelay(kShortDelayMs, (slotSpk_ & 1) != 0, now);
        return r;
    }
    if (low == 0x70) {                                // PTRIG, :2043-2044
        tempDelay(kShortDelayMs, (gameIo_ & 0x40) != 0, now);
        return r;
    }
    if (low >= 0x90) {                                // slot n $C0nX, :2898
        tempDelay(kSlotDelayMs, ((slotSpk_ >> (low >> 4 & 7)) & 1) != 0, now);
        return r;
    }
    if (!unlocked_ || (low & 0xF8) != 0x58) return r;

    // :2242-2244 — the annunciator side effect is skipped when unlocked AND
    // fast; :2247-2279 — the register values.
    r.answer = true;
    r.skipMotherboard = fast_;
    switch (low) {
        case 0x58: r.value = 0xC0; break;     // "undocumented, not floating bus"
        case 0x59: r.value = 0x20; break;
        case 0x5A: case 0x5D: case 0x5F: r.value = 0x00; break;
        case 0x5B: {
            const uint8_t b7 = ((now >> 9) & 1) ? 0x80 : 0x00;   // PH0 / 1024
            const uint8_t b5 = (gameIo_ & 0x80) ? 0x20 : 0x00;   // LC cache off
            const uint8_t b4 = fast_ ? 0x00 : 0x10;              // Zip disabled
            const uint8_t b3 = now < slowUntil_ ? 0x08 : 0x00;   // delay active
            r.value = static_cast<uint8_t>(b7 | b5 | b4 | b3 | 0x03);
            break;
        }
        case 0x5C: r.value = slotSpk_; break;
        case 0x5E: r.value = stateByte; break;
        default: break;
    }
    return r;
}

void ZipChip::ioWrite(uint8_t low, uint8_t value, uint64_t now)
{
    // The unlock window: any write other than $5A to $C05A between the
    // first and the fourth cancels it (:2570-2590; see the header for what
    // POM2 can and cannot see).
    if (stage_ > 0 && !unlocked_) {
        if (low != 0x5A || value != 0x5A) {
            stage_ = 0;
        } else if (++stage_ == 4) {
            unlocked_ = true;
            stage_ = 0;
            return;
        } else {
            return;
        }
    }

    // Speaker: the delay lives in do_io (:1914), which writes reach too.
    // PTRIG's paddle delay is on the READ side only (c000_r :2043-2044;
    // c000_w's $C070-$C07F case has none), so a write does not open it.
    if ((low & 0xF0) == 0x30) { tempDelay(kShortDelayMs, (slotSpk_ & 1) != 0, now); return; }
    if (low >= 0x90) {                                // c080_w, :2934
        tempDelay(kSlotDelayMs, ((slotSpk_ >> (low >> 4 & 7)) & 1) != 0, now);
        return;
    }

    switch (low) {
        case 0x5A:                                    // :2562-2607
            if (value == 0x5A) {
                if (stage_ == 0 && !unlocked_) stage_ = 1;
            } else if (value == 0xA5) {
                unlocked_ = false;
                stage_ = 0;
            } else if (unlocked_) {
                fast_ = false;
            }
            break;
        case 0x5B: if (unlocked_) fast_ = true; break;          // :2609-2616
        case 0x5C: if (unlocked_) slotSpk_ = value; break;      // :2618-2624
        case 0x5E:                                              // :2626-2640
            if (unlocked_) {
                disableDelay_ = (value & 0x80) != 0;
                if (disableDelay_) slowUntil_ = 0;              // accel_stop_delay
            }
            break;
        case 0x5F: if (unlocked_) gameIo_ = value; break;       // :2642-2648
        default: break;
    }
}

// MAME save_item list (:1214-1223), with the timer as a deadline.
void ZipChip::appendSnapshotState(std::vector<uint8_t>& out) const
{
    byteio::putU32(out, kSnapMagic);
    byteio::putU16(out, kSnapVersion);
    out.push_back(unlocked_ ? 1 : 0);
    out.push_back(static_cast<uint8_t>(stage_));
    out.push_back(fast_ ? 1 : 0);
    out.push_back(disableDelay_ ? 1 : 0);
    out.push_back(slotSpk_);
    out.push_back(gameIo_);
    out.push_back(bootFast_ ? 1 : 0);
    byteio::putU64(out, slowUntil_);
}

bool ZipChip::validateSnapshotState(const uint8_t* data, std::size_t len) const
{
    if (!data || len < kSnapBytes) return false;
    byteio::Reader r(data, len);
    return r.u32() == kSnapMagic && r.u16() == kSnapVersion;
}

bool ZipChip::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (!validateSnapshotState(data, len)) return false;
    byteio::Reader r(data, len);
    (void)r.u32(); (void)r.u16();
    unlocked_     = r.u8() != 0;
    stage_        = r.u8() & 3;
    fast_         = r.u8() != 0;
    disableDelay_ = r.u8() != 0;
    slotSpk_      = r.u8();
    gameIo_       = r.u8();
    (void)r.u8();                 // bootFast_ is a host setting, not machine state
    slowUntil_    = r.u64();
    return true;
}

} // namespace pom2
