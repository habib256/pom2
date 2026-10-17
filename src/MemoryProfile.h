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

// MemoryProfile — strategy for machine-profile-specific memory behaviour
// that would otherwise leak into Memory's generic dispatcher.
//
// Today only the //c-class (16 KB rev-255 //c, 32 KB //c rev-0/3/4, and
// //c+) needs one: alt-firmware bank switch ($C028 ROMBANK), on-board
// IWM ($C0E0-$C0EF), //c+ MIG windows ($CC00/$CE00), forced INTCXROM
// (no physical slots), and the alt-firmware override of $C100-$FFFF.
// II/II+/IIe run with `Memory::iicProfile_ == nullptr` — a single
// `if (iicProfile_)` branch on the hot path, **zero virtual calls** on
// those profiles. See MemoryProfile_IIcClass for the implementation and
// DEV.md § Memory.
#pragma once

#include <cstddef>   // size_t — <cstdint>/<vector> happen to drag it in on
                     // libstdc++ 14, but not on Debian bookworm's 12; the
                     // Raspberry Pi release build caught the difference.
#include <cstdint>
#include <vector>

namespace pom2 { class IWMDevice; class SmartPortHub; class SmartPortBusPort; }

class MemoryProfile {
public:
    virtual ~MemoryProfile() = default;

    // $C100-$CFFF gate: //c-class forces INTCXROM (it has no slots).
    virtual bool forcesIntCxRom() const = 0;

    // Ctrl-Reset / power-on: drop ROMBANK back to bank 0 (cold-start).
    virtual void onResetSoftSwitches() = 0;

    // $C02x ROMBANK toggle. Returns true (consumed) on //c-class so the
    // caller skips the II/II+ cassette-output fallback.
    virtual bool romBankToggle() = 0;

    // $C0E0-$C0EF on-board IWM. Read: advances the IWM state machine and,
    // when authoritative, sets `out` and returns true; returns false in
    // shadow mode or with no media/IWM (caller falls back to the slot-6
    // LSS byte). Write: dispatches the byte to the IWM.
    virtual bool ioReadIWM (uint16_t addr, uint64_t cyc, uint8_t& out) = 0;
    /// Returns true when the external SmartPort port CLAIMED the write —
    /// Memory then keeps it from the slot-6 Disk II, exactly as on the read
    /// side: the bus's packet bytes are not flux, and a DiskIICard that
    /// saw them spliced them into the 5.25" under its head (bug hunt 3).
    virtual bool ioWriteIWM(uint16_t addr, uint8_t value, uint64_t cyc) = 0;

    // $C100-$CFFF under INTCXROM. Returns true + `out` for //c+ MIG
    // windows ($CC00/$CE00) or alt-firmware bank 1; false ⇒ caller
    // returns its internal I/O ROM byte. `floatBus` is the floating-bus
    // value used for MIG read fall-through (MIG control reads float).
    virtual bool internalRomRead(uint16_t addr, uint8_t floatBus, uint8_t& out) = 0;
    // Returns true if the write hit a MIG window (side effects applied);
    // false ⇒ caller absorbs the write (internal ROM is read-only).
    virtual bool internalRomWrite(uint16_t addr, uint8_t value) = 0;

    // $D000-$FFFF ROM reads (lcReadRam == false): alt-firmware bank 1
    // overrides motherboard ROM. Returns true + `out`, else false.
    virtual bool languageCardRomRead(uint16_t addr, uint8_t& out) = 0;

    // Façade forwards from Memory::setIWM / setSmartPortHub /
    // setIWMAuthoritative (kept for test/back-compat — see DEV.md).
    virtual void setIwm(pom2::IWMDevice* iwm) = 0;
    virtual void setSmartPortHub(pom2::SmartPortHub* hub) = 0;
    virtual void setIwmAuthoritative(bool on) = 0;

    /// The external disk port's SmartPort bus responder (plain 32 KB //c
    /// only — the //c+ has its MIG path and the 16 KB //c no 3.5"
    /// firmware at all). Optional: with none set, the //c keeps the
    /// host-served $C500 substitute. See SmartPortBusPort.h.
    virtual void setExternalSmartPort(pom2::SmartPortBusPort* /*port*/) {}
    /// The //c+ (MIG + shared IWM), as opposed to the plain //c.
    virtual bool isIIcPlus() const { return false; }
    /// Canonical additional firmware for snapshot compatibility checks.
    /// Must not read the bus, alter ROMBANK, or include volatile state.
    virtual void appendSnapshotFirmware(std::vector<uint8_t>&) const {}
    /// True while the machine's own firmware is serving slot 5 over that
    /// port — Memory must then leave the real $C500 page alone.
    virtual bool servesExternalSmartPort() { return false; }

    /// Serialize any profile-private device state that survives across
    /// frames. Only the //c-class profile has any (the //c+ MIG gate
    /// array's 2 KB RAM + its auto-incrementing page pointer); the base
    /// contract is "append nothing, accept nothing" so profiles without
    /// state need no override. Called from `Memory::appendSnapshotState`.
    virtual void appendSnapshotState(std::vector<uint8_t>& out) const { (void)out; }
    /// Same framing/semantic checks as load, with no live-state mutation.
    virtual size_t validateSnapshotState(const uint8_t*, size_t) const { return 0; }
    /// Consume a blob produced by `appendSnapshotState`. Returns the number
    /// of bytes consumed, or 0 if the blob is absent/unrecognised — older
    /// snapshots simply lack the section and keep the live values.
    virtual size_t loadSnapshotState(const uint8_t* data, size_t n)
    {
        (void)data; (void)n; return 0;
    }
};
