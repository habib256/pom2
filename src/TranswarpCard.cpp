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

// TranswarpCard — see the header for the port's provenance and the one
// structural divergence from MAME `a2bus/transwarp.cpp`.

#include "TranswarpCard.h"

#include "ByteIO.h"
#include "CpuClock.h"
#include "Logger.h"
#include "Memory.h"

#include <cstring>
#include <fstream>

namespace pom2 {
namespace {

constexpr uint32_t kSnapMagic   = 0x50525754u;   // 'TWRP' little-endian
// v2 appends the displaced $F000 window (see appendSnapshotState). v1 blobs
// still load — they simply leave `displaced_` at whatever the live card has.
constexpr uint16_t kSnapVersion = 2;

/// Microseconds → 6502 cycles at the Apple's own clock. The slowdown
/// windows are specified in real time on the board (an RC one-shot), so
/// they are converted here rather than written as cycle constants.
constexpr int microsToCycles(int us)
{
    return static_cast<int>((static_cast<long long>(us) * POM2_CPU_CLOCK_HZ
                             + 500000) / 1000000);
}

std::string probeRom(const char* leaf)
{
    static const char* kBases[] = { "", "../", "../../" };
    for (const char* b : kBases) {
        std::string p = std::string(b) + leaf;
        std::ifstream f(p, std::ios::binary);
        if (f.good()) return p;
    }
    return {};
}

} // namespace

// The slot is bookkeeping the base class already does (`busSlot()`); the
// card decodes nothing slot-relative — it has no ROM and no $C0nX window.
TranswarpCard::TranswarpCard(int) {}

// ─── DIP switches ────────────────────────────────────────────────────────

void TranswarpCard::setFullAcceleration(bool full)
{
    if (full) dsw1_ = static_cast<uint8_t>(dsw1_ & ~kDsw1HalfSpeed);
    else      dsw1_ = static_cast<uint8_t>(dsw1_ |  kDsw1HalfSpeed);
}

void TranswarpCard::setAccelerationEnabled(bool on)
{
    if (on) dsw2_ = static_cast<uint8_t>(dsw2_ & ~kDsw2AccelOff);
    else    dsw2_ = static_cast<uint8_t>(dsw2_ |  kDsw2AccelOff);
}

void TranswarpCard::setSlotAccelerated(int s, bool on)
{
    if (s < 1 || s > 7) return;
    const uint8_t bit = static_cast<uint8_t>(1u << (s - 1));
    if (on) dsw2_ = static_cast<uint8_t>(dsw2_ |  bit);
    else    dsw2_ = static_cast<uint8_t>(dsw2_ & ~bit);
}

bool TranswarpCard::slotAccelerated(int s) const
{
    if (s < 1 || s > 7) return false;
    return (dsw2_ & (1u << (s - 1))) != 0;
}

// ─── Speed ───────────────────────────────────────────────────────────────

double TranswarpCard::cpuSpeedMultiplier() const
{
    // MAME expresses all of these by calling set_unscaled_clock() on its
    // own CPU; here they are the reasons the Apple's own CPU is not sped
    // up. Order matters only in that each is independently sufficient.
    if (!enabled_ || halted_)          return 1.0;   // $C074 = 3
    if (!accelerationEnabled())        return 1.0;   // DSW2 bit 7
    if (in1MHz_)                       return 1.0;   // $C074 = 1
    if (slowCycles_ > 0)               return 1.0;   // inside a slot window
    return fullAcceleration() ? kFullSpeed : kHalfSpeed;
}

void TranswarpCard::advanceCycles(int cycles)
{
    // The countdown that replaces MAME's emu_timer. Cycles here are 6502
    // cycles at the Apple's clock, which is the domain the windows are
    // specified in — during a slowdown the machine IS at 1 MHz, so a
    // cycle of budget is a microsecond of real time and the two agree.
    if (slowCycles_ > 0) {
        slowCycles_ -= cycles;
        if (slowCycles_ < 0) slowCycles_ = 0;
    }
}

void TranswarpCard::hitSlot(int slot)
{
    // MAME `hit_slot`: only slow down when acceleration is on at all, and
    // then only for a slot whose DSW2 bit says "stock speed".
    if (!accelerationEnabled()) return;
    if (slot < 1 || slot > 7) return;
    if (slotAccelerated(slot)) return;
    slowCycles_ = microsToCycles(kSlotSlowMicros);
}

void TranswarpCard::hitJoystick()
{
    // MAME `hit_slot_joy`: no per-slot gate — a paddle read is a paddle
    // read. The window covers a whole PREAD sweep, which is why it is two
    // orders of magnitude longer than a slot access.
    if (!accelerationEnabled()) return;
    slowCycles_ = microsToCycles(kJoySlowMicros);
}

// ─── Bus snoop (MAME dma_r / dma_w) ──────────────────────────────────────

bool TranswarpCard::busSnoop(uint16_t addr, bool isWrite, uint8_t value)
{
    if (halted_) return false;   // the card's CPU is off; it watches nothing

    if (addr == 0xC070) {
        hitJoystick();
        return false;            // MAME falls through to slot_dma_*
    }

    if (isWrite && addr == 0xC072) {
        // Stop shadowing the Apple's F8 ROM. MAME sets the flag and falls
        // through to slot_dma_write, so the paddle latch still rearms.
        readA2Rom_ = true;
        releaseShadow();
        return false;
    }

    if (isWrite && addr == 0xC074) {
        // MAME `dma_w`: this one RETURNS — the Apple never sees the access.
        switch (value) {
        case 0:
            in1MHz_ = false;
            break;
        case 1:
            in1MHz_ = true;
            break;
        case 3:
            // "disable our CPU / re-enable the Apple's". In POM2 both are
            // the same CPU, so the whole effect is that the multiplier
            // goes to 1 and stays there until a reset.
            halted_ = true;
            break;
        default:
            // Undocumented value: MAME ignores it and still swallows the
            // write. Keep both halves of that.
            break;
        }
        return true;
    }

    if (addr >= 0xC090 && addr <= 0xC0FF) {
        // MAME: `((offset >> 4) & 0xf) - 8` → $C09x = slot 1 … $C0Fx = 7.
        hitSlot(((addr >> 4) & 0x0F) - 8);
        return false;
    }

    if (addr >= 0xC100 && addr <= 0xC7FF) {
        hitSlot((addr >> 8) & 0x07);
        return false;
    }

    return false;
}

// ─── ROM shadow ──────────────────────────────────────────────────────────

bool TranswarpCard::setRom(std::vector<uint8_t> bytes)
{
    if (bytes.size() != kRomSize) return false;
    rom_ = std::move(bytes);
    if (busSlot() >= 0 && !readA2Rom_) engageShadow();
    return true;
}

std::string TranswarpCard::loadRomFromDisk()
{
    const std::string path = probeRom(kRomPath);
    if (path.empty())
        return std::string("no ") + kRomPath
             + " — the card accelerates but will not shadow $F000-$FFFF "
               "(dump: 4096 bytes, CRC32 afe37f55)";

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return std::string("cannot open ") + path;
    const auto size = static_cast<std::size_t>(f.tellg());
    if (size != kRomSize)
        return path + " is " + std::to_string(size) + " bytes, expected "
             + std::to_string(kRomSize);

    std::vector<uint8_t> bytes(kRomSize);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(kRomSize));
    if (!f) return std::string("short read on ") + path;

