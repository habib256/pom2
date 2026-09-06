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

#include "MemoryProfile_IIcClass.h"

#include "SmartPortBusPort.h"

#include "IWMDevice.h"
#include "SmartPortHub.h"

#include <algorithm>
#include <cstring>

IIcClassProfile::IIcClassProfile(const uint8_t* payload, std::size_t payloadSize,
                                 const uint8_t* altBank16k,
                                 pom2::IWMDevice* iwm, pom2::SmartPortHub* hub,
                                 bool iwmAuthoritative)
    : iwm_(iwm), hub_(hub), iwmAuthoritative_(iwmAuthoritative)
{
    // //c+ probe (MAME `apple2e.cpp:1275-1299`): the firmware revision
    // byte at file offset 0x3bbf is 0x05 on //c+ ROMs. Plain //c
    // rev-0/3/4 leave isPlus_ = false (alt firmware, but no MIG).
    if (payloadSize > 0x3bbf && payload[0x3bbf] == 0x05) {
        isPlus_ = true;
    }
    // Stash bank 1 (upper 16 KB) for the $C028 ROMBANK toggle. Only
    // 32 KB dumps provide it; 16 KB rev-255 //c passes nullptr and the
    // alt-firmware read paths stay inert (hasAltBank_ == false).
    if (altBank16k) {
        std::copy(altBank16k, altBank16k + 0x4000, altFirmware_.begin());
        hasAltBank_ = true;
    }
}

bool IIcClassProfile::romBankToggle()
{
    // MAME `apple2e.cpp:1907-1923`: `if (m_isiic) m_romswitch = !m_romswitch`.
    romBank_ = !romBank_;
    // When ROMSWITCH transitions back to bank 0 the MIG state machine
    // resets — page cursor to 0, internal-drive + 3.5"-select cleared
    // (the hub recomputes the active floppy on its next access). RAM
    // contents survive (MAME `apple2e.cpp:1917-1922`).
    // MAME gates this MIG reset on `m_isiicplus` (do_io:1701-1706): the MIG
    // gate-array only exists on the //c+. A plain 32 KB //c (rev 0/3/4) has
    // an alt bank but no MIG, so it must not poke MIG/hub state.
    if (isPlus_ && !romBank_) {
        migPage_ = 0;
        if (hub_) {
            hub_->setMigIntDrive(false);
            hub_->setMig35Sel(false);
        }
    }
    return true;
}

void IIcClassProfile::onResetSoftSwitches()
{
    romBank_ = false;
    // A reset mid-transaction leaves the external drive waiting for bytes
    // that will never come; it starts over with the machine.
    if (extPort_) extPort_->reset();
}

bool IIcClassProfile::servesExternalSmartPort()
{
    // The plain 32 KB //c, with a device on its port. Its bank-0 $C500 page
    // is the real controller firmware (the Liron's, byte for byte) and its
    // bank 1 the SmartPort bus code; with something answering on the bus,
    // that firmware enumerates and boots the drive itself, and the
    // host-served substitute must stay out of its way.
    // …and the //c+ as well: its own firmware probes the rear connector for
    // an intelligent device at boot ($F223 in bank 1, the same 50-poll
    // scan), and boots or lists what answers. With a device answering, the
    // real $C500 must run, not the substitute.
    return hasAltBank_ && extPort_ && extPort_->live();
}

