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

// IIcClassProfile — MemoryProfile for the //c family: 16 KB rev-255 //c,
// 32 KB //c rev-0/3/4, and //c+. Holds the //c-specific state that used
// to live in Memory (alt-firmware bank, ROMBANK flag, //c+ MIG gate
// array) and the on-board IWM / SmartPort hub pointers. The MIG decode
// (`migRead`/`migWrite`) is a verbatim port of MAME
// `apple2e.cpp:532-624` (apple2e_state::mig_r / mig_w).
#pragma once

#include "MemoryProfile.h"

#include <array>
#include <cstddef>

namespace pom2 { class IWMDevice; class SmartPortHub; class SmartPortBusPort; }

class IIcClassProfile final : public MemoryProfile {
public:
    // `payload` = 16 KB firmware covering $C000-$FFFF (bank 0, active at
    // reset). `payloadSize` >= 0x3c00 (guaranteed by Memory's //c-class
    // probe). `altBank16k` = upper 16 KB (bank 1) for the $C028 toggle,
    // or nullptr (16 KB rev-255 //c has no alt bank). //c+ is probed from
    // `payload[0x3bbf] == 0x05` (MAME `apple2e.cpp:1275-1299`).
    IIcClassProfile(const uint8_t* payload, std::size_t payloadSize,
                    const uint8_t* altBank16k,
                    pom2::IWMDevice* iwm, pom2::SmartPortHub* hub,
                    bool iwmAuthoritative);

    bool forcesIntCxRom() const override { return true; }
    void onResetSoftSwitches() override;
    bool romBankToggle() override;

    bool ioReadIWM (uint16_t addr, uint64_t cyc, uint8_t& out) override;
    bool ioWriteIWM(uint16_t addr, uint8_t value, uint64_t cyc) override;

    bool internalRomRead (uint16_t addr, uint8_t floatBus, uint8_t& out) override;
    bool internalRomWrite(uint16_t addr, uint8_t value) override;
    bool languageCardRomRead(uint16_t addr, uint8_t& out) override;

    void setIwm(pom2::IWMDevice* iwm) override            { iwm_ = iwm; }
    void setSmartPortHub(pom2::SmartPortHub* hub) override { hub_ = hub; }
    void setIwmAuthoritative(bool on) override            { iwmAuthoritative_ = on; }
    void setExternalSmartPort(pom2::SmartPortBusPort* port) override { extPort_ = port; }
    bool servesExternalSmartPort() override;
    bool isIIcPlus() const override { return isPlus_; }

    /// //c+ MIG state (page pointer + 2 KB RAM) for snapshot / rewind.
    /// Without these the MIG falls out of a rewind and the IWM freezes
    /// on //c-class profiles — pinned by `iwm_mig_snapshot`.
    void   appendSnapshotState(std::vector<uint8_t>& out) const override;
    size_t loadSnapshotState(const uint8_t* data, size_t n) override;

private:
    uint8_t migRead (uint16_t migOffset, uint8_t floatBus);
    void    migWrite(uint16_t migOffset, uint8_t value);

    // Alt firmware (bank 1) for the $C028 ROMBANK toggle. Bytes
    // 0x100-0x0FFF mirror $C100-$CFFF (alt internal I/O ROM); bytes
    // 0x1000-0x3FFF mirror $D000-$FFFF (alt firmware).
    std::array<uint8_t, 0x4000> altFirmware_{};
    bool     hasAltBank_ = false;   // only 32 KB dumps provide bank 1
    bool     romBank_    = false;   // false = bank 0 (cold-start), true = bank 1
    bool     isPlus_     = false;   // //c+ — gates the MIG windows

    // Apple MIG gate array (//c+). See MAME `apple2e.cpp:532-624`.
    std::array<uint8_t, 0x800> migRam_{};
    uint16_t migPage_     = 0;
    bool     migIntDrive_ = false;
    bool     migHdSel_    = false;
    bool     mig35Sel_    = false;   // $C240/$C260 — the hub's sel35_, saved

    pom2::IWMDevice*    iwm_ = nullptr;
    pom2::SmartPortHub* hub_ = nullptr;
    pom2::SmartPortBusPort* extPort_ = nullptr;   // plain //c external drive
    bool               iwmAuthoritative_ = true;
};
