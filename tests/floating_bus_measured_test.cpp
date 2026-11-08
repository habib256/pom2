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

// floating_bus_measured — POM2's floating bus against bytes READ OFF A REAL
// European //e (rev B, PAL).
//
// [A] Stéphane Champailler (Accurapple), `composite/video_scanner/vidscan.s`
//     @ gitlab.com/wiz21/accurapple 69f45f3 (2026-11): a program run on the
//     iron fills HGR page 1 (and text page 1) so that every byte holds its own
//     address — even address = low byte, odd = high byte — then reads $C070
//     on EVERY cycle of a window (8 passes of LDA/STA, 8 cycles apart, one
//     pass per frame) and prints what the bus returned. Three windows are
//     recorded in the file's header. Only the numbers are used here; the
//     program itself is AGPL and is not copied.
//
// These are the grade-A counterpart of `video_scanner_sather`, which pins
// the same scanner to Sather's printed tables (grade B). The windows are
// matched by SHAPE, anywhere in one frame, so nothing here depends on how
// the real program synchronised to the beam. What they settle on the iron:
//   * the last two displayed bytes of a line, then the next line's HBL
//     whose first address is fetched twice (Sather rule 3);
//   * on a //e, no A12 in the HBL scan (the 1985 book; MAME agreed);
//   * MIXED: a text row's HBL and display scan sits right after the last
//     HGR line (159 -> 160);
//   * MIXED does not reach into VBL: after line 191 (text row 23) the VBL
//     line scans HIRES addresses again ($2060, the HGR VBL base).

#include "CpuClock.h"
#include "Memory.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;
constexpr int X = -1;   // a byte the record does not let us pin (see below)

std::vector<int> scanFrame(Memory& mem, bool mixed)
{
    mem.setIIEMode(true);
    mem.resetSoftSwitches();
    mem.setVideoStandard(VideoStandard::PAL);
    // The program's fill: each byte holds its own address, little endian.
    for (uint32_t a = 0x2000; a < 0x4000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(a & 1 ? (a - 1) >> 8 : a & 0xFF));
    for (uint32_t a = 0x0400; a < 0x0800; ++a)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(a & 1 ? (a - 1) >> 8 : a & 0xFF));
    (void)mem.memRead(0xC050);              // graphics
    (void)mem.memRead(0xC057);              // HIRES
    (void)mem.memRead(0xC054);              // page 1
    (void)mem.memRead(mixed ? 0xC053 : 0xC052);
    const int frame = pom2VideoTiming(VideoStandard::PAL).cyclesPerFrame;
    std::vector<int> bus(static_cast<size_t>(frame) + 400);
    for (size_t c = 0; c < bus.size(); ++c) {
        mem.setCycleCounter(c);
        bus[c] = mem.peekFloatingBus();
    }
    return bus;
}

int countMatches(const std::vector<int>& bus, const std::vector<int>& want)
{
    int n = 0;
    for (size_t i = 0; i + want.size() <= bus.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < want.size() && ok; ++k)
            ok = want[k] == X || bus[i + k] == want[k];
        n += ok;
    }
    return n;
}

void expectWindow(const char* what, const std::vector<int>& bus, const std::vector<int>& want)
{
    // The bus vector covers a frame plus a margin, so a window may recur
    // once at the wrap; one or two hits, never zero.
    const int n = countMatches(bus, want);
    if (n == 0) { std::printf("FAIL: %s — the measured sequence never appears\n", what); ++failures; }
    else std::printf("[ OK ] %s (%d bytes)\n", what, static_cast<int>(want.size()));
}

} // namespace

