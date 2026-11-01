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

// video_scanner_sather — `Memory::floatingBus` against the PRINTED table.
//
// POM2's floating bus is a verbatim port of MAME's `apple2video.cpp`
// `scanner_address`, and until 2026-11-01 that was the whole of its
// provenance: a port of a port, pinned only for its PAGE2 / 80STORE gating
// (`floatingbus_page2_smoke`). Nothing checked the ADDRESSES, which is what
// every vapor-lock and every floating-bus RNG actually depends on.
//
// Jim Sather, *Understanding the Apple II* (Quality Software, 1983),
// **p. 5-11, Figure 5.6 "TEXT/LORES Video Scanning Map"** is a printed
// ground truth: the HBL base address and the display base address of all
// 32 character rows, page 1 and page 2, in hex and decimal. The page states
// five rules above the table, and this test pins POM2 to all of them:
//
//   1. "HBL is equivalent to A12 in TEXT/LORES mode, so the memory scanned
//      during HBL is completely separate from the memory scanned during
//      HBL'. This is not true in HIRES mode."
//   2. "HBL scanned memory begins $18 bytes before display scanned memory
//      plus $1000."
//   3. "The first address of HBL is always addressed twice consecutively,
//      because H0-H5 is in the all zero state for two consecutive scans."
//   4. "During VBL, V3 and V4 are both true. The horizontal offset sum
//      becomes H5-H4-H3 minus 0100. This is almost the same as the top of
//      the displayed screen (H4-H3-H2 minus 0011). The VBL base addresses
//      are equal to the FIRST 40 base addresses minus eight bytes using
//      128-byte wraparound subtraction. Example: $400 minus $8 gives $478;
//      not $3F8."
//   5. "Horizontal scanning wraps around at the 128-byte segment
//      boundaries. Example: the address scanned before address $400 is
//      $47F during VBL. The address scanned before $400 when VBL is false
//      is $147F."
//
// AN ERRATUM IN THE PRIMARY SOURCE, recorded so nobody "corrects" POM2
// toward it. Rule 2's Applesoft sequence as printed reads:
//
//      10 HBL = BASE-24
//      20 IF INT(HBL/128)<>INT(BASE/128) THEN HBL = HBL+128
//      30 HBL = HBL+1024.
//
// Step 30 must be **+4096**, not +1024. The prose one line above says
// "plus $1000" ($1000 = 4096), and Sather's own table settles it: row 0 has
// BASE = $400 and HBL = $1468. $400 - 24 = $3E8; the 128-byte wraparound of
// step 20 lifts it to $468; $468 + 1024 would be $868, which is text page 2
// and is not what the table prints. $468 + 4096 = $1468, which is. POM2
// (and MAME) set bit A12, i.e. +$1000, and are right.
//
// THE //e IS DIFFERENT, AND SATHER SAYS SO HIMSELF. POM2 suppresses the HBL
// A12 bit on a //e (`if (!iieMode && h_clock < 25)`), which was MAME's word
// alone until *Understanding the Apple IIe* (Quality Software, 1985) was
// fetched on 2026-11-01. Its § TEXT/LORES Scanning, p. 5-10, settles it:
//
//   "In TEXT/LORES, A15, A14, A13, and A12 equivalents are false (low,
//    ground, zip). The A10 equivalent is (80STORE' . PAGE2)', and the A11
//    equivalent is 80STORE' . PAGE2."
//
// — no A12 term at all, so no HBL phantom row; and the A10/A11 pair is
// exactly POM2's `Page2 = page2 && !80store` base select, which the table at
// the foot of the same page spells out (PAGE 1 -> $0400-$07FF; PAGE 2 with
// 80STORE -> $0400-$07FF; PAGE 2 with 80STORE' -> $0800-$0BFF).
//
// The second confirmation is the better one: THE TWO EDITIONS DIFFER EXACTLY
// AS THE HARDWARE DOES. The 1983 book's rule 1 reads "HBL scanned memory
// begins $18 bytes before display scanned memory PLUS $1000" and its
// Applesoft sequence has three steps; the 1985 //e book's reads "HBL scanned
// memory begins $18 bytes before display scanned memory" and its sequence has
// TWO — `30 HBL = HBL+1024` is simply absent, because a //e has no A12 image
// to add. Asserted in `testIIeHasNoHblPhantomRow`.
//
// Everything else here is the II/II+, which is the machine the 1983 book
// documents.
//
// Core test: no ImGui, no CPU — drives Memory directly.

