// POM2 Apple II Emulator — GPL-3.0-or-later
//
// imagewriter_escape — escape-sequence parsing defects found by the
// 2026-10-27 bug hunt, kept out of imagewriter_smoke (at its size budget):
// high-bit ESC V count digits, and Epson SO's one-line expansion.

#include "ImageWriter.h"
#include "PrinterCard.h"
#include "PrinterFeedCursor.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include <memory>

using pom2::ImageWriter;

// ── Epson SO is one line wide; ESC W 1 is not ────────────────────────────
// They shared one style bit: `SO "TITLE" CR LF` left the whole document
// expanded, and DC4 cancelled a persistent ESC W 1.
void testEpsonOneLineExpanded()
{
    auto cpiAfter = [](const std::vector<uint8_t>& job) {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        iw.setModel(pom2::IwModel::EpsonFX80);
        iw.queueBytes(job.data(), job.size());
        for (int i = 0; i < 2000 && iw.pendingBytes(); ++i) iw.tick(0.05);
        return iw.status().cpi;
    };
    assert(cpiAfter({ 0x0E, 'A' }) < 6.0);                        // SO: expanded
    assert(cpiAfter({ 0x0E, 'A', 0x0D, 0x0A, 'B' }) > 9.0);       // ends at the line
    assert(cpiAfter({ 0x1B, 'W', 1, 0x0D, 0x0A, 'B' }) < 6.0);    // ESC W 1 stays
    assert(cpiAfter({ 0x1B, 'W', 1, 0x14, 'B' }) < 6.0);          // DC4 leaves it
    assert(cpiAfter({ 0x0E, 'A', 0x14, 'B' }) > 9.0);             // DC4 ends SO
    std::printf("  ok: Epson SO is one line, ESC W 1 persists\n");
}

// ── ESC V's count digits are digits with bit 7 set too ──────────────────
// ESC V unmasks bit 7 when the command letter arrives (for the pattern
// byte), which also left the four count digits unmasked: Applesoft's
// high-bit "0100" read as a count of 0 and nothing printed.
void testRepeatCountAcceptsHighBitDigits()
{
    auto run = [](const std::vector<uint8_t>& job) {
        ImageWriter iw(144, ImageWriter::PaperSize::Letter);
        iw.queueBytes(job.data(), job.size());
        for (int i = 0; i < 2000 && (iw.pendingBytes() || iw.pendingRepeats()); ++i)
            iw.tick(0.05);
        return iw.currentPage().pix;
    };
    const auto plain = run({ 0x1B, 'V', '0', '1', '0', '0', 0xFF });
    const auto high  = run({ 0x9B, 0xD6, 0xB0, 0xB1, 0xB0, 0xB0, 0xFF });
    bool anyInk = false;
    for (uint8_t v : plain) if (v) { anyInk = true; break; }
    assert(anyInk);
    assert(high == plain && "high-bit count digits read as 0");
    std::printf("  ok: ESC V counts accept bit-7 digits\n");
}

// FX vol. 2 App. B: control codes and parameterised commands on the FX-80.
void testEpsonControlsAndLists()
{
    auto run = [](const std::vector<uint8_t>& job) {
        auto iw = std::make_unique<ImageWriter>(144, ImageWriter::PaperSize::Letter);
        iw->setModel(pom2::IwModel::EpsonFX80);
        iw->queueBytes(job.data(), job.size());
        for (int i = 0; i < 4000 && iw->pendingBytes(); ++i) iw->tick(0.05);
        return iw;
    };
    const double home = ImageWriter::kHomeX, cell = 0.1;   // Pica
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    // Undefined control codes print nothing and do not move the head (they
    // drew CP437 glyphs); NUL is ignored.
    assert(near(run({ 'A', 0x00, 0x1E, 0x10, 'B' })->status().headX, home + 2 * cell));
    // HT: power-on stops every 8 Pica columns from the margin.
    assert(near(run({ 'A', 0x09 })->status().headX, home + 0.8));
    // ESC D n1 n2 NUL: a list, consumed whole — its 10 used to line-feed.
    {
        auto iw = run({ 0x1B, 'D', 10, 20, 0, 0x09 });
        assert(near(iw->status().headX, home + 1.0));
        assert(near(iw->status().headY, run({})->status().headY));
    }
    // ESC ^ / ESC & bodies and ESC ? / ESC % parameters never print.
    {
        auto iw = run({ 0x1B, '^', 0, 2, 0, 1, 2, 3, 4, 0x1B, '?', 'K', 3,
                        0x1B, '&', 0, 'A', 'A', 0,1,2,3,4,5,6,7,8,9,10,11, 'X' });
        assert(near(iw->status().headX, home + cell));
    }
    // LF returns the carriage too (ESC/P C-27).
    assert(near(run({ 'A', 'B', 0x0A })->status().headX, home));
    // ESC W 0 cancels a pending one-line SO as well.
    assert(run({ 0x0E, 0x1B, 'W', 0, 'A' })->status().cpi > 9.0);
    std::printf("  ok: ESC/P control codes, tabs and lists per the FX manual\n");
}

// Diablo 630 (SPI § 3, ECS/API § 3.4): ESC US n is the HORIZONTAL index,
// ESC RS n the vertical — POM2 had them swapped — and control codes print
// nothing.
void testDiablo630()
{
    auto run = [](const std::vector<uint8_t>& job) {
        auto iw = std::make_unique<ImageWriter>(144, ImageWriter::PaperSize::Letter);
        iw->setModel(pom2::IwModel::LaserWriterDiablo);
        iw->queueBytes(job.data(), job.size());
        for (int i = 0; i < 4000 && iw->pendingBytes(); ++i) iw->tick(0.05);
        return iw;
    };
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    const double home = ImageWriter::kHomeX;
    // ESC US 13 → HMI 12/120 = 0.1": one character later the head is 0.1" on.
    assert(near(run({ 0x1B, 0x1F, 13, 'A' })->status().headX, home + 0.1));
    // ESC RS 5 → VMI 4/48 = 1/12".
    assert(near(run({ 0x1B, 0x1E, 5 })->status().lineSpacing, 1.0 / 12.0));
    // BEL / SO / DC1 print nothing and do not move the head.
    assert(near(run({ 0x1B, 0x1F, 13, 'A', 0x07, 0x0E, 0x11, 'B' })->status().headX, home + 0.2));
    // ESC HT n: (n-1) HMI from position 0.
    assert(near(run({ 0x1B, 0x1F, 13, 0x1B, 0x09, 6 })->status().headX, home + 0.5));
    std::printf("  ok: Diablo 630 RS/US, ESC HT and silent control codes\n");
}

int main()
{
    testDiablo630();
    testEpsonControlsAndLists();
    testRepeatCountAcceptsHighBitDigits();
    testEpsonOneLineExpanded();
    std::printf("imagewriter_escape: OK\n");
    return 0;
}