bool IIcClassProfile::ioReadIWM(uint16_t addr, uint64_t cyc, uint8_t& out)
{
    // Plain //c: the external SmartPort port answers its own traffic and
    // nothing else (see IIcExternalSmartPort). The machine's shared IWM
    // stays untouched here — `iic_diskii_no_iwm_conflict` pins why.
    if (hasAltBank_ && !isPlus_ && extPort_)
        return extPort_->read(static_cast<uint8_t>(addr & 0xF), cyc, out);
    // $C0E0-$C0EF on-board IWM (MAME wires A2BUS_IWM at sl6 for 32 KB
    // //c-class — see `apple2e.cpp:5249-5272` + `:6281-6291`). The
    // slot-6 DiskIICard still observes the access for motor sound /
    // turbo / head tracking; the **value** returned to the CPU is the
    // IWM's when authoritative.
    //
    // **//c+ ONLY.** MAME wires its IWM as *the* slot-6 controller,
    // replacing the Disk II. POM2 does not: the DiskIICard stays
    // authoritative for 5.25" (see iwmAuthoritative_), so mirroring here
    // adds a *second* controller on the same soft switches without
    // supplying the data path — and the IWM's phase/motor handling then
    // fights the DiskIICard's over one drive. On a plain //c that
    // corrupts the head position and DOS 3.3 RWTS falls into endless
    // seek/retry storms ($B948-$B956, head oscillating track N<->0):
    // Print Shop could not save its setup or load its print overlay, so
    // printing silently produced nothing. The IWM is only needed where
    // it actually owns a drive — the //c+ MIG / Sony 3.5" path — hence
    // the isPlus_ gate. Pinned by `iic_diskii_no_iwm_conflict`.
    if (!hasAltBank_ || !iwm_ || !isPlus_) return false;
    iwm_->tick(cyc);
    const uint8_t v = iwm_->read(static_cast<uint8_t>(addr & 0xF));
    // The IWM's value is returned ONLY while the hub routes to a 3.5"
    // Sony drive — the one device this IWM actually owns. For everything
    // else (5.25" selected, or nothing selected during the firmware's
    // boot drive-scan) the DiskIICard LSS answers: the 2026-07 bug hunt
    // showed the IWM's bit-cell walker mis-frames DOS 3.3 RWTS reads
    // enough that a SAVE on the //c+ ended in I/O ERROR (write-verify
    // re-reads through the walker), while the full boot + write
    // round-trip is clean through the LSS. This is POM2's split of
    // MAME's single-controller model (apple2e.cpp recalc_active_device
    // hands m_cur_floppy to ONE iwm): each drive class keeps the
    // controller that owns it.
    // The rear connector's SmartPort device, on the //c+'s own IWM: while
    // the firmware addresses the bus (or a transaction is in flight) the
    // port answers; the Sony drives never see those bytes.
    if (extPort_ && extPort_->sharedAfterRead(*iwm_, v, out)) return true;
    if (iwmAuthoritative_ && hub_ && hub_->active35Selected()) {
        out = v;
        return true;
    }
    // Shadow: IWM state advanced (the firmware's status probes and the
    // MIG handshake still see a live chip), but the byte returned to the
    // CPU comes from the slot-6 DiskIICard LSS path (caller falls
    // through).
    return false;
}

bool IIcClassProfile::ioWriteIWM(uint16_t addr, uint8_t value, uint64_t cyc)
{
    if (hasAltBank_ && !isPlus_ && extPort_)
        return extPort_->write(static_cast<uint8_t>(addr & 0xF), value, cyc);
    // //c+ only — see the rationale in ioReadIWM above.
    if (!hasAltBank_ || !iwm_ || !isPlus_) return false;
    iwm_->tick(cyc);
    const bool forBus = extPort_ && extPort_->sharedWantsWrite(*iwm_);
    iwm_->setBusCapture(forBus);
    iwm_->write(static_cast<uint8_t>(addr & 0xF), value);
    if (extPort_) extPort_->sharedAfterWrite(*iwm_, static_cast<uint8_t>(addr & 0xF),
                                             value, forBus);
    return forBus;
}

bool IIcClassProfile::internalRomRead(uint16_t addr, uint8_t floatBus, uint8_t& out)
{
    // //c+ MIG window ($CC00/$CE00) when bank 1 (ROMSWITCH) is active —
    // MAME `apple2e.cpp:3148-3151 c800_b2_int_r`, gated `m_isiicplus &&
    // m_romswitch`. Plain //c rev-0/3/4 also has a bank 1 (alt firmware)
    // but NO MIG — its reads must return ROM bytes (D-2-2).
    if (isPlus_ && romBank_) {
        if (addr >= 0xCC00 && addr <= 0xCCFF) {
            out = migRead(static_cast<uint16_t>(addr - 0xCC00), floatBus);
            return true;
        }
        if (addr >= 0xCE00 && addr <= 0xCEFF) {
            out = migRead(static_cast<uint16_t>(addr - 0xCC00), floatBus);
            return true;
        }
    }
    // Bank-1 alt firmware bytes (plain //c rev-0/3/4 AND //c+ outside the
    // MIG windows). Bank 0 → caller returns its internal I/O ROM.
    if (hasAltBank_ && romBank_) {
        out = altFirmware_[addr - 0xC000];
        return true;
    }
    return false;
}

