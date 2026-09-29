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

// VidexVideotermCard — the Videx Videoterm 80-column card for the Apple ][ /
// ][+ (1980), catalog key `videoterm`. A port of MAME
// `a2bus_videx80_device` (src/devices/bus/a2bus/a2videoterm.cpp).
//
// The card: an HD6845S CRTC (Hd6845Crtc.h) clocked at 17.43 MHz / 9, 2 KB of
// video RAM, a 2 KB firmware EPROM of which the Apple sees 1 KB, and two 2 KB
// character generators (a "normal" set and an alternate one — inverse video
// on the stock card). Its picture is 80 × 24 cells of 9 × 9 dots = 720 × 216.
//
// Bus map (a2videoterm.cpp:379-471):
//   $C0n0 W   6845 address latch; $C0n1 R/W the selected register
//   $C0nX     ANY access, read or write, selects the VRAM bank
//             (offset >> 2) & 3 — so $C0n0/1 bank 0, $C0n4 1, $C0n8 2, $C0nC 3
//   $CnXX  R  firmware[$300 + low byte] (a mirror of $CB00-$CBFF); writes are
//             ignored — the firmware writes there only to claim $C800
//   $C800-$CBFF R  firmware[0..$3FF]
//   $CC00-$CDFF RW the 512-byte window onto the selected quarter of VRAM
//   $CE00-$CFFF    open bus
//   take_c800() = true (:122). A bus RESET resets the 6845 (:372-375).
// The v2.4 firmware hard-codes $C0B0/$C0B1, so it only works in SLOT 3
// (Videoterm manual, 3rd ed.; AppleWin issue #105); the card itself decodes
// wherever it sits.
//
// Glyphs (crtc_update_row, :473-500): 16 bytes per glyph, 128 glyphs per
// set, the byte index is the 6845 raster address; 9 dots per cell, bits 7..0
// MSB first and then bit 0 AGAIN as the ninth dot. VRAM bit 7 picks the
// alternate set. The cursor inverts the whole cell. VRAM wraps at 2 KB.
//
// Which picture the monitor shows is NOT something MAME models — MAME gives
// the card its own screen. On the real machine a "soft video switch" put the
// card's output on the monitor when the Apple was in TEXT mode with AN0 set:
// the firmware's init ends `STA $C059` (vterm24.dis LC82A), its re-entry does
// it again (LCA89), the return-to-40 escape does `STA $C058` (LC987), and the
// II+ RESET handler reads $C058 (Autostart $FA6F), so Ctrl-Reset gives the
// Apple picture back. izapple2 implements the same rule
// (cardVidexVideotern.go:131-139). `ownsScreen()` below is that rule.

#ifndef POM2_VIDEX_VIDEOTERM_CARD_H
#define POM2_VIDEX_VIDEOTERM_CARD_H

#include "CardVideoSource.h"
#include "Hd6845Crtc.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

class VidexVideotermCard : public SlotPeripheral, public pom2::CardVideoSource
{
public:
    static constexpr std::size_t kRomBytes     = 2048;  ///< EPROM (1 KB dumps pad with $FF)
    static constexpr std::size_t kCharSetBytes = 2048;  ///< 128 glyphs × 16 rasters
    static constexpr std::size_t kVramBytes    = 2048;  ///< a2videoterm.cpp:125
    static constexpr int kCellWidth  = 9;               ///< m_char_width
    static constexpr int kColumns    = 80;
    static constexpr int kRows       = 24;
    static constexpr int kPictureWidth  = 720;          ///< set_raw(..., 0, 720, ...) :200
    static constexpr int kPictureHeight = 216;          ///< set_raw(..., 0, 216) :200
    /// HD6845S(config, m_crtc, 17.43_MHz_XTAL / 9) — :204.
    static constexpr double kCharClockHz = 17.43e6 / 9.0;

    explicit VidexVideotermCard(int slot);

    int getSlot() const { return slot_; }

    /// Firmware: the 2 KB clone-card dump (MAME BIOS 0, `6.ic6.bin`, 60 Hz
    /// CRTC table) or the 1 KB Videx v2.4 (BIOS 1, 50 Hz). A 1 KB image is
    /// padded with $FF, MAME's ROMREGION_ERASEFF (:45). Other sizes refused.
    bool loadFirmware(const std::vector<uint8_t>& bytes);
    /// Character generators, 2 KB each. `alternate` empty = derive it as the
    /// bitwise inverse of `normal`: MAME's default alternate set `4.ic4.bin`
    /// IS that, byte for byte on every raster the card displays (0-8).
    bool loadCharRoms(const std::vector<uint8_t>& normal,
                      const std::vector<uint8_t>& alternate);
    bool firmwareLoaded() const { return firmwareLoaded_; }
    bool alternateIsDerived() const { return altDerived_; }

    // ── Read-only views (display, tests, debug panels) ──────────────────
    const pom2::Hd6845Crtc& crtc() const { return crtc_; }
    const uint8_t* vram() const { return vram_.data(); }
    /// Byte offset of the VRAM quarter the $CC00 window shows (0/512/1024/1536).
    int vramBank() const { return bank_; }

    // ─── SlotPeripheral ─────────────────────────────────────────────────
    std::string_view name() const override { return "Videx Videoterm"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead      (uint8_t low8) override;
    uint8_t expansionRomRead (uint16_t offset) override;
    void    expansionRomWrite(uint16_t offset, uint8_t v) override;
    bool    takesC800() const override { return true; }   // a2videoterm.cpp:122
    void    onReset() override;
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── pom2::CardVideoSource ───────────────────────────────────────────
    bool ownsScreen(bool textMode, bool an0) const override { return textMode && an0; }
    int  pictureWidth()  const override { return kPictureWidth; }
    int  pictureHeight() const override { return kPictureHeight; }
    void paintPicture(uint32_t* dst, uint32_t lit, uint32_t dark,
                      uint64_t emuCycles, double cpuHz) const override;

    /// The CRTC frame index at emulated time `emuCycles` — what the cursor
    /// blink is a function of. Public for the render test.
    uint64_t crtcFrameAt(uint64_t emuCycles, double cpuHz) const;

private:
    int slot_;
    std::array<uint8_t, kRomBytes> rom_{};
    std::array<uint8_t, kCharSetBytes> charNormal_{};
    std::array<uint8_t, kCharSetBytes> charAlt_{};
    std::array<uint8_t, kVramBytes> vram_{};
    int  bank_ = 0;
    bool firmwareLoaded_ = false;
    bool altDerived_ = true;
    pom2::Hd6845Crtc crtc_;

    void selectBank(uint8_t low4) { bank_ = ((low4 >> 2) & 3) * 512; }
};

#endif // POM2_VIDEX_VIDEOTERM_CARD_H
