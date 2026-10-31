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

// Ccs7710Card — California Computer Systems Model 7710 Asynchronous Serial
// Interface: a Motorola 6850 ACIA in any slot. Catalog key `ccs7710`. MAME
// `ccs7710.cpp`: the ACIA answers the whole $C0n0-$C0nF range on A0 alone —
// even = control (write) / status (read), odd = transmit / receive data —
// so the $C0nE/$C0nF a CP/M BIOS uses are the same two registers.
//
// What it is for in POM2: the "type 3" card of Microsoft's SoftCard II CP/M
// (6850 serial: Apple Communications Interface or CCS 7710), which its loader
// recognises by $Cn05/$Cn07 = $18/$38 and which in slot 1 is the buffered
// printer (sbabcoc's SoftCard2 HARDWARE-REFERENCE.md). What the card sends
// goes to a spool the host ImageWriter takes, like the Super Serial's tap.
//
// Firmware. The card's two 256×4 PROMs (MAME `7710-03h.u5` / `7710-03l.u6`,
// high and low nibble of each byte) are in no download POM2 uses; when both
// sit in roms/ they are loaded and execute — the LLE reference. Without
// them, or with `ccs7710_firmware` = "pom2", the card serves POM2's own
// page, written to behave as the 7710-03 does (see buildOwnFirmware; the
// `ccs7710_card` test runs both and compares).
//
// ACIA (MC6850 data sheet): control CR1:0 = 11 master reset; CR6:5 = 01
// enables the transmit interrupt; CR7 the receive one. Status: b0 RDRF, b1
// TDRE, b2 /DCD and b3 /CTS (both low: the cable is wired), b7 IRQ. The line
// is infinitely fast: TDRE is always set. Bytes from the host wait in a
// queue and arrive one at a time.

#ifndef POM2_CCS7710_CARD_H
#define POM2_CCS7710_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class Ccs7710Card : public SlotPeripheral
{
public:
    static constexpr const char* kPromHiPath = "roms/ccs7710_03h.u5";
    static constexpr const char* kPromLoPath = "roms/ccs7710_03l.u6";

    explicit Ccs7710Card(int slot);

    /// The two 256-byte nibble PROMs (each byte's low nibble counts). False,
    /// keeping POM2's page, unless both are 256 bytes.
    bool loadProms(const std::vector<uint8_t>& hi, const std::vector<uint8_t>& lo);
    bool promsLoaded() const { return promsLoaded_; }
    /// Back to POM2's page (the Abstraction Levels switch, setting
    /// `ccs7710_firmware` = "pom2").
    void useOwnFirmware();
    bool romLayoutError() const { return romLayoutError_; }

    /// A byte from the far end of the line.
    void receive(uint8_t b);
    std::size_t pendingInput() const { return rx_.size() + (rdrf_ ? 1 : 0); }

    /// Everything the card has sent, and the ImageWriter's drain of it.
    std::size_t bytesWritten() const { return spool_.size(); }
    std::size_t drainSpoolFrom(std::size_t from, std::vector<uint8_t>& out) const;
    bool drainPrinterSpool(std::size_t from, std::vector<uint8_t>& out,
                           std::size_t& next) const override
    {
        next = drainSpoolFrom(from, out);
        return true;
    }

    uint8_t status() const;
    uint8_t control() const { return control_; }

    // ─── SlotPeripheral ─────────────────────────────────────────────────
    std::string_view name() const override { return "CCS 7710 serial"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override { return rom_[low8]; }
    bool    peekSlotRom(uint8_t low8, uint8_t& out) const override { out = rom_[low8]; return true; }
    void    onPowerCycle() override;
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

private:
    void buildOwnFirmware();
    void masterReset();
    void updateIrq();

    int slot_;
    std::array<uint8_t, 256> rom_{};
    bool promsLoaded_ = false;
    bool romLayoutError_ = false;

    uint8_t control_ = 0x03;     // powers up held in master reset
    uint8_t rdr_ = 0;
    bool    rdrf_ = false;
    bool    overrun_ = false;
    std::deque<uint8_t> rx_;
    std::vector<uint8_t> spool_;
};

#endif // POM2_CCS7710_CARD_H