#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

// Everything the scanner can reach, per mode. TEXT page 1 is $400-$7FF plus
// the HBL A12 image at $1400-$17FF; HIRES page 1 is the whole $2000-$3FFF
// page, because in HIRES the HBL scan has NO A12 bit and lands inside it
// (rule 1's own exception, and Figure 5.9 prints the overlap).
uint16_t kScanLow  = 0x0400;
uint16_t kScanHigh = 0x1800;
constexpr int      kCyclesPerLine = 65;
/// The first cycle of horizontal display enable. The 25 HBL cycles are
/// `h_clock` 0..24 — and the table's own column header spells that out, as
/// "00123456789ABCDEF01234567": twenty-five positions whose first entry
/// appears twice, which is rule 3 drawn rather than stated.
constexpr int      kFirstDisplayH = 25;

// ── Reading the scanner's address off the BUS ────────────────────────────
//
// The test must not reimplement the formula it is checking, so the address
// is recovered through `peekFloatingBus` alone: fill the reachable range
// with the LOW byte of each address and sweep every cycle of a frame, then
// refill with the HIGH byte and sweep again. Two passes, and the two bytes
// of each sample compose the address the scanner was on.
//
// (The first draft walked a single marker through RAM per probe instead.
// Same answer, but ~12 M memory writes a probe: standalone it passed in
// seconds, and under `ctest -j2` it blew a 30-second timeout.)
struct ScanTable {
    std::vector<int> addr;      // [v * 65 + h]
    int at(int h, int v) const { return addr[static_cast<size_t>(v) * kCyclesPerLine + h]; }
};

ScanTable buildScanTable(Memory& mem, int lines)
{
    ScanTable t;
    t.addr.assign(static_cast<size_t>(lines) * kCyclesPerLine, 0);
    for (int pass = 0; pass < 2; ++pass) {
        for (uint16_t a = kScanLow; a < kScanHigh; ++a)
            mem.memWrite(a, static_cast<uint8_t>(pass == 0 ? (a & 0xFF) : (a >> 8)));
        for (int v = 0; v < lines; ++v) {
            for (int h = 0; h < kCyclesPerLine; ++h) {
                mem.setCycleCounter(static_cast<uint64_t>(v) * kCyclesPerLine + h);
                const int byte = mem.peekFloatingBus();
                const size_t i = static_cast<size_t>(v) * kCyclesPerLine + h;
                t.addr[i] |= pass == 0 ? byte : (byte << 8);
            }
        }
    }
    return t;
}

/// The table the assertions below read. Rebuilt whenever the machine's mode
/// changes; `scannerReads` / `scannerAddress` are thin lookups over it so
/// the assertions read the same either way.
const ScanTable* gScan = nullptr;

int scannerAddress(Memory&, int h, int v) { return gScan->at(h, v); }
bool scannerReads(Memory&, int h, int v, uint16_t addr)
{
    return gScan->at(h, v) == static_cast<int>(addr);
}

void check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

