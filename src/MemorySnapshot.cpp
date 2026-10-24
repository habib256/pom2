// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

// Memory's extended state on the wire — the machine snapshot / rewind /
// session trailer: `appendSnapshotState` writes it, `loadSnapshotState`
// applies it. Lifted out of Memory.cpp (ARCH-004, the seam
// tools/file_size_budget.txt named): the paging core and the wire format
// are two concerns. The preflight that validates a trailer before it is
// applied lives beside it in MemorySnapshotValidation.cpp.
#include "Memory.h"

#include "CharRomDump.h"
#include "MemoryProfile_IIcClass.h"
#include "CassetteDevice.h"
#include "IWMDevice.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "Logger.h"
#include "M6502.h"
#include "NoSlotClock.h"
#include "SpeakerDevice.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

void Memory::appendSnapshotState(std::vector<uint8_t>& out)
{
    auto putU8  = [&](uint8_t v) { out.push_back(v); };
    auto putU16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto putU32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto putU64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto putBytes = [&](const void* p, size_t k) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + k);
    };

    putU8(kMemStateBlobVersion);
    putU8(iieMode ? 1 : 0);
    putU16(iieMemMode);
    putU8(lcReadRam     ? 1 : 0);
    putU8(lcWriteEnable ? 1 : 0);
    putU8(lcBank2Active ? 1 : 0);
    putU8(lcPrewrite    ? 1 : 0);
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        putU8(display.textMode    ? 1 : 0);
        putU8(display.mixedMode   ? 1 : 0);
        putU8(display.page2       ? 1 : 0);
        putU8(display.hiRes       ? 1 : 0);
        putU8(display.eightyCol   ? 1 : 0);
        putU8(display.an3         ? 1 : 0);
        putU8(display.altChar     ? 1 : 0);
        putU8(display.dhgr        ? 1 : 0);
        putU8(display.eightyStore ? 1 : 0);
    }
    putU64(cycleCounter);
    putU64(paddles_.latchCycle());

    // Main Language-Card RAM (II/II+ and IIe main-bank LC live here; the
    // mem[] $D000-$FFFF region is always the ROM mirror).
    putBytes(lcBank1.data(), lcBank1.size());
    putBytes(lcBank2.data(), lcBank2.size());
    putBytes(lcHigh.data(),  lcHigh.size());

    putU32(ramWorksBanks_);
    putU8(ramWorksBank_);
    if (ramWorksBanks_ <= 1) {
        // Stock aux — serialize the visible aux + aux-LC arrays directly.
        putBytes(aux.data(),        aux.size());
        putBytes(auxLcBank1.data(), auxLcBank1.size());
        putBytes(auxLcBank2.data(), auxLcBank2.size());
        putBytes(auxLcHigh.data(),  auxLcHigh.size());
    } else {
        // RamWorks — flush the live visible bank back into the backing store
        // so the serialized backing is fully coherent (see ramWorksSwapToBank
        // for the per-bank layout), then dump the whole backing.
        uint8_t* slot = ramWorksBacking_.data()
                      + static_cast<size_t>(ramWorksBank_) * kRamWorksBankStride;
        std::memcpy(slot,           aux.data(),        0x10000);
        std::memcpy(slot + 0x10000, auxLcBank1.data(), 0x1000);
        std::memcpy(slot + 0x11000, auxLcBank2.data(), 0x1000);
        std::memcpy(slot + 0x12000, auxLcHigh.data(),  0x2000);
        putU32(static_cast<uint32_t>(ramWorksBacking_.size()));
        putBytes(ramWorksBacking_.data(), ramWorksBacking_.size());
    }

    // Trailer (appended, version stays 1 — older blobs simply lack it and
    // the loader falls back to the field's default): the //c-class on-board
    // SmartPort ROM-exposure gate. Without it, a rewind-ring entry captured
    // after a //c HDV/3.5" boot restored with armed=false, flipping
    // $C500-$C5FF back to the real //c firmware under a live ProDOS whose
    // device vector points into the stub — the next MLI call executed
    // unrelated ROM bytes.
    putU8(iicSmartPortArmed_ ? 1 : 0);

    // Second trailer: //c-class on-board device state, each section
    // self-identifying by magic so a blob may carry neither, either or
    // both. Both were previously absent, and both are cycle-sensitive:
    //   * IWMDevice holds eight absolute emuCycles stamps. A rewind rolls
    //     `cycleCounter` back while the IWM kept a larger `lastSync_`, so
    //     `sync()`'s walker stopped advancing and the controller froze
    //     until emulated time caught up to its pre-rewind position.
    //   * The //c+ MIG gate array's 2 KB RAM + page pointer came back
    //     zeroed, so the alt firmware read something other than what it
    //     had written.
    // Length-prefixed: the loader must be able to skip a section it does
    // not understand without losing the rest of the trailer.
    {
        std::vector<uint8_t> sect;
        if (iwmDevice) iwmDevice->appendSnapshotState(sect);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    {
        std::vector<uint8_t> sect;
        if (iicProfile_) iicProfile_->appendSnapshotState(sect);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    // Third section: paging/IOU flip-flops that were previously invisible
    // to snapshot/rewind — INTC8ROM (latched by $C3xx access, cleared by
    // $CFFF; a restored PC inside $C800-$CFFF slot firmware read the
    // wrong ROM without it), IOUDIS, and the //c VBL mask + pending
    // latch. Same length-prefixed convention: absent in older blobs →
    // live values kept.
    {
        std::vector<uint8_t> sect;
        sect.push_back(intC8Rom      ? 1 : 0);
        sect.push_back(ioudis        ? 1 : 0);
        sect.push_back(vblIrqMask    ? 1 : 0);
        sect.push_back(vblIrqPending ? 1 : 0);
        // Annunciators AN0-AN2, appended 2026-09-06. AN3 already travels
        // inside DisplayState; these three did not travel at all, and AN2
        // drives A12 of an 8 KB international character generator
        // (`charRomBankOffset`) — so a snapshot restore or a rewind of a
        // French Touch "Block ASCII" screen came back rendering the other
        // 4 KB font. The section is length-prefixed and grew at the END, so
        // a 4-byte blob from an older build still loads (the loader keeps
        // the live values for anything the section does not carry).
        sect.push_back(display.an0 ? 1 : 0);
        sect.push_back(an1 ? 1 : 0);
        sect.push_back(an2 ? 1 : 0);
        // Appended 2026-09-06, same grow-at-the-end rule:
        //  * `vblWasActive` is the EDGE detector behind the //c VBL IRQ.
        //    It defaults to true, so a restore taken while the beam was
        //    already inside the blanking interval re-armed the edge and
        //    fired one spurious VBL IRQ on the next boundary — the frame
        //    sync a //c PAL French Touch demo races against.
        //  * `iicCardWindow_` is the partner latch of `iicSmartPortArmed_`
        //    (already in the first trailer): it says whether the punched
        //    $C500 page has its $C800 window open. Restoring one without
        //    the other left the pair inconsistent.
        sect.push_back(vblWasActive   ? 1 : 0);
        sect.push_back(iicCardWindow_ ? 1 : 0);
        // Appended 2026-09-09, same grow-at-the-end rule: the $C800-$CFFE
        // expansion-window OWNER (`SlotBus::activeExpansionSlot`). It is the
        // slot-side partner of `intC8Rom` five bytes up — that flag says the
        // MOTHERBOARD holds the window, this byte says WHICH CARD holds it
        // otherwise. Seven cards take it (ClockCard, GrapplerCard, LironCard,
        // WorkstationCard, CffaCard, SmartPortCard, SuperSerialCard) and no
        // section carried it, so a restore whose PC sat inside a card's
        // expansion ROM — a SmartPort/Liron driver, the SSC firmware — fetched
        // the FLOATING BUS for the whole 2 KB window instead of the card's
        // ROM. Encoded 0 = unclaimed, 1-7 = slot (the latch is never anything
        // else). Older blobs stop before this byte and keep the live owner,
        // exactly as they keep the live IOUDIS.
        {
            const int owner = slots.getActiveExpansionSlot();
            sect.push_back(static_cast<uint8_t>(owner >= 1 && owner <= 7 ? owner : 0));
        }
        // 2026-09-17: the //c Mockingboard 4c wake latch.
        sect.push_back(iicExpansionAwake_ ? 1 : 0);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    // Fourth section: the No-Slot Clock. It uses no slot, so it gets no
    // SLOTn section — but it is a bit-serial state machine walked over many
    // reads (see NoSlotClock.h), and it lives behind a Memory pointer, so
    // Memory's trailer is its home. Same length-prefixed, absent-tolerant
    // convention: a machine with no NSC writes a zero length.
    {
        std::vector<uint8_t> sect;
        if (noSlotClock_) noSlotClock_->appendSnapshotState(sect);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    // The original two Sony sections stay in place for old snapshots.
    for (int which = 0; which < 2; ++which) {
        std::vector<uint8_t> sect;
        if (smartPortHub) {
            const pom2::Sony35Drive* d = (which == 0)
                ? smartPortHub->internal35() : smartPortHub->external35();
            if (d) d->appendSnapshotState(sect);
        }
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    // Native //c IOU mouse: part of the motherboard, so file snapshots
    // carry it even though its scheduler/IRQ owner is registered in slot 4.
    {
        std::vector<uint8_t> sect;
        if (iicProfile_) if (auto* p = slots.peripheral(4)) p->saveIicMouseState(sect);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    {   // Optional third Sony: append after the historical mouse section.
        std::vector<uint8_t> sect;
        if (smartPortHub && smartPortHub->external35Second())
            smartPortHub->external35Second()->appendSnapshotState(sect);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
    {   // Zip Chip in the CPU socket (CpuSocketAccelerator.h): no slot, so here.
        std::vector<uint8_t> sect;
        if (accel_) accel_->appendSnapshotState(sect);
        putU32(static_cast<uint32_t>(sect.size()));
        putBytes(sect.data(), sect.size());
    }
}

bool Memory::loadSnapshotState(const uint8_t* data, size_t n,
                              uint32_t* savedRamWorksBanks)
{
    // Direct callers (including rewind) need the same all-or-nothing
    // refusal as files. This scans headers/device state, not RAM contents.
    if (!validateSnapshotState(data, n, savedRamWorksBanks)) return false;
    // Default the report to "stock aux, no RamWorks": a blob that never
    // reaches the bank field (too short, malformed) must not look like a
    // 128-bank capture to the caller that decides whether to refuse it.
    if (savedRamWorksBanks) *savedRamWorksBanks = 1;
    size_t pos = 0;
    // `k <= n - pos`, NOT `pos + k <= n`: k comes straight out of the blob as
    // a uint32_t (the RamWorks backing size, among others), and on a 32-bit
    // size_t — the wasm32 target is one — `pos + 0xFFFFFFFF` wraps to
    // `pos - 1` and sails through, after which the memcpys below read tens of
    // megabytes past the payload. The subtraction form cannot overflow,
    // because pos <= n holds throughout.
    auto need  = [&](size_t k) { return pos <= n && k <= n - pos; };
    auto getU8 = [&]() -> uint8_t { return data[pos++]; };
    auto getU16 = [&]() -> uint16_t {
        uint16_t v = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2; return v;
    };
    auto getU32 = [&]() -> uint32_t {
        uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[pos + i]) << (8 * i);
        pos += 4; return v;
    };
    auto getU64 = [&]() -> uint64_t {
        uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
        pos += 8; return v;
    };
    auto getBytes = [&](void* dst, size_t k) {
        std::memcpy(dst, data + pos, k); pos += k;
    };

    if (!need(1)) return false;
    if (getU8() != kMemStateBlobVersion) return false;
    // iieMode(1) + iieMemMode(2) + lc flags(4) + display(9) + 2×u64(16) = 32
    if (!need(32)) return false;
    (void)getU8();                       // iieMode — informational (mode is set by the profile)
    iieMemMode    = getU16();
    lcReadRam     = getU8() != 0;
    lcWriteEnable = getU8() != 0;
    lcBank2Active = getU8() != 0;
    lcPrewrite    = getU8() != 0;
    DisplayState ds;
    ds.textMode    = getU8() != 0; ds.mixedMode = getU8() != 0; ds.page2 = getU8() != 0;
    ds.hiRes       = getU8() != 0; ds.eightyCol = getU8() != 0; ds.an3   = getU8() != 0;
    ds.altChar     = getU8() != 0; ds.dhgr      = getU8() != 0; ds.eightyStore = getU8() != 0;
    cycleCounter     = getU64();
    vblNextEventCycle_ = 0;     // re-derive the VBL/frame gate from the new counter
    paddles_.setLatchCycle(getU64());
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        display = ds;
        // The restored clock invalidates the beam-racing event log:
        // events stamped with pre-restore (possibly FUTURE, on rewind)
        // emuCycles break the non-decreasing-stamp invariant the
        // publication carry loop in advanceCycles() relies on. After a
        // rewind every stale event sat past the new frame start, so
        // publishedEvents_ came out empty on every publication for the
        // whole rewound span (beam-raced effects froze at the frame-start
        // state) while videoEvents_ carried the stale tail forever and
        // grew without bound. Start the log fresh from the restored
        // display state and clock.
        displayAtFrameStart_ = ds;
        publishedFrameStart_ = ds;
        resetVideoEventLogForClockJump();
    }

    if (!need(lcBank1.size() + lcBank2.size() + lcHigh.size())) return false;
    getBytes(lcBank1.data(), lcBank1.size());
    getBytes(lcBank2.data(), lcBank2.size());
    getBytes(lcHigh.data(),  lcHigh.size());

    if (!need(5)) return false;
    const uint32_t savedBanks = getU32();
    const uint8_t  savedBank  = getU8();
    // Tell the caller what aux geometry this blob was captured with. The
    // mismatch branch below keeps only the saved CURRENT bank — up to 8 MB
    // of guest RAM dropped without a word — and snapshotMachineId hashes the
    // profile key alone, so nothing upstream could see it coming.
    if (savedRamWorksBanks) *savedRamWorksBanks = savedBanks ? savedBanks : 1u;

    if (savedBanks <= 1) {
        if (!need(aux.size() + auxLcBank1.size() + auxLcBank2.size() + auxLcHigh.size()))
            return false;
        getBytes(aux.data(),        aux.size());
        getBytes(auxLcBank1.data(), auxLcBank1.size());
        getBytes(auxLcBank2.data(), auxLcBank2.size());
        getBytes(auxLcHigh.data(),  auxLcHigh.size());
        if (ramWorksBanks_ > 1) {
            // Live config has RamWorks; mirror the restored data into the
            // current backing slot so a later bank swap doesn't lose it.
            uint8_t* slot = ramWorksBacking_.data()
                          + static_cast<size_t>(ramWorksBank_) * kRamWorksBankStride;
            std::memcpy(slot,           aux.data(),        0x10000);
            std::memcpy(slot + 0x10000, auxLcBank1.data(), 0x1000);
            std::memcpy(slot + 0x11000, auxLcBank2.data(), 0x1000);
            std::memcpy(slot + 0x12000, auxLcHigh.data(),  0x2000);
        }
    } else {
        if (!need(4)) return false;
        const uint32_t backingSize = getU32();
        if (!need(backingSize)) return false;
        if (savedBanks == ramWorksBanks_ && backingSize == ramWorksBacking_.size()) {
            std::memcpy(ramWorksBacking_.data(), data + pos, backingSize);
            ramWorksBank_ = static_cast<uint8_t>(savedBank % ramWorksBanks_);
            const uint8_t* slot = ramWorksBacking_.data()
                                + static_cast<size_t>(ramWorksBank_) * kRamWorksBankStride;
            std::memcpy(aux.data(),        slot,           0x10000);
            std::memcpy(auxLcBank1.data(), slot + 0x10000, 0x1000);
            std::memcpy(auxLcBank2.data(), slot + 0x11000, 0x1000);
            std::memcpy(auxLcHigh.data(),  slot + 0x12000, 0x2000);
        } else if (static_cast<size_t>(savedBank) * kRamWorksBankStride
                       + kRamWorksBankStride <= backingSize) {
            // Bank-count mismatch — best effort: lift just the saved current
            // bank's visible slice into the live aux arrays.
            const uint8_t* slot = data + pos
                                + static_cast<size_t>(savedBank) * kRamWorksBankStride;
            std::memcpy(aux.data(),        slot,           0x10000);
            std::memcpy(auxLcBank1.data(), slot + 0x10000, 0x1000);
            std::memcpy(auxLcBank2.data(), slot + 0x11000, 0x1000);
            std::memcpy(auxLcHigh.data(),  slot + 0x12000, 0x2000);
        }
        pos += backingSize;
    }

    // Optional trailer: the //c on-board SmartPort arming gate.
    if (need(1)) iicSmartPortArmed_ = getU8() != 0;

    // Second trailer: length-prefixed //c-class device sections, absent in
    // older blobs (live values kept). A started section must be valid; a
    // failure propagates to MachineSnapshot's transactional rollback.
    auto readSection = [&](auto&& apply, bool wantsEmpty = false) -> bool {
        if (pos == n) return true;             // optional trailer absent
        if (!need(4)) return false;            // torn length prefix
        const uint32_t len = getU32();
        if (len == 0) return !wantsEmpty || apply(data + pos, std::size_t{0});
        if (n - pos < len) return false;
        if (!apply(data + pos, static_cast<size_t>(len))) return false;
        pos += len;
        return true;
    };
    // The IWM stays in restore mode until the MIG section below has re-pointed
    // the hub, whose push otherwise re-anchored the restored 3.5" revolution.
    struct IwmRestore { pom2::IWMDevice* d; ~IwmRestore() { if (d) d->setRestoring(false); } };
    const IwmRestore iwmRestore{iwmDevice}; if (iwmDevice) iwmDevice->setRestoring(true);
    if (!readSection([&](const uint8_t* p, size_t k) {
            return !iwmDevice || iwmDevice->loadSnapshotState(p, k);
        })) return false;
    if (!readSection([&](const uint8_t* p, size_t k) {
            return !iicProfile_ || iicProfile_->loadSnapshotState(p, k) != 0;
        })) return false;
    // Paging/IOU flip-flops (see appendSnapshotState). Older blobs end
    // before this section and keep the live values — the documented
    // back-compat convention for this trailer.
    if (!readSection([&](const uint8_t* p, size_t k) {
            if (k < 4) return false;
            intC8Rom      = p[0] != 0;
            ioudis        = p[1] != 0;
            vblIrqMask    = p[2] != 0;
            vblIrqPending = p[3] != 0;
            // Re-drive the LINE to match the restored latch. A rewind that
            // lands `pending = false` while IRQ_SRC_VBL is still asserted
            // used to wedge the machine permanently: the $C070 ack is
            // gated on the latch, so it would be skipped forever and the
            // //c would spin in its IRQ vector.
            if (iicProfile_ && cpu)
                cpu->setIrqLine(M6502::IRQ_SRC_VBL, vblIrqPending);
            // Annunciators, appended after the four above. A blob written
            // before 2026-09-06 stops at k == 4 and keeps the live values,
            // which is the same back-compat rule the whole trailer follows.
            if (k >= 7) {
                std::lock_guard<std::mutex> lk(stateMutex);   // AN0 is in DisplayState:
                display.an0 = displayAtFrameStart_.an0 = publishedFrameStart_.an0 = p[4] != 0;
                an1 = p[5] != 0;
                an2 = p[6] != 0;
            }
            // VBL edge detector + the //c $C800 window latch (see
            // appendSnapshotState). Older blobs stop before these.
            if (k >= 9) {
                vblWasActive   = p[7] != 0;
                iicCardWindow_ = p[8] != 0;
            }
            // The $C800 expansion-window owner (see appendSnapshotState).
            // Validated inside SlotBus: a blob naming an empty slot or a
            // card that does not drive /IOSTB restores as unclaimed.
            if (k >= 10) slots.restoreExpansionOwner(static_cast<int>(p[9]));
            iicExpansionAwake_ = k >= 11 ? (iicProfile_ && p[10] != 0)  // older: 4c always awake
                : (iicProfile_ && slots.peripheralForIicRomPage(4) != nullptr);
            return true;
        })) return false;
    // No-Slot Clock (see appendSnapshotState). A blob written by a build
    // without it, or by a machine with the chip disabled, ends here.
    if (!readSection([&](const uint8_t* p, size_t k) {
            return !noSlotClock_ || noSlotClock_->loadSnapshotState(p, k);
        })) return false;
    // On-board Sony 3.5" mechanisms (see appendSnapshotState).
    for (int which = 0; which < 2; ++which) {
        if (!readSection([&](const uint8_t* p, size_t k) {
                pom2::Sony35Drive* d = smartPortHub
                    ? (which == 0 ? smartPortHub->internal35()
                                  : smartPortHub->external35())
                    : nullptr;
                return !d || d->loadSnapshotState(p, k);
            })) return false;
    }

    if (!readSection([&](const uint8_t* p, size_t k) {
            auto* mouse = iicProfile_ ? slots.peripheral(4) : nullptr;
            return !mouse || mouse->loadIicMouseState(p, k);
        })) return false;
    if (!readSection([&](const uint8_t* p, size_t k) {
            auto* d = smartPortHub ? smartPortHub->external35Second() : nullptr;
            return !d || d->loadSnapshotState(p, k);
        })) return false;
    if (!readSection([&](const uint8_t* p, size_t k) {
            // None saved: reset state, not what the last machine left.
            if (accel_ && k == 0) { accel_->onReset(cycleCounter); return true; }
            return !accel_ || accel_->loadSnapshotState(p, k);
        }, /*wantsEmpty=*/true)) return false;
    return true;
}