    setRom(std::move(bytes));
    return {};
}

void TranswarpCard::engageShadow()
{
    if (shadowing_ || !memory_ || !hasRom()) return;
    // Keep the bytes we are covering so $C072 can put them back. The ROM
    // mirror lives in Memory's flat array at $F000, so this is a plain
    // 4 KB swap and costs nothing once done.
    std::memcpy(displaced_.data(), memory_->data() + 0xF000, kRomSize);
    memory_->loadRomBytes(rom_.data(), kRomSize, 0xF000);
    shadowing_ = true;
}

void TranswarpCard::releaseShadow()
{
    if (!shadowing_ || !memory_) return;
    memory_->loadRomBytes(displaced_.data(), kRomSize, 0xF000);
    shadowing_ = false;
}

// ─── Lifecycle ───────────────────────────────────────────────────────────

void TranswarpCard::onPlug()
{
    onReset();
}

void TranswarpCard::onUnplug()
{
    // Never leave the machine running someone else's Monitor.
    releaseShadow();
}

void TranswarpCard::onReset()
{
    // MAME `reset_from_bus`: re-enable, drop the ROM-passthrough flag, take
    // the bus, and set the clock from the DIPs.
    enabled_   = true;
    readA2Rom_ = false;
    halted_    = false;
    slowCycles_ = 0;
    // MAME does not clear m_bIn1MHzMode here, but it DOES set the fast
    // clock unconditionally and no timer is pending across a reset — so its
    // observable state after reset is "fast". Clearing the flag is how that
    // same observable state is reached in a model where the multiplier is
    // recomputed from the flags on every read.
    in1MHz_ = false;
    engageShadow();
}