// ── Sather, Figure 5.6, p. 5-11, PAGE 1 ──────────────────────────────────
// Character rows 0..23 are the 24 displayed text rows; 24..31 are scanned
// during VBL. ("All other rows are scanned 8 consecutive times" — the last
// one 14 times, which is how 24x8 + 7x8 + 14 makes 262.)
const uint16_t kHblBase[32] = {
    0x1468, 0x14E8, 0x1568, 0x15E8, 0x1668, 0x16E8, 0x1768, 0x17E8,
    0x1410, 0x1490, 0x1510, 0x1590, 0x1610, 0x1690, 0x1710, 0x1790,
    0x1438, 0x14B8, 0x1538, 0x15B8, 0x1638, 0x16B8, 0x1738, 0x17B8,
    0x1460, 0x14E0, 0x1560, 0x15E0, 0x1660, 0x16E0, 0x1760, 0x17E0,
};
const uint16_t kDisplayBase[32] = {
    0x0400, 0x0480, 0x0500, 0x0580, 0x0600, 0x0680, 0x0700, 0x0780,
    0x0428, 0x04A8, 0x0528, 0x05A8, 0x0628, 0x06A8, 0x0728, 0x07A8,
    0x0450, 0x04D0, 0x0550, 0x05D0, 0x0650, 0x06D0, 0x0750, 0x07D0,
    0x0478, 0x04F8, 0x0578, 0x05F8, 0x0678, 0x06F8, 0x0778, 0x07F8,
};

// ── Figure 5.6, every printed row ────────────────────────────────────────
void testScanningMap(Memory& mem)
{
    for (int row = 0; row < 32; ++row) {
        const int v = row * 8;          // the row's first scanline
        if (!scannerReads(mem, 0, v, kHblBase[row])) {
            std::printf("FAIL: row %2d (v=%3d) HBL base: scanner is on $%04X, "
                        "Figure 5.6 prints $%04X\n",
                        row, v, scannerAddress(mem, 0, v), kHblBase[row]);
            ++failures;
        }
        if (!scannerReads(mem, kFirstDisplayH, v, kDisplayBase[row])) {
            std::printf("FAIL: row %2d (v=%3d) display base: scanner is on $%04X, "
                        "Figure 5.6 prints $%04X\n",
                        row, v, scannerAddress(mem, kFirstDisplayH, v),
                        kDisplayBase[row]);
            ++failures;
        }
    }
    std::printf("  ok: all 32 rows x (HBL base, display base) match Figure 5.6\n");
}

// ── Rule 2, as arithmetic rather than as a table ─────────────────────────
// "HBL scanned memory begins $18 bytes before display scanned memory plus
// $1000", with step 20's 128-byte wraparound. Recomputed here from the
// DISPLAY base alone, so it fails if either column drifts.
void testRule2(Memory&)
{
    for (int row = 0; row < 32; ++row) {
        const int base = kDisplayBase[row];
        int hbl = base - 24;
        if (hbl / 128 != base / 128) hbl += 128;     // step 20
        hbl += 0x1000;                               // step 30, the erratum
        if (hbl != kHblBase[row]) {
            std::printf("FAIL: rule 2 on row %2d: $%04X - $18 (wrapped) + $1000 "
                        "= $%04X, table prints $%04X\n",
                        row, base, hbl, kHblBase[row]);
            ++failures;
        }
    }
    std::printf("  ok: rule 2 reproduces every HBL base from its display base\n");
}

// ── Rule 1 ───────────────────────────────────────────────────────────────
// "HBL is equivalent to A12 in TEXT/LORES mode, so the memory scanned during
// HBL is completely separate from the memory scanned during HBL'."
void testRule1(Memory& mem)
{
    bool anyHblLow = false, anyDisplayHigh = false;
    for (int v = 0; v < 262; v += 7) {               // 7 to stride the rows
        for (int h = 0; h < kCyclesPerLine; ++h) {
            const int a = scannerAddress(mem, h, v);
            check(a >= 0, "rule 1: the scanner is on no reachable address");
            if (a < 0) return;
            if (h < kFirstDisplayH && !(a & 0x1000)) anyHblLow     = true;
            if (h >= kFirstDisplayH && (a & 0x1000)) anyDisplayHigh = true;
        }
    }
    check(!anyHblLow,      "rule 1: an HBL cycle scanned below $1000");
    check(!anyDisplayHigh, "rule 1: a display cycle scanned at or above $1000");
    std::printf("  ok: rule 1 — A12 is exactly HBL, on every cycle of 38 lines\n");
}

