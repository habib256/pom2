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

// Apple II NTSC composite artifact-decode primitives — the MAME-ported core
// shared by every hi-res colour path (HGR 280, DHGR 560, the composite
// signal generator). Factored out of Apple2Display.cpp so the bit-stream
// builders and the artifact LUT live in one place instead of being copied
// between renderHiRes / renderDhgr / fillCompositeSignal.
//
// Header-only on purpose: these are tiny, hot, per-scanline primitives that
// benefit from inlining, and keeping them header-only avoids threading a new
// .cpp through the ~12 build targets that compile Apple2Display.cpp. All
// definitions are `inline` / `inline constexpr` (C++17) → single definition
// across translation units, no ODR violation, no duplicated storage.
//
// Source of truth: MAME `apple2video.cpp` (PR #10773 by benrg). Citations
// preserved verbatim from the original Apple2Display.cpp block.
//
// Convention: bit 0 of an HGR byte is the LEFTMOST pixel, bit 6 the
// RIGHTMOST, bit 7 the per-byte half-dot delay flag.

#ifndef POM2_APPLE2_VIDEO_DECODE_H
#define POM2_APPLE2_VIDEO_DECODE_H

#include <array>
#include <cstdint>

namespace pom2::a2v {

// 280 visible color clocks × 2 sub-pixels.
inline constexpr int kStreamLen = 560;

// Bit doubler. `kBitDoubler[i]` is the 14-bit word obtained by replacing
// each of the 7 low bits of i with a doubled (b, b) pair.
inline constexpr std::array<uint16_t, 128> makeBitDoubler()
{
    std::array<uint16_t, 128> t{};
    for (unsigned i = 1; i < 128; ++i)
        t[i] = static_cast<uint16_t>(t[i >> 1] * 4 + (i & 1) * 3);
    return t;
}
inline constexpr std::array<uint16_t, 128> kBitDoubler = makeBitDoubler();

// Verbatim from MAME `apple2video.cpp:376-419` `artifact_color_lut[2][128]`.
// Each byte packs four 4-bit lo-res palette indices, one per NTSC sub-
// cycle phase; `rotl4b` selects which. Row 0 is the canonical
// composite/NTSC table; row 1 is MAME's "medium-color biased" variant
// (4n colored pixels for runs of medium colors against black/white, at
// the cost of uglier 40-col text). Picked by `composite_color_mode()`
// in MAME — driven here by `HiResMode::ColorNTSC` vs `ColorCompMedium`.
inline constexpr uint8_t kArtifactColorLut[2][128] = {
{
    0x00,0x00,0x00,0x00,0x88,0x00,0x00,0x00,0x11,0x11,0x55,0x11,0x99,0x99,0xdd,0xff,
    0x22,0x22,0x66,0x66,0xaa,0xaa,0xee,0xee,0x33,0x33,0x33,0x33,0xbb,0xbb,0xff,0xff,
    0x00,0x00,0x44,0x44,0xcc,0xcc,0xcc,0xcc,0x55,0x55,0x55,0x55,0x99,0x99,0xdd,0xff,
    0x00,0x22,0x66,0x66,0xee,0xaa,0xee,0xee,0x77,0x77,0x77,0x77,0xff,0xff,0xff,0xff,
    0x00,0x00,0x00,0x00,0x88,0x88,0x88,0x88,0x11,0x11,0x55,0x11,0x99,0x99,0xdd,0xff,
    0x00,0x22,0x66,0x66,0xaa,0xaa,0xaa,0xaa,0x33,0x33,0x33,0x33,0xbb,0xbb,0xff,0xff,
    0x00,0x00,0x44,0x44,0xcc,0xcc,0xcc,0xcc,0x11,0x11,0x55,0x55,0x99,0x99,0xdd,0xdd,
    0x00,0x22,0x66,0x66,0xee,0xaa,0xee,0xee,0xff,0xff,0xff,0x77,0xff,0xff,0xff,0xff,
}, {
    // composite_color_mode = 1: medium-color biased variant. 8 entries
    // differ from row 0 (highlighted by MAME's comment: 0110000 maps
    // to a permutation of 0110 instead of black; counterparts under
    // the symmetries do the same).
    0x00,0x00,0x00,0x00,0x88,0x00,0xcc,0x00,0x11,0x11,0x55,0x11,0x99,0x99,0xdd,0xff,
    0x22,0x22,0x66,0x66,0xaa,0xaa,0xee,0xee,0x33,0x33,0x33,0x33,0xbb,0xbb,0xff,0xff,
    0x00,0x00,0x44,0x44,0xcc,0xcc,0xcc,0xcc,0x55,0x55,0x55,0x55,0x99,0x99,0xdd,0xff,
    0x66,0x22,0x66,0x66,0xee,0xaa,0xee,0xee,0x77,0x77,0x77,0x77,0xff,0xff,0xff,0xff,
    0x00,0x00,0x00,0x00,0x88,0x88,0x88,0x88,0x11,0x11,0x55,0x11,0x99,0x99,0xdd,0x99,
    0x00,0x22,0x66,0x66,0xaa,0xaa,0xaa,0xaa,0x33,0x33,0x33,0x33,0xbb,0xbb,0xff,0xff,
    0x00,0x00,0x44,0x44,0xcc,0xcc,0xcc,0xcc,0x11,0x11,0x55,0x55,0x99,0x99,0xdd,0xdd,
    0x00,0x22,0x66,0x66,0xee,0xaa,0xee,0xee,0xff,0x33,0xff,0x77,0xff,0xff,0xff,0xff,
}};

// `rotl4b(n, count)` — extract the 4-bit nibble of `n` at logical
// position `count` (mod 4). Maps to the NTSC phase rotation MAME uses.
inline constexpr unsigned rotl4b(unsigned n, unsigned count)
{
    return (n >> ((-static_cast<int>(count)) & 3)) & 0x0fu;
}

// Decode 40 HGR bytes into a 40-element array of 14-bit doubled words,
// applying the half-dot delay when the source byte's MSB is set.
// `bit7Mask` honours the IIe DHIRES annunciator's rev-0 emulation: pass
// `0x7F` to force-mask the MSB (no half-dot delay, no orange/blue
// palette) when DHIRES=on + 80COL=off, mirroring MAME `apple2video.cpp`
// `bit7_mask = m_dhires ? 0 : 0x80`. Default 0xFF = transparent.
inline void buildHgrWordRow(const uint8_t* ram, uint16_t rowAddr,
                            uint16_t (&words)[40], uint8_t bit7Mask = 0xFFu)
{
    unsigned last_output_bit = 0;
    for (int col = 0; col < 40; ++col) {
        const uint8_t b = ram[rowAddr + col] & bit7Mask;
        uint16_t word = kBitDoubler[b & 0x7Fu];
        if (b & 0x80u) {
            word = static_cast<uint16_t>(((word << 1) | last_output_bit) & 0x3FFFu);
        }
        words[col] = word;
        last_output_bit = (word >> 13) & 1u;
    }
}

// Build the 560-sub-pixel raw bit stream — one entry per sub-pixel,
// 0/1. Used by the monochrome paths (which don't need the windowed
// LUT). Equivalent to laying buildHgrWordRow's output end-to-end.
inline void buildBitStream(const uint8_t* ram, uint16_t rowAddr,
                           uint8_t (&stream)[kStreamLen], uint8_t bit7Mask = 0xFFu)
{
    uint16_t words[40];
    buildHgrWordRow(ram, rowAddr, words, bit7Mask);
    int out = 0;
    for (int col = 0; col < 40; ++col) {
        const uint16_t w = words[col];
        for (int b = 0; b < 14; ++b) {
            stream[out++] = static_cast<uint8_t>((w >> b) & 1u);
        }
    }
}

// The 7M LORES mode. [B] Sather, *Understanding the Apple IIe*, p. 8-22
// (Table 8.8 note 3 and the paragraph below it): a //e in LORES40 GRAPHICS
// with 80COL reset and AN3 low (FRCTXT' low) shifts the lo-res pattern at
// 7 MHz, undelayed — "the processing of LORES patterns at HIRES40 speed".
// The load/shift register is loaded with the column's 14 MHz pattern (dot i
// of column c shows nibble bit (14c + i) & 3) and at 7 MHz each of its first
// seven bits lasts two dots: an undelayed HGR byte whose bit k is nibble bit
// (2c + k) & 3. Sather's own checks hold: 0000 black, 1111 white, 0101
// violet in even columns and green in odd ones, 1010 the other way round,
// every other pattern fragmented. [D] AppleWin @ 98b72658 NTSC.cpp
// `updateScreenDoubleLores40` builds the same bytes. [F] The bit order
// inside the 7 MHz load is derived from the 14 MHz one, not measured;
// settled by: a capture of Accurapple's multi_mode.s TEST 8 on a real //e.
inline uint8_t sevenMLoresByte(uint8_t nibble, int col)
{
    uint8_t b = 0;
    for (int k = 0; k < 7; ++k)
        b = static_cast<uint8_t>(b | (((nibble >> ((2 * col + k) & 3)) & 1u) << k));
    return b;
}

// One scanline of 7M LORES as the 40 undelayed HGR bytes the shift register
// sees: `textRowAddr` the text row of scanline `y`, the nibble by y & 4.
inline void buildSevenMLoresRow(const uint8_t* ram, uint16_t textRowAddr, int y,
                                uint8_t (&out)[40])
{
    for (int col = 0; col < 40; ++col) {
        const uint8_t byte = ram[textRowAddr + col];
        out[col] = sevenMLoresByte((y & 4) ? byte >> 4 : byte & 0x0F, col);
    }
}

// Average two RGBA pixels per channel (the optical chroma-bandwidth-limit
// real CRTs apply when downsampling 560 sub-pixels → 280 framebuffer pixels).
inline uint32_t avgRgb(uint32_t a, uint32_t b)
{
    const uint32_t r = ((a & 0xFFu) + (b & 0xFFu)) >> 1;
    const uint32_t g = (((a >> 8)  & 0xFFu) + ((b >> 8)  & 0xFFu)) >> 1;
    const uint32_t bl = (((a >> 16) & 0xFFu) + ((b >> 16) & 0xFFu)) >> 1;
    return (uint32_t(0xFF) << 24) | (bl << 16) | (g << 8) | r;
}

} // namespace pom2::a2v

#endif // POM2_APPLE2_VIDEO_DECODE_H
