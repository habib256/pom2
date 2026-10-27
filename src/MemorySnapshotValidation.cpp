// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Memory.h"
#include <cmath>
#include <cstring>

#include "CpuSocketAccelerator.h"
#include "IWMDevice.h"
#include "MemoryProfile.h"
#include "NoSlotClock.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"

// Pure preflight of Memory's v1 extended-state wire layout. No apply/rollback:
// restoring a checkpoint cannot put back beam events, cache cursors or an
// external bus transaction that a failed decode already discarded.
bool Memory::validateSnapshotState(const uint8_t* data, size_t n,
                                   uint32_t* savedRamWorksBanks) const
{
    if (savedRamWorksBanks) *savedRamWorksBanks = 1;
    if (!data || n == 0 || data[0] != kMemStateBlobVersion) return false;
    // version + memory/display/latch/clock header + main language-card RAM.
    size_t pos = 1 + 32 + lcBank1.size() + lcBank2.size() + lcHigh.size();
    auto need = [&](size_t k) { return pos <= n && k <= n - pos; };
    auto u32 = [&]() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(data[pos++]) << (8 * i);
        return v;
    };
    if (!need(5)) return false;
    const uint32_t banks = u32();
    const uint8_t bank = data[pos++];
    const uint32_t count = banks == 0 ? 1 : banks; // legacy stock encoding
    if (savedRamWorksBanks) *savedRamWorksBanks = count;
    if (count > 128 || bank >= count) return false;
    if (banks <= 1) {
        if (!need(kRamWorksBankStride)) return false;
        pos += kRamWorksBankStride;
    } else {
        if (!need(4)) return false;
        const uint32_t backing = u32();
        // A truncated/surplus backing is not a different machine geometry:
        // load's best-effort branch would silently keep only one aux bank.
        if (backing != count * kRamWorksBankStride || !need(backing)) return false;
        pos += backing;
    }
    if (need(1)) ++pos; // optional //c SmartPort arming latch
    auto section = [&](auto&& validate) {
        if (pos == n) return true; // older blobs end at any trailer boundary
        if (!need(4)) return false;
        const uint32_t len = u32();
        if (!need(len)) return false;
        if (len && !validate(data + pos, static_cast<size_t>(len))) return false;
        pos += len;
        return true;
    };
    if (!section([&](const uint8_t* p, size_t k) {
            return !iwmDevice || iwmDevice->loadSnapshotState(p, k, false);
        })) return false;
    if (!section([&](const uint8_t* p, size_t k) {
            return !iicProfile_ || iicProfile_->validateSnapshotState(p, k) != 0;
        })) return false;
    if (!section([](const uint8_t*, size_t k) { return k >= 4; })) return false;
    if (!section([&](const uint8_t* p, size_t k) {
            return !noSlotClock_ || noSlotClock_->loadSnapshotState(p, k, false);
        })) return false;
    for (int which = 0; which < 2; ++which) {
        if (!section([&](const uint8_t* p, size_t k) {
                auto* d = smartPortHub ? (which == 0 ? smartPortHub->internal35()
                                                    : smartPortHub->external35()) : nullptr;
                return !d || d->loadSnapshotState(p, k, false);
            })) return false;
    }
    if (!section([&](const uint8_t* p, size_t k) {
            const auto* mouse = iicProfile_ ? slots.peripheral(4) : nullptr;
            return !mouse || mouse->validateIicMouseState(p, k);
        })) return false;
    if (!section([&](const uint8_t* p, size_t k) {
            auto* d = smartPortHub ? smartPortHub->external35Second() : nullptr;
            return !d || d->loadSnapshotState(p, k, false);
        })) return false;
    if (!section([&](const uint8_t* p, size_t k) {
            return !accel_ || accel_->validateSnapshotState(p, k);
        })) return false;
    if (!section([](const uint8_t* p, size_t k) {   // the beam's clock
            if (k != 24) return false;
            uint64_t bits = 0;
            for (int i = 0; i < 8; ++i) bits |= static_cast<uint64_t>(p[16 + i]) << (8 * i);
            double r = 0.0;
            std::memcpy(&r, &bits, sizeof r);
            return std::isfinite(r) && r > 0.0 && r <= 1.0;
        })) return false;
    // Unknown future trailers, like the apply path, are intentionally ignored.
    return true;
}