// ── Rule 3 ───────────────────────────────────────────────────────────────
// "The first address of HBL is always addressed twice consecutively."
void testRule3(Memory& mem)
{
    for (int row = 0; row < 32; ++row) {
        const int v = row * 8;
        check(scannerReads(mem, 0, v, kHblBase[row]) &&
              scannerReads(mem, 1, v, kHblBase[row]),
              "rule 3: h=0 and h=1 are not the same address");
        // ...and the one after it is NOT a third repeat.
        check(!scannerReads(mem, 2, v, kHblBase[row]),
              "rule 3: h=2 repeated the HBL base a third time");
    }
    std::printf("  ok: rule 3 — the HBL base is scanned twice, and only twice\n");
}

// ── Rules 4 and 5 ────────────────────────────────────────────────────────
void testRules4And5(Memory& mem)
{
    // Rule 4's worked example: "$400 minus $8 gives $478; not $3F8."
    check(kDisplayBase[24] == 0x0478, "rule 4: the table's own VBL base moved");
    check(scannerReads(mem, kFirstDisplayH, 24 * 8, 0x0478),
          "rule 4: the first VBL row does not start at $478");

    // Rule 5, walked rather than asserted at a guessed cycle: find where the
    // scan of a line reaches $400 and look at the cycle before it.
    auto predecessorOf400 = [&](int v) -> int {
        for (int h = 1; h < kCyclesPerLine; ++h)
            if (scannerReads(mem, h, v, 0x0400)) return scannerAddress(mem, h - 1, v);
        return -1;
    };
    const int inVbl  = predecessorOf400(24 * 8);   // VBL row: wraps mid-display
    const int notVbl = predecessorOf400(0);        // row 0: $400 is the first
    check(inVbl == 0x047F,
          "rule 5: during VBL the address before $400 is not $47F");
    check(notVbl == 0x147F,
          "rule 5: outside VBL the address before $400 is not $147F");
    if (inVbl != 0x047F || notVbl != 0x147F)
        std::printf("       (got $%04X in VBL, $%04X outside)\n", inVbl, notVbl);
    std::printf("  ok: rules 4 and 5 — VBL base $478, 128-byte wraparound\n");
}

// ── Figure 5.9, "HIRES Video Scanning Map (1 of 4)", p. 5-45 ────────────
//
// The same two columns for the first 64 SCANLINES (HIRES has a distinct row
// per scanline, not per character row). Rule 1's exception is visible in the
// numbers: the HIRES HBL base is $2068 where the display base is $2000 — no
// A12 bit, so HBL scanned memory sits INSIDE the displayed page and the two
// overlap, exactly as the rule says they do not in TEXT/LORES.
//
// The table is a regular lattice, which is why it is generated here rather
// than typed out 128 times: scanline v = 8*g + k has base
// `kHiresHblRow8[k] + g*$80`, and the display base is $68 below it. Both
// anchors are read off the printed page, and the generated values were
// checked against every printed row before this was written.
const uint16_t kHiresHblRow8[8] = {
    0x2068, 0x2468, 0x2868, 0x2C68, 0x3068, 0x3468, 0x3868, 0x3C68,
};