// ─── Snapshot ────────────────────────────────────────────────────────────

void TranswarpCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    byteio::putU32(out, kSnapMagic);
    byteio::putU16(out, kSnapVersion);
    out.push_back(dsw1_);
    out.push_back(dsw2_);
    out.push_back(enabled_   ? 1 : 0);
    out.push_back(readA2Rom_ ? 1 : 0);
    out.push_back(in1MHz_    ? 1 : 0);
    out.push_back(halted_    ? 1 : 0);
    byteio::putU32(out, static_cast<uint32_t>(slowCycles_));
    // The shadow flag is NOT written: whether $F000 currently holds the
    // card's ROM is a property of Memory, which the snapshot captures
    // wholesale. Restoring it here would swap 4 KB a second time.
    //
    // `displaced_` IS written (v2). It is the Apple's OWN $F000-$FFFF, saved
    // aside while the card shadows it, and the only copy that exists —
    // Memory holds the card's ROM there. Without it a restore + a later
    // `$C072` (readA2Rom) put 4 KB of zeroes over Applesoft + Monitor. It is
    // only meaningful while shadowing, so a flag byte keeps the cost at one
    // byte otherwise; while shadowing the bytes never change, so the rewind
    // XOR delta codec collapses them to nothing after the first keyframe.
    out.push_back(shadowing_ ? 1 : 0);
    if (shadowing_) out.insert(out.end(), displaced_.begin(), displaced_.end());
}

void TranswarpCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    byteio::Reader r(data, len);
    if (!r.has(4 + 2 + 6 + 4)) return;
    if (r.u32() != kSnapMagic)   return;
    const uint16_t version = r.u16();
    if (version == 0 || version > kSnapVersion) return;

    dsw1_      = r.u8();
    dsw2_      = r.u8();
    enabled_   = r.u8() != 0;
    readA2Rom_ = r.u8() != 0;
    in1MHz_    = r.u8() != 0;
    halted_    = r.u8() != 0;
    // Clamp: a crafted/corrupt blob must not park the card in a slow window
    // that never expires (advanceCycles only counts down).
    slowCycles_ = static_cast<int>(r.u32());
    if (slowCycles_ < 0) slowCycles_ = 0;
    if (slowCycles_ > microsToCycles(kJoySlowMicros))
        slowCycles_ = microsToCycles(kJoySlowMicros);
    // Memory came back with whatever was at $F000 when the snapshot was
    // taken, so track that rather than swapping: if the card was shadowing
    // then, the restored ROM mirror already IS the card's.
    shadowing_ = !readA2Rom_ && hasRom();
    if (version >= 2 && r.has(1)) {
        const bool hadDisplaced = r.u8() != 0;
        if (hadDisplaced && r.has(kRomSize)) {
            std::memcpy(displaced_.data(), r.p + r.pos, kRomSize);
            r.pos += kRomSize;
        }
    }
}

} // namespace pom2
