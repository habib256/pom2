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

// VidexUltratermCard — the Videx UltraTerm (1984), catalog key `ultraterm`:
// the Videoterm's successor, 80 to 160 columns and up to 48 lines, with its
// own video switch. A port of MAME `a2bus_videx160_device`
// (src/devices/bus/a2bus/a2ultraterm.cpp, R. Belmont), with izapple2's
// `cardVidexUltraterm.go` (after the UltraTerm manual) where MAME is silent.
//
// The card: an HD6845S (Hd6845Crtc.h) clocked at 17.43 or 28.7595 MHz / 9,
// 4 KB of video RAM, a 4 KB firmware EPROM, a 4 KB character ROM (a high-
// and a low-density set of 128 glyphs × 16 rasters).
//
// Bus map (a2ultraterm.cpp:250-375):
//   $C0n0 W   6845 address latch           $C0n1 R/W  the selected register
//   $C0n2 R/W control 1 (MCP)              $C0n3 R/W  control 2 (attributes)
//             A0-A1 decode the register; while MCP bit 4 is 0 (Videoterm
//             mode) EVERY access also selects the 512-byte VRAM block from
//             A2-A3, as on the Videoterm. (MAME reads only $C0n1-3 exactly;
//             izapple2 decodes A0-A1 — modelled: the four repeat.)
//   $CnXX  R  firmware[slot × $100 + low byte]: the low 2 KB holds one copy
//             per slot. Writes ignored (they claim $C800).
//   $C800-$CBFF R  firmware[$800 + offset]
//   $CC00-$CFFF R  MCP bit 7 set: firmware[$C00-$FFF]; clear: video RAM.
//               W  always video RAM. Window: 512-byte block (Videoterm mode,
//               mirrored) or the 256-byte page MCP bits 0-3 name.
//   take_c800() = true.
//
// MCP (control 1): bit 7 firmware page at $CC00, bit 6 the video switch
// (the card's picture on the monitor — `ownsScreen`), bit 5 the 28.7595 MHz
// clock, bit 4 256-byte pages (else Videoterm 512-byte blocks), bits 0-3 the
// page. Reset writes 0: Apple video, Videoterm mode.
//
// Control 2: bit 7 attributes from the DIP switches (not modelled: plain
// text); bit 2 high-density glyphs; for characters with VRAM bit 7 set, bit 4
// highlight and bit 5 inverse; with bit 7 clear, bit 0 highlight and bit 1
// inverse (crtc_update_row).
//
// Glyph dots: bits 7..0 then bit 0 again as the ninth (MAME). izapple2 only
// repeats it for codes below $20 (the line-drawing set) — no manual page to
// settle it, MAME's rule kept. The cursor raster is all lit (MAME
// `data = 0xFF`). Lowlight is 3/4 of highlight (izapple2; MAME's grey levels
// $55/$FF would make normal text a third as bright).
//
// The picture is R1 × 9 dots by R6 × (R9 + 1) rasters: an interlaced mode
// (80 × 48, 128 × 32…) paints both fields' rasters into one frame.

#ifndef POM2_VIDEX_ULTRATERM_CARD_H
#define POM2_VIDEX_ULTRATERM_CARD_H

#include "CardVideoSource.h"
#include "Hd6845Crtc.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

class VidexUltratermCard : public SlotPeripheral, public pom2::CardVideoSource
{
public:
    static constexpr std::size_t kRomBytes  = 4096;
    static constexpr std::size_t kCharBytes = 4096;
    static constexpr std::size_t kVramBytes = 4096;
    static constexpr int kCellWidth = 9;
    static constexpr int kMaxWidth  = 160 * kCellWidth;
    static constexpr int kMaxHeight = 64 * 16;
    static constexpr double kLowClockHz  = 17.43e6;
    static constexpr double kHighClockHz = 28.7595e6;
    /// MAME ROM sets: a2ulttrm (original) and a2ultrme (enhanced //e,
    /// MouseText in place of the lo-res glyphs).
    static constexpr const char* kRomPath     = "roms/videx_ultraterm_frm_b537.bin";
    static constexpr const char* kCharPath    = "roms/videx_ultraterm_chs_7859.bin";
    static constexpr const char* kRomPathEnh  = "roms/videx_ultraterm_enh_frm_b5c9.bin";
    static constexpr const char* kCharPathEnh = "roms/videx_ultraterm_enh_chs_5604.bin";

    // MCP bits (a2ultraterm.cpp:65-69).
    static constexpr uint8_t kMemSel  = 0x80;
    static constexpr uint8_t kVidSel  = 0x40;
    static constexpr uint8_t kClkSel  = 0x20;
    static constexpr uint8_t kPages256 = 0x10;

    explicit VidexUltratermCard(int slot) : slot_(slot) { rom_.fill(0xFF); }

    /// Both 4 KB; false (and nothing loaded) otherwise.
    bool loadRoms(const std::vector<uint8_t>& firmware, const std::vector<uint8_t>& chars);

    // ── Read-only views (display, tests) ────────────────────────────────
    const pom2::Hd6845Crtc& crtc() const { return crtc_; }
    const uint8_t* vram() const { return vram_.data(); }
    uint8_t control1() const { return ctrl1_; }
    uint8_t control2() const { return ctrl2_; }
    /// Byte offset of the VRAM window's base.
    int windowBase() const { return (ctrl1_ & kPages256) ? (ctrl1_ & 0x0F) * 256 : block512_; }
    /// VRAM as the display walks it: 2 KB in Videoterm mode, 4 KB in pages.
    uint16_t displayMask() const { return (ctrl1_ & kPages256) ? 0x0FFF : 0x07FF; }

    // ─── SlotPeripheral ─────────────────────────────────────────────────
    std::string_view name() const override { return "Videx UltraTerm"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead      (uint8_t low8) override;
    uint8_t expansionRomRead (uint16_t offset) override;
    void    expansionRomWrite(uint16_t offset, uint8_t v) override;
    bool    takesC800() const override { return true; }
    void    onReset() override;
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── pom2::CardVideoSource ───────────────────────────────────────────
    /// The card's own video switch, MCP bit 6 — no AN0, unlike the Videoterm.
    bool ownsScreen(bool, bool) const override { return (ctrl1_ & kVidSel) != 0; }
    int  pictureWidth()  const override;
    int  pictureHeight() const override;
    void paintPicture(uint32_t* dst, uint32_t lit, uint32_t dark,
                      uint64_t emuCycles, double cpuHz) const override;

private:
    void selectBlock(uint8_t low4)
    {
        if (!(ctrl1_ & kPages256)) block512_ = ((low4 >> 2) & 3) * 512;
    }
    std::size_t windowIndex(uint16_t offset) const
    {
        const std::size_t mask = (ctrl1_ & kPages256) ? 0xFF : 0x1FF;
        return (offset & mask) + std::size_t(windowBase());
    }

    int slot_;
    std::array<uint8_t, kRomBytes> rom_{};
    std::array<uint8_t, kCharBytes> chars_{};
    std::array<uint8_t, kVramBytes> vram_{};
    uint8_t ctrl1_ = 0;
    uint8_t ctrl2_ = 0;
    int block512_ = 0;
    pom2::Hd6845Crtc crtc_;
};

#endif // POM2_VIDEX_ULTRATERM_CARD_H