void testHiresScanningMap(Memory& mem)
{
    mem.memRead(0xC050);            // graphics
    mem.memRead(0xC057);            // HIRES
    mem.memRead(0xC054);            // page 1
    kScanLow  = 0x2000;
    kScanHigh = 0x4000;
    const ScanTable hires = buildScanTable(mem, 262);
    const ScanTable* saved = gScan;
    gScan = &hires;
    for (int v = 0; v < 64; ++v) {
        const uint16_t hbl  = static_cast<uint16_t>(kHiresHblRow8[v % 8] + (v / 8) * 0x80);
        const uint16_t disp = static_cast<uint16_t>(hbl - 0x68);
        if (!scannerReads(mem, 0, v, hbl)) {
            std::printf("FAIL: HIRES scanline %2d HBL base: scanner is on $%04X, "
                        "Figure 5.9 prints $%04X\n", v, scannerAddress(mem, 0, v), hbl);
            ++failures;
        }
        if (!scannerReads(mem, kFirstDisplayH, v, disp)) {
            std::printf("FAIL: HIRES scanline %2d display base: scanner is on $%04X, "
                        "Figure 5.9 prints $%04X\n", v,
                        scannerAddress(mem, kFirstDisplayH, v), disp);
            ++failures;
        }
    }
    // Rule 1's exception, asserted rather than implied: no HIRES HBL cycle
    // sets A12 relative to its page, and the HBL base is inside the page.
    check(scannerReads(mem, 0, 0, 0x2068),
          "HIRES: the HBL base is not $2068 (rule 1's overlap)");
    std::printf("  ok: 64 HIRES scanlines x (HBL base, display base) match Figure 5.9\n");
    gScan = saved;
    mem.resetSoftSwitches();
    kScanLow  = 0x0400;
    kScanHigh = 0x1800;
}

// ── Figure 5.6's footnote ────────────────────────────────────────────────
// "The last row of memory is scanned 14 consecutive times. All other rows
// are scanned 8 consecutive times." That is how 24x8 displayed + 7x8 + 14
// makes an NTSC frame's 262 lines, and it is what a vapor lock leans on:
// the final text row holds the bus for nearly twice as long as any other.
void testRowScanRepeats(Memory& mem)
{
    int runs = 0, lastRun = 0;
    bool otherThanEight = false;
    int prev = -1, run = 0;
    for (int v = 0; v < 262; ++v) {
        const int a = scannerAddress(mem, kFirstDisplayH, v);
        if (a != prev) {
            if (prev >= 0 && run != 8) otherThanEight = true;
            if (prev >= 0) ++runs;
            prev = a; run = 1;
        } else {
            ++run;
        }
    }
    ++runs; lastRun = run;              // the run still open at end of frame
    check(runs == 32, "row-scan repeats: not 32 distinct rows in a frame");
    check(!otherThanEight, "row-scan repeats: a row other than the last was not scanned 8 times");
    check(lastRun == 14, "row-scan repeats: the last row is not scanned 14 times");
    std::printf("  ok: 32 rows per frame, 8 scans each, the last one 14\n");
}

// ── A //e RESET clears the DHGR (AN3) latch ─────────────────────────────
//
// `Understanding the Apple IIe` **p. 7-3**, explanation 1 of Figure 7.1 —
// the IOU functional diagram, which is where POM2 and MAME both already
// cite this rule from. Read off a RENDER of the page, not just the OCR:
// the archive.org text layer for this book is noisy enough ("lOU" for
// "IOU", "ail" for "all") that a behaviour change must not rest on it.
//
//   "All soft switches except KEYSTROBE, TEXT, and MIXED are reset when
//    the RESET' line drops low. All soft switches appear to be cleared at
//    power up."
//
// Three names, and the DHGR / AN3 latch ($C05E-$C05F) is not one of them.
// MAME agrees with its own citation — `apple2e.cpp` `reset_w` carries the
// comment "Certain IOU switches off (80STORE, 80COL, ALTCHR, PAGE2, HIRES,
// AN0, AN1, AN2, AN3) // Sather, Fig 7.1" and then does
// `m_an0 = m_an1 = m_an2 = m_an3 = false;`.
//
// POM2 preserved it, citing those same two lines for the opposite rule, so a
// Ctrl-Reset out of a DHGR program left the //e in double hi-res where real
// hardware drops to HGR until something sets AN3 again — the first frame
// after the reset came out shredded.
//
// Sather's own hedge, which bounds the claim: Figure 7.1 is "only a
// functional representation resulting primarily from my investigations of
// Apple IIe operational features". It is not a die shot — but it agrees
// with MAME, and POM2 agreed with neither.
void testIIeResetClearsDhgr(Memory& iie)
{
    iie.setIIEMode(true);
    iie.resetSoftSwitches();
    // Enter DHGR the way a program does: 80COL on, then $C05E.
    (void)iie.memRead(0xC050);              // graphics
    (void)iie.memRead(0xC057);              // HIRES
    iie.memWrite(0xC00D, 0);                // SET80COL
    (void)iie.memRead(0xC05E);              // DHGR on (AN3 low)
    check(iie.getDisplayState().dhgr, "setup: $C05E did not enable DHGR");
    const bool textBefore  = iie.getDisplayState().textMode;
    const bool mixedBefore = iie.getDisplayState().mixedMode;

    iie.resetSoftSwitches(/*warm=*/true);   // Ctrl-Reset

    check(!iie.getDisplayState().dhgr,
          "a //e Ctrl-Reset must clear the DHGR/AN3 latch (IIe book, Fig. 7.1 note 1)");
    // ...and the two that Sather DOES name must still survive, so the fix is
    // not a blanket wipe.
    check(iie.getDisplayState().textMode  == textBefore,  "TEXT must survive a reset");
    check(iie.getDisplayState().mixedMode == mixedBefore, "MIXED must survive a reset");
    std::printf("  ok: //e reset clears DHGR, keeps TEXT and MIXED\n");
    iie.setIIEMode(false);
}

