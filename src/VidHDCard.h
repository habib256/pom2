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

// VidHDCard — Blue Shift's VidHD (2018) as a //e video card, catalog key
// `vidhd`: the Apple IIgs Super Hi-Res modes (320 × 200 and 640 × 200, 16
// palettes of 16 colours out of 4096) on a machine with no IIgs in it. A
// port of AppleWin's partial model (source/VidHD.cpp + the SHR scanner in
// source/NTSC.cpp `updateScreenSHR`, :1866-1895) — no MAME device exists.
//
// What the card is, at the level POM2 models:
//
//   • A detection signature. AppleWin `VidHDCard::IORead` (VidHD.cpp:68-79):
//     READS of $C0n0 / $C0n1 / $C0n2 answer $24 $EA $4C — bytes the real
//     card's firmware carries — and every other device-select access is an
//     unclaimed read (`IO_Null`, the floating bus).
//   • Four IIgs video registers latched off the motherboard bus — the card
//     sees every WRITE to $C022 SCREENCOLOR, $C029 NEWVIDEO, $C034
//     BORDERCOLOR and $C035 SHADOW (VidHD.cpp:82-102 `VideoIOWrite`;
//     Memory.cpp:622-650 `IOWrite_C02x` / `IOWrite_C03x`). The motherboard
//     keeps its own meaning for those addresses: $C02x still toggles the
//     cassette output, $C03x still clicks the speaker. The card has no
//     read-back ("VidHD has no support for reading the IIgs video
//     registers", VidHD.cpp:81). POM2 delivers them through
//     `SlotPeripheral::onVideoSoftSwitchWrite` (Memory::softSwitchAccess).
//   • A bus RESET clears NEWVIDEO (VidHD.cpp:57-61), so Ctrl-Reset gives the
//     Apple's own picture back.
//   • Super Hi-Res is ON while NEWVIDEO bits 7 and 6 are both set
//     (VidHD.h `IsSHR`: "Enable SHR(b7) | Linearize SHR video memory(b6)") —
//     whatever TEXT, MIXED or AN0 say. The picture is read from the //e's
//     AUXILIARY RAM at $2000-$9FFF, which is where a IIgs's bank $E1 shadows
//     from: on a //e the card watches the aux writes go by. Layout, per
//     NTSC.cpp:905-912 + :1872-1887:
//         pixels   $2000 + 160·y   (y = 0..199)
//         SCB      $9D00 + y       bit 7 = 640 mode, bit 5 = colour fill,
//                                  bits 3-0 = palette
//         palettes $9E00 + 32·p    16 little-endian words $0RGB
//     320 mode: high nibble first, each dot two output pixels; with fill,
//     index 0 repeats the previous dot's colour. 640 mode: four 2-bit dots
//     per byte, MSB pair first, each from its own quarter of the palette —
//     offsets 8, 12, 0, 4 (IIgs Hardware Reference, table 4-21).
//
// The picture is 640 × 400: each SHR line painted twice, the way pom2gs's
// VGC (`drawShrLine`) and AppleWin (NTSC.cpp:109) double it. It is read
// from the aux bank the CPU currently has selected — the one RAMWRT and
// 80STORE write — i.e. the RamWorks bank in use (AppleWin `MemGetAuxPtr`
// reads `memaux`, the active bank). The card reaches that RAM through the
// bus (`SlotPeripheral::busAuxRam`), so it needs no Memory dependency and
// Apple2Display links nothing new (CardVideoSource.h).
//
// NOT modelled (DEV.md § VidHD): the IIgs text / border colours (SCREENCOLOR
// and BORDERCOLOR are latched, snapshotted, never painted), the SHADOW
// register's meaning, B&W DHGR (NEWVIDEO bit 5), the VidHD's own extended
// text modes (80×45, 120×67, 240×135), its enable/disable switch, and the
// ][ / ][+ aux mirror (AppleWin mirrors 80STORE/RAMRD/RAMWRT/PAGE2 into a
// card-side RAM there) — the card is offered on //e-class profiles only.

#ifndef POM2_VIDHD_CARD_H
#define POM2_VIDHD_CARD_H

#include "CardVideoSource.h"
#include "SlotPeripheral.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

class VidHDCard : public SlotPeripheral, public pom2::CardVideoSource
{
public:
    static constexpr int kShrLines       = 200;
    static constexpr int kPictureWidth   = 640;
    static constexpr int kPictureHeight  = 2 * kShrLines;   ///< each line doubled
    static constexpr uint16_t kPixelBase   = 0x2000;   ///< NTSC.cpp:911
    static constexpr uint16_t kBytesPerLine = 160;
    static constexpr uint16_t kScbBase     = 0x9D00;   ///< NTSC.cpp:1877
    static constexpr uint16_t kPaletteBase = 0x9E00;   ///< NTSC.cpp:1885

    explicit VidHDCard(int slot) : slot_(slot) {}

    int getSlot() const { return slot_; }

    /// The 64 KB the picture is read from, for a card with no bus (tests).
    /// Plugged into a Memory-backed bus, the card uses the bus's aux RAM and
    /// this override stays null.
    void setAuxRamOverride(const uint8_t* aux) { auxOverride_ = aux; }

    // ── Registers (debug panels, tests) ─────────────────────────────────
    uint8_t screenColor() const { return screenColor_; }
    uint8_t newVideo()    const { return newVideo_; }
    uint8_t borderColor() const { return borderColor_; }
    uint8_t shadow()      const { return shadow_; }
    /// VidHD.h `IsSHR`: NEWVIDEO b7 (SHR) and b6 (linear memory) both set.
    bool shrEnabled() const { return (newVideo_ & 0xC0) == 0xC0; }

    /// $0RGB little-endian word → 0xAABBGGRR, each 4-bit component × 17
    /// (pom2gs VGC.cpp `rgb12`).
    static uint32_t rgb12(uint8_t lo, uint8_t hi)
    {
        const uint32_t r = (hi & 0x0Fu) * 17u;
        const uint32_t g = ((lo >> 4) & 0x0Fu) * 17u;
        const uint32_t b = (lo & 0x0Fu) * 17u;
        return 0xFF000000u | (b << 16) | (g << 8) | r;
    }

    // ─── SlotPeripheral ─────────────────────────────────────────────────
    std::string_view name() const override { return "VidHD"; }
    uint8_t deviceSelectRead(uint8_t low4) override;
    void    onVideoSoftSwitchWrite(uint16_t addr, uint8_t value) override;
    void    onReset() override { newVideo_ = 0; }   // VidHD.cpp:57-61
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── pom2::CardVideoSource ───────────────────────────────────────────
    bool ownsScreen(bool /*textMode*/, bool /*an0*/) const override
    {
        return shrEnabled() && auxRam() != nullptr;
    }
    int  pictureWidth()  const override { return kPictureWidth; }
    int  pictureHeight() const override { return kPictureHeight; }
    void paintPicture(uint32_t* dst, uint32_t lit, uint32_t dark,
                      uint64_t emuCycles, double cpuHz) const override;

private:
    int slot_;
    const uint8_t* auxOverride_ = nullptr;
    uint8_t screenColor_ = 0;   // $C022
    uint8_t newVideo_    = 0;   // $C029
    uint8_t borderColor_ = 0;   // $C034
    uint8_t shadow_      = 0;   // $C035

    const uint8_t* auxRam() const
    {
        return auxOverride_ ? auxOverride_ : busAuxRam();
    }
};

#endif // POM2_VIDHD_CARD_H
