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

// EzColorCard — "E-Z Color Graphics Interface", Steve Ciarcia, BYTE, August
// 1982: a TI TMS9918A video display processor with 16 KB of VRAM on a slot
// card, 10.738635 MHz crystal, its own video output. Catalog key `ezcolor`.
// MAME `a2bus/ezcgi.cpp`: $C0n0 is the VDP's data port (VRAM read / write),
// $C0n1 its register port (status read / address and register writes),
// every other $C0nX reads $FF; the VDP's /INT is the slot IRQ; a bus reset
// resets the VDP. No ROM.
//
// The VDP is POM1's TMS9918 (TMS9918.h, imported as-is): sprites with the
// 4/5th-sprite and collision flags, the four modes, the access window, the
// progressive raster. The access window is ENFORCED — silicon-strict mode,
// on from construction: a port access the chip has not had time to serve is
// dropped, as on the real part, and counted (`vdp().droppedWriteCount()`).
// The slots come from openMSX's TMS9918 tables, so the gap needed depends on
// the mode and the beam (~2 cycles in VBlank or with the display off, more
// during the active lines) rather than a fixed figure. Strict mode also
// makes R1 bit 7 select 4 KB / 16 KB addressing: the card's 16 KB need it. Writes catch the raster up to the beam first and a
// status read syncs the sprite scan to it (POM1's Étapes 0/1), with the
// in-flight cycles of the CPU's current instruction.
//
// The card's monitor is not the Apple's: the picture (288 × 216, the 256 ×
// 192 image in its border) is a CardVideoSource that never takes the Apple's
// screen by its own rule. It is shown in Display ▸ Card Monitor — Apple video
// on the main screen plus that panel is the two-monitor set-up the card had —
// or on the main screen with the switch at "Card video".

#ifndef POM2_EZCOLOR_CARD_H
#define POM2_EZCOLOR_CARD_H

#include "CardVideoSource.h"
#include "SlotPeripheral.h"
#include "TMS9918.h"

#include <cstdint>
#include <vector>

class M6502;

class EzColorCard : public SlotPeripheral, public pom2::CardVideoSource
{
public:
    EzColorCard();

    void setCpu(M6502* cpu) { cpu_ = cpu; }
    TMS9918&       vdp()       { return vdp_; }
    const TMS9918& vdp() const { return vdp_; }

    // ─── SlotPeripheral ─────────────────────────────────────────────────
    std::string_view name() const override { return "E-Z Color (TMS9918A)"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    void    advanceCycles(int cycles) override;
    void    onReset() override;
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── CardVideoSource ────────────────────────────────────────────────
    bool ownsScreen(bool, bool) const override { return false; }   // its own monitor
    int  pictureWidth()  const override { return TMS9918::kFullWidth; }
    int  pictureHeight() const override { return TMS9918::kFullHeight; }
    void paintPicture(uint32_t* dst, uint32_t lit, uint32_t dark,
                      uint64_t emuCycles, double cpuHz) const override;

private:
    int inFlight() const;
    void updateIrq() { assertIrq(vdp_.irqAsserted()); }

    TMS9918 vdp_;
    M6502* cpu_ = nullptr;
};

#endif // POM2_EZCOLOR_CARD_H