int main()
{
    // ── Window 1: HGR, lines 15 -> 16 -> 17 (MIXED off) ─────────────────
    // $3CA6/$3CA7 end line 15; line 16's HBL starts at $2168 TWICE, runs to
    // $217F, then the display $2100-$2127; line 17's HBL again from $2568.
    //
    // [F] One recorded byte is NOT the RAM's: line 17's fifth HBL read
    // ($256B, which holds $25) is printed `26`. A misread on the iron or a
    // typo copying the screen — unknown, so it is a wildcard here. Settled
    // by: re-running the program and photographing the screen.
    {
        Memory mem;
        const auto bus = scanFrame(mem, false);
        expectWindow("HGR line 15 -> 16 -> 17, HBL first address twice", bus, {
            0xA6,0x3C,0x68,0x68,0x21,0x6A,0x21,0x6C,0x21,0x6E,0x21,0x70,0x21,
            0x72,0x21,0x74,0x21,0x76,0x21,0x78,0x21,0x7A,0x21,0x7C,0x21,0x7E,
            0x21,0x00,0x21,0x02,0x21,0x04,0x21,0x06,0x21,0x08,0x21,0x0A,0x21,
            0x0C,0x21,0x0E,0x21,0x10,0x21,0x12,0x21,0x14,0x21,0x16,0x21,0x18,
            0x21,0x1A,0x21,0x1C,0x21,0x1E,0x21,0x20,0x21,0x22,0x21,0x24,0x21,
            0x26,0x21,0x68,0x68,0x25,0x6A,   X,0x6C,0x25,0x6E,0x25,0x70,0x25,
            0x72,0x25,0x74,0x25,0x76,0x25,0x78,0x25,0x7A,0x25,0x7C,0x25,0x7E,
            0x25,0x00,0x25,0x02,0x25,0x04 });
    }

    // ── Window 2: MIXED, line 159 (last HGR) -> 160, 161 (text row 20) ──
    {
        Memory mem;
        const auto bus = scanFrame(mem, true);
        expectWindow("MIXED line 159 -> 160 -> 161, text row after HGR", bus, {
            0xF6,0x3D,0x38,0x38,0x06,0x3A,0x06,0x3C,0x06,0x3E,0x06,0x40,0x06,
            0x42,0x06,0x44,0x06,0x46,0x06,0x48,0x06,0x4A,0x06,0x4C,0x06,0x4E,
            0x06,0x50,0x06,0x52,0x06,0x54,0x06,0x56,0x06,0x58,0x06,0x5A,0x06,
            0x5C,0x06,0x5E,0x06,0x60,0x06,0x62,0x06,0x64,0x06,0x66,0x06,0x68,
            0x06,0x6A,0x06,0x6C,0x06,0x6E,0x06,0x70,0x06,0x72,0x06,0x74,0x06,
            0x76,0x06,0x38,0x38,0x06,0x3A,0x06,0x3C,0x06,0x3E,0x06,0x40,0x06,
            0x42,0x06,0x44,0x06 });

        // ── Window 3: MIXED, line 191 (text row 23) -> 192 (VBL) ────────
        // The VBL line is scanned as HIRES ($2060 twice, $2061, $2062):
        // MIXED's text window ends with the displayed rows.
        expectWindow("MIXED line 191 -> 192, VBL back on HIRES addresses", bus, {
            0xF6,0x07,0xB8,0xB8,0x07,0xBA,0x07,0xBC,0x07,0xBE,0x07,0xC0,0x07,
            0xC2,0x07,0xC4,0x07,0xC6,0x07,0xC8,0x07,0xCA,0x07,0xCC,0x07,0xCE,
            0x07,0xD0,0x07,0xD2,0x07,0xD4,0x07,0xD6,0x07,0xD8,0x07,0xDA,0x07,
            0xDC,0x07,0xDE,0x07,0xE0,0x07,0xE2,0x07,0xE4,0x07,0xE6,0x07,0xE8,
            0x07,0xEA,0x07,0xEC,0x07,0xEE,0x07,0xF0,0x07,0xF2,0x07,0xF4,0x07,
            0xF6,0x07,0x60,0x60,0x20,0x62 });
    }

    if (failures) { std::printf("floating_bus_measured: %d FAILURE(S)\n", failures); return 1; }
    std::printf("floating_bus_measured: OK\n");
    return 0;
}
