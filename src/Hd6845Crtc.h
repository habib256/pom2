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

// Hd6845Crtc — the register file of a Hitachi HD6845S CRT controller, the
// chip on the Videx Videoterm (MAME `a2videoterm.cpp:204`, `HD6845S(...)`).
//
// Only what a card that owns the chip needs to PAINT a frame is modelled:
// the 18 registers with MAME's write masks, the readable subset, the reset,
// the HD6845S cursor rule and the cursor blink. There is no raster timer: the
// frame is painted whole by its owner, and the blink phase is a pure function
// of the CRTC frame index, which the owner derives from emulated time
// (`emuCycles`, the convention every POM2 device follows) — no per-frame
// tick, so a paused machine's cursor freezes, and a rewind lands on the phase
// of the moment it restores.
//
// Source of truth: MAME `src/devices/video/mc6845.cpp` —
//   address_w            143-146
//   register_r           192-212 (R12/R13 readable on the HD6845S: 1214)
//   register_w           215-270 (the masks)
//   HD check_cursor_visible 546-569 (no wrap, unlike the MC6845's 507-543)
//   update_cursor_state  784-813 (R10 bits 6-5: on / off / 16 / 32 frames)
//   device_reset         1029-1054 — NOT followed for R14-R17: the Hitachi
//                        datasheet keeps every register across RES (reset())
// The cursor blink counts FIELDS with the Hitachi period (cursorBlinkOn),
// not MAME's frames and toggle-every-16/32.

#ifndef POM2_HD6845_CRTC_H
#define POM2_HD6845_CRTC_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

class Hd6845Crtc
{
public:
    /// R0-R17. R16/R17 are the light-pen latch (read-only, never strobed
    /// here — no Apple II card wires LPSTB).
    static constexpr int kRegisters = 18;

    /// RES: R0-R17 untouched (Hitachi HD6845S, U70 p. 451/463 — the cursor
    /// address survives, unlike MAME's device_reset); the address latch
    /// goes to zero.
    void reset();

    /// $C0n0 write on the Videoterm — MAME `address_w` (143-146).
    void addressWrite(uint8_t v) { latch_ = static_cast<uint8_t>(v & 0x1F); }
    /// $C0n1 read — MAME `register_r` (192-212). Only R12-R17 answer; the
    /// rest are write-only and read 0.
    uint8_t registerRead() const;
    /// $C0n1 write — MAME `register_w` (215-270), with its masks.
    void registerWrite(uint8_t v);

    uint8_t latch() const { return latch_; }
    /// Raw stored value of Rn (masked as written). 0 for n out of range.
    uint8_t reg(int n) const
    {
        return (n >= 0 && n < kRegisters) ? r_[static_cast<std::size_t>(n)] : 0;
    }

    // ── Decoded fields ───────────────────────────────────────────────────
    int horizTotal()     const { return r_[0]; }
    int horizDisplayed() const { return r_[1]; }
    int vertTotal()      const { return r_[4]; }
    int vertAdjust()     const { return r_[5]; }
    int vertDisplayed()  const { return r_[6]; }
    int maxRaster()      const { return r_[9]; }
    /// R8 bits 1-0 == 11: Interlace Sync AND Video (MAME mc6845.cpp:81
    /// `MODE_INTERLACE_AND_VIDEO`). The HD6845S then paints BOTH fields'
    /// rasters into one frame, so a character row is R9 + 2 rasters tall, not
    /// R9 + 1 — the datasheet puts it as "when the total number of rasters is
    /// RN, RN-2 shall be programmed" (quoted in mc6845.cpp:1278-1280, where
    /// `m_interlace_adjust = 2`).
    bool interlaceAndVideo() const { return (r_[8] & 0x03) == 0x03; }
    /// Rasters per character row, interlace adjust included. R8 was latched
    /// and read by nothing, so every interlaced row was one raster short: the
    /// UltraTerm's own firmware selects this mode for four of its seven
    /// display tables (80x24 HQ, 80x32, 80x48, 128x32) and programs a SMALLER
    /// R9 because the chip adds 2 — its 80x48 table has R9 = 7, i.e. the nine
    /// rasters the 9x9 glyph needs. With R9+1 the ninth raster of every glyph
    /// on all 48 rows was never painted (descenders, underlines, the bottom
    /// edge of the box-drawing and MouseText cells) and the picture was 48
    /// scanlines short.
    int rasterCount()    const { return maxRaster() + (interlaceAndVideo() ? 2 : 1); }
    int cursorStart()    const { return r_[10] & 0x1F; }
    int cursorEnd()      const { return r_[11]; }
    /// R10 bits 6-5: 0 steady, 1 off, 2 blink period 16 fields, 3 32 fields.
    int cursorMode()     const { return (r_[10] >> 5) & 3; }
    uint16_t startAddress()  const
    {
        return static_cast<uint16_t>((r_[12] << 8) | r_[13]);
    }
    uint16_t cursorAddress() const
    {
        return static_cast<uint16_t>((r_[14] << 8) | r_[15]);
    }

    /// Frames per second the chip scans with its current timing registers,
    /// for a character clock of `charClockHz`. 0 when the registers do not
    /// describe a frame yet (all zero before the firmware's init).
    double frameRateHz(double charClockHz) const;
    /// FIELDS per second — the clock of the cursor blink. Equal to
    /// frameRateHz() unless interlaced; in interlace sync & video a field
    /// shows half the rasters of each row, so it runs at ~2× the frame.
    double fieldRateHz(double charClockHz) const;

    /// Cursor blink state for CRTC field `fieldIndex` (count fields at
    /// fieldRateHz()), as a pure function: the blink PERIOD is 16 or 32
    /// fields, light + dark (Hitachi Table 7), so the state is bit 3 (fast)
    /// or bit 4 (slow) of the field counter, off first as MAME starts.
    bool cursorBlinkOn(uint64_t fieldIndex) const;

    /// HD6845S `check_cursor_visible` (546-569): is the cursor lit on raster
    /// `ra` of the character row whose first address is `lineAddr`, given
    /// the blink state? Returns the cursor's column in `cursorX` when it is.
    bool cursorVisible(int ra, uint16_t lineAddr, bool blinkOn,
                       int& cursorX) const;

    // ── Snapshot ─────────────────────────────────────────────────────────
    /// 19 bytes: R0-R17 then the address latch.
    static constexpr std::size_t kStateBytes = kRegisters + 1;
    void saveState(std::vector<uint8_t>& out) const;
    /// Validates everything first (each register must carry only the bits
    /// its mask allows — a blob this class wrote always does) and applies
    /// nothing unless all of it is good.
    bool loadState(const uint8_t* p, std::size_t len);

    /// MAME `register_w` masks, R0-R17 (R16/R17 are read-only: 0 = never
    /// written).
    static uint8_t writeMask(int n);

private:
    std::array<uint8_t, kRegisters> r_{};
    uint8_t latch_ = 0;
};

} // namespace pom2

#endif // POM2_HD6845_CRTC_H
