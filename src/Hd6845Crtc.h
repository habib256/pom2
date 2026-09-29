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
//   device_reset         1029-1054 (cursor address, latch, light pen cleared;
//                        the other registers survive)

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

    /// MAME `mc6845.cpp:1029-1054`: cursor address, address latch and light
    /// pen go to zero; the timing registers are untouched.
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
    int cursorStart()    const { return r_[10] & 0x1F; }
    int cursorEnd()      const { return r_[11]; }
    /// R10 bits 6-5: 0 steady, 1 off, 2 blink every 16 frames, 3 every 32.
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

    /// Cursor blink state for CRTC frame `frameIndex` — MAME
    /// `update_cursor_state` (784-813) as a pure function: the state starts
    /// off (974-975) and toggles each time bit 4 (fast) or bit 5 (slow) of
    /// the frame counter changes, so it is simply that bit.
    bool cursorBlinkOn(uint64_t frameIndex) const;

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