bool IIcClassProfile::internalRomWrite(uint16_t addr, uint8_t value)
{
    // //c+ MIG window writes (MAME `apple2e.cpp:3186-3190`, gated
    // `m_isiicplus && m_romswitch`): drive enable/disable, IWM reset,
    // MIG RAM cache. Plain //c bank-1 writes fall through and are
    // absorbed by the caller (ROM is read-only on real silicon).
    if (isPlus_ && romBank_) {
        if (addr >= 0xCC00 && addr <= 0xCCFF) {
            migWrite(static_cast<uint16_t>(addr - 0xCC00), value);
            return true;
        }
        if (addr >= 0xCE00 && addr <= 0xCEFF) {
            migWrite(static_cast<uint16_t>(addr - 0xCC00), value);
            return true;
        }
    }
    return false;
}

bool IIcClassProfile::languageCardRomRead(uint16_t addr, uint8_t& out)
{
    // //c ROMBANK alt firmware overrides motherboard ROM at $D000-$FFFF.
    // The LC RAM path is unaffected — banking only swaps the ROM side.
    if (hasAltBank_ && romBank_) {
        out = altFirmware_[addr - 0xC000];
        return true;
    }
    return false;
}

uint8_t IIcClassProfile::migRead(uint16_t migOffset, uint8_t floatBus)
{
    // Verbatim port of MAME `apple2e.cpp:532-569 apple2e_state::mig_r`.
    // Side-effects on read are part of the MIG contract — the firmware
    // walks the MIG RAM page cursor and flips 3.5" head select through
    // them.
    // The composed index is masked (not just the page cursor) so migRam_
    // can never be indexed past its end however migPage_ was set.
    if (migOffset >= 0x200 && migOffset < 0x220) {
        return migRam_[(migPage_ + (migOffset & 0x1F)) & 0x7FF];
    }
    if (migOffset >= 0x220 && migOffset < 0x240) {
        const uint8_t rv = migRam_[(migPage_ + (migOffset & 0x1F)) & 0x7FF];
        migPage_ = static_cast<uint16_t>((migPage_ + 0x20) & 0x7FF);
        return rv;
    }
    if (migOffset >= 0x240 && migOffset < 0x260) {
        migHdSel_ = false;       // 3.5" head 0
        if (hub_) hub_->setMigHdSel(false);
    }
    if (migOffset >= 0x260 && migOffset < 0x280) {
        migHdSel_ = true;        // 3.5" head 1
        if (hub_) hub_->setMigHdSel(true);
    }
    if (migOffset == 0x2A0) {
        migPage_ = 0;
    }
    return floatBus;
}

void IIcClassProfile::migWrite(uint16_t migOffset, uint8_t value)
{
    // Verbatim port of MAME `apple2e.cpp:571-624 apple2e_state::mig_w`.
    if (migOffset == 0x40) {
        // IWM reset (MAME `apple2e.cpp:647-650`). The //c+ alt firmware
        // writes here on every boot; without it stale mode/control/whd
        // state leaks into the fresh IWM probe sequence (E-5-4).
        if (iwm_) iwm_->reset();
        return;
    }
    if (migOffset >= 0x80 && migOffset < 0xA0) {
        migIntDrive_ = true;
        if (hub_) hub_->setMigIntDrive(true);
        return;
    }
    if (migOffset >= 0xC0 && migOffset < 0xE0) {
        migIntDrive_ = false;
        if (hub_) hub_->setMigIntDrive(false);
        return;
    }
    if (migOffset >= 0x200 && migOffset < 0x220) {
        migRam_[(migPage_ + (migOffset & 0x1F)) & 0x7FF] = value;
        return;
    }
    if (migOffset >= 0x220 && migOffset < 0x240) {
        migRam_[(migPage_ + (migOffset & 0x1F)) & 0x7FF] = value;
        migPage_ = static_cast<uint16_t>((migPage_ + 0x20) & 0x7FF);
        return;
    }
    if (migOffset >= 0x240 && migOffset < 0x260) {         // 3.5" m_35sel=false
        if (hub_) hub_->setMig35Sel(false);
        return;
    }
    if (migOffset >= 0x260 && migOffset < 0x280) {         // 3.5" m_35sel=true
        if (hub_) hub_->setMig35Sel(true);
        return;
    }
    if (migOffset == 0x2A0) {
        migPage_ = 0;
        return;
    }
    // Other offsets: NOP.
}