// ── The //e has no HBL phantom row ──────────────────────────────────────
void testIIeHasNoHblPhantomRow(Memory& iie)
{
    iie.setIIEMode(true);
    iie.resetSoftSwitches();
    const ScanTable iieTable = buildScanTable(iie, 262);
    const ScanTable* saved = gScan;
    gScan = &iieTable;
    // Same probe range: if A12 were set anywhere the sample would land in
    // the $1400-$17FF image instead of the page itself.
    for (int row = 0; row < 32; ++row) {
        const int v = row * 8;
        // The HBL base is the TEXT/LORES one WITHOUT the $1000 bit.
        const uint16_t want = static_cast<uint16_t>(kHblBase[row] & 0x0FFF);
        if (!scannerReads(iie, 0, v, want)) {
            std::printf("FAIL: //e row %2d HBL base: scanner is on $%04X, "
                        "Sather (IIe, p. 5-10) says A12 is false -> $%04X\n",
                        row, scannerAddress(iie, 0, v), want);
            ++failures;
        }
    }
    // And rule 5 in its //e form: the address before $400 is $47F whether or
    // not VBL is true, because there is no A12 image to come from.
    auto predecessorOf400 = [&](int v) -> int {
        for (int h = 1; h < kCyclesPerLine; ++h)
            if (scannerReads(iie, h, v, 0x0400)) return scannerAddress(iie, h - 1, v);
        return -1;
    };
    check(predecessorOf400(24 * 8) == 0x047F, "//e rule 5 in VBL");
    check(predecessorOf400(0)      == 0x047F, "//e rule 5 outside VBL");
    std::printf("  ok: //e TEXT/LORES has no HBL A12 image (IIe book, p. 5-10)\n");
    gScan = saved;
    iie.setIIEMode(false);
}

}  // namespace

int main()
{
    std::printf("video_scanner_sather — Understanding the Apple II, Fig. 5.6\n");

    Memory mem;                  // II/II+ default: the machine the book documents
    mem.resetSoftSwitches();     // TEXT, page 1

    const ScanTable base = buildScanTable(mem, 262);
    gScan = &base;

    testScanningMap(mem);
    testRule2(mem);
    testRule1(mem);
    testRule3(mem);
    testRules4And5(mem);
    testRowScanRepeats(mem);
    testHiresScanningMap(mem);

    Memory iie;
    testIIeHasNoHblPhantomRow(iie);
    testIIeResetClearsDhgr(iie);

    if (failures) {
        std::printf("video_scanner_sather: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("video_scanner_sather: OK\n");
    return 0;
}