// ─────────────────────────────────────────────────────────────────────────
// Snapshot / rewind — MIG gate array (//c+ only)
// ─────────────────────────────────────────────────────────────────────────

namespace {
constexpr uint8_t kMigBlobMagic[4] = { 'M', 'I', 'G', '1' };
constexpr size_t  kMigBlobBytes    = 4 + 2 + 0x800;   // magic + page + RAM
// Optional v1.1 tail: romBank_ + migIntDrive_ + migHdSel_. Old blobs end
// at kMigBlobBytes and keep the live values for these three.
constexpr size_t  kMigBlobTail     = 3;
}  // namespace

void IIcClassProfile::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMigBlobMagic, kMigBlobMagic + 4);
    out.push_back(static_cast<uint8_t>(migPage_));
    out.push_back(static_cast<uint8_t>(migPage_ >> 8));
    out.insert(out.end(), migRam_.begin(), migRam_.end());
    // romBank_ is the highest-impact field here: the //c+ alt firmware
    // runs from bank 1 during MIG/3.5" work, and a rewind across a $C028
    // toggle restored a PC captured under one bank while the ROM reader
    // served the other — wrong 16 KB of firmware at $C100-$FFFF.
    out.push_back(romBank_     ? 1 : 0);
    out.push_back(migIntDrive_ ? 1 : 0);
    out.push_back(migHdSel_    ? 1 : 0);
    // v1.2 tail: the external SmartPort port — its private IWM (plain //c)
    // and the bus transaction in flight. Self-identifying, so a blob
    // without it (or a machine without a port) is the v1.1 layout.
    if (extPort_) extPort_->appendSnapshotState(out);
}

size_t IIcClassProfile::loadSnapshotState(const uint8_t* data, size_t n)
{
    // Identify the blob BEFORE resetting anything: the external port used
    // to be cleared first, so a foreign / absent MIG section tore down a
    // live //c SmartPort transaction as a side effect of a blob that was
    // then rejected two lines later.
    if (data == nullptr || n < kMigBlobBytes) return 0;
    if (std::memcmp(data, kMigBlobMagic, 4) != 0) return 0;
    if (extPort_) extPort_->reset();   // re-filled below when the blob has it
    // migRead/migWrite index `migRam_[migPage_ + (offset & 0x1F)]`, so a
    // corrupt page pointer would read past the array. `& 0x7FF` alone is
    // not enough: the index only stays inside a 0x800 array because the
    // live cursor is *32-byte aligned* (starts at 0, only ever advances by
    // 0x20), so the highest legal page is 0x7E0. Re-impose the alignment
    // on the way in rather than trust the blob — a restored 0x7FF would
    // otherwise index up to 0x81E, past migRam_ and over migPage_ /
    // migIntDrive_ / migHdSel_ / iwm_ / hub_.
    migPage_ = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[4]) |
         static_cast<uint16_t>(data[5]) << 8) & 0x7E0);
    std::memcpy(migRam_.data(), data + 6, migRam_.size());
    if (n >= kMigBlobBytes + kMigBlobTail) {
        romBank_     = data[kMigBlobBytes]     != 0;
        migIntDrive_ = data[kMigBlobBytes + 1] != 0;
        migHdSel_    = data[kMigBlobBytes + 2] != 0;
        size_t used = kMigBlobBytes + kMigBlobTail;
        if (extPort_ && n > used)
            used += extPort_->loadSnapshotState(data + used, n - used);
        return used;
    }
    return kMigBlobBytes;
}

