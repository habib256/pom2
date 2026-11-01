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

// ── ESC S hands the HMI back, ESC X cancels the WP modes ────────────────
//
// Both come from Diablo's own `90440-00A Model 630/630ECS API Interface
// Manual, Rev A (12/82)` (fetched 2026-11-01 from bitsavers,
// pdf/diablo/printer/630):
//
//   § 3.9.4.1  "While the terminal is in remote HMI mode, the SPACING switch
//              is ignored. An ESC S sequence will return control of HMI to
//              the SPACING switch."
//   p. 3-13    "ESC P  Proportional Space ON (cleared by ESC S)"
//   § 3.24.11  "The sequence ESC X will cancel the following features: Auto
//              Underscore, Bold Overprint, Shadow Print, Program Mode,
//              Offset Selection, Auto Justify, Auto Center. Increased
//              carriage settling time and proportional space mode are not
//              cancelled by ESC X."
//
// POM2 dropped both, so a driver that tightened the HMI for one passage
// printed the whole rest of the job at that HMI, and ESC X left bold and
// underline standing.
void testDiabloEscSandX()
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
    // The profile's own pitch, measured with no HMI ever set: the "SPACING
    // switch" position this head powers up in. The HMI the test sets below
    // is deliberately NOT equal to it — an earlier draft picked 12/120 =
    // 0.1", which is this head's own 10 cpi cell, so the assertion held
    // whether ESC S worked or not.
    const double cell = run({ 'A' })->status().headX - home;
    assert(cell > 0.0);
    // ESC US 25 sets HMI = 24/120 = 0.2", twice the cell.
    assert(!near(0.2, cell));
    // Without ESC S every character after it advances by that HMI.
    assert(near(run({ 0x1B, 0x1F, 25, 'A', 'B' })->status().headX, home + 0.4));
    // With ESC S the second character is back on the switch's pitch.
    assert(near(run({ 0x1B, 0x1F, 25, 'A', 0x1B, 'S', 'B' })->status().headX,
                home + 0.2 + cell));
    // ESC P is cleared by ESC S too (p. 3-13), so the advance after it is the
    // fixed cell again and not a proportional escapement.
    assert(near(run({ 0x1B, 'P', 0x1B, 'S', 'A' })->status().headX, home + cell));

    // ESC X cancels Auto Underscore and Bold Overprint...
    assert(run({ 0x1B, 'E', 0x1B, 'O' })->status().styleText.find("bold")
           != std::string::npos);
    const std::string afterX = run({ 0x1B, 'E', 0x1B, 'O', 0x1B, 'X' })->status().styleText;
    assert(afterX.find("bold")      == std::string::npos);
    assert(afterX.find("underline") == std::string::npos);
    // ...but NOT proportional space, which is § 3.24.11's last sentence and
    // the control that keeps a too-eager ESC X from clearing kStyleProp with
    // the rest.
    const std::string propAfterX = run({ 0x1B, 'P', 0x1B, 'X' })->status().styleText;
    assert(propAfterX.find("prop") != std::string::npos);
    std::printf("  ok: Diablo ESC S returns the HMI, ESC X spares prop space\n");
}

// ── ESC CR P and ESC SUB I really do re-initialise the head ─────────────
//
// API manual § 3.23.1: *"This command [ESC SUB I] will cause the Model 630 to
// unconditionally execute an initialize sequence regardless of any error
// conditions... This command is executed immediately when received over the
// interface, unlike the corresponding remote reset sequence ESC CR P which is
// queued along with other commands. The Model 630 will default to the same
// conditions that exist at power-up."* And § 3.9.4: *"The ESC CR P (Remote
// Reset) sequence may also be used here to cancel all optional format factors
// and return to the format selected by the SPACING switch."*
//
// POM2 consumed the parameter byte of both and did nothing with it, so a
// driver that opened its job with a remote reset — the normal way to clear
// whatever the last job left — inherited the previous job's HMI, VMI, tabs
// and styles. POM2's input stream IS the queue, so the immediate/queued
// distinction has no analogue here and both land on `resetPrinter()`, the
// same power-up path `ESC c` takes on the Epson head.
void testDiabloRemoteReset()
{
    auto run = [](const std::vector<uint8_t>& job) {
        auto iw = std::make_unique<ImageWriter>(144, ImageWriter::PaperSize::Letter);
        iw->setModel(pom2::IwModel::LaserWriterDiablo);
        iw->queueBytes(job.data(), job.size());
        for (int i = 0; i < 4000 && iw->pendingBytes(); ++i) iw->tick(0.05);
        return iw;
    };
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    // Measured against a head that was never disturbed, so the test never has
    // to predict where `resetPrinter()` leaves the paper.
    const double cleanX  = run({ 'B', 'C' })->status().headX;
    const double cleanLs = run({ 'B', 'C' })->status().lineSpacing;
    for (const std::vector<uint8_t>& reset :
         { std::vector<uint8_t>{ 0x1B, 0x0D, 'P' },      // ESC CR P
           std::vector<uint8_t>{ 0x1B, 0x1A, 'I' } }) {  // ESC SUB I
        // Disturb every format factor the manual names, then reset.
        std::vector<uint8_t> job = { 0x1B, 0x1F, 25,     // HMI 24/120"
                                     0x1B, 0x1E, 13,     // VMI 12/48"
                                     0x1B, 'E', 0x1B, 'O' };  // underline + bold
        job.insert(job.end(), reset.begin(), reset.end());
        job.push_back('B');
        job.push_back('C');
        const auto iw = run(job);
        assert(near(iw->status().headX, cleanX));
        assert(near(iw->status().lineSpacing, cleanLs));
        assert(iw->status().styleText == "normal");
        assert(iw->diabloTabStops() == 0);
    }
    std::printf("  ok: Diablo ESC CR P / ESC SUB I re-initialise the head\n");
}


/// The five divergences the 2026-11-01 ImageWriter parity slice found against
/// the ImageWriter II Technical Reference. POM2 inherited all five from the
/// GSport / greg-kennedy reference, and web-a2e shares most of them: three
/// emulators against the manual.
/// `docs/audits/parite-imagewriter-2026-11-01.md`.
void testImageWriterManualDefaults()
{
    auto run = [](const std::vector<uint8_t>& job) {
        auto iw = std::make_unique<ImageWriter>(144, ImageWriter::PaperSize::Letter);
        iw->queueBytes(job.data(), job.size());
        for (int i = 0; i < 8000 && iw->pendingBytes(); ++i) iw->tick(0.05);
        return iw;
    };
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-4; };
    const double home = ImageWriter::kHomeX;

    // [B] TR p. 39 and Tables A-1 / A-2: "Font: Draft" at power-up.
    assert(run({})->quality() == ImageWriter::Quality::Draft &&
           "the printer powers up on the draft font");

    // [B] TR Table 4-1 p. 39: ESC m = ESC a 0, ESC M = ESC a 2 (Apple Scribe
    // compatibility). Both were swallowed as a "typeface variant".
    assert(run({ 0x1B, 'm' })->quality() == ImageWriter::Quality::Correspondence);
    assert(run({ 0x1B, 'M' })->quality() == ImageWriter::Quality::NLQ);

    // [B] TR Table 5-14 p. 75: CR insertion before LF is ON by default, so a
    // bare LF returns the carriage; ESC l 1 turns it off and the text
    // staircases, which is what POM2 always did.
    assert(near(run({ 'A', 'B', 'C', 0x0A })->status().headX, home) &&
           "a bare LF returns the carriage (CR insertion is the default)");
    {
        auto iw = run({ 0x1B, 'l', 1, 'A', 'B', 'C', 0x0A });
        assert(iw->status().headX > home + 0.2 &&
               "ESC l 1 turns the insertion off: the head stays put");
    }

    // [B] TR pp. 34 / 79 / 83: a full line overprints by default; A-6, set by
    // ESC D SP NUL, is what adds the line feed. 100 characters at the
    // power-up pitch overflow the line.
    {
        const std::vector<uint8_t> many(100, 'X');
        auto plain = run(many);
        std::vector<uint8_t> withA6 = { 0x1B, 'D', ' ', 0 };
        withA6.insert(withA6.end(), many.begin(), many.end());
        auto fed = run(withA6);
        assert(near(plain->status().headY, run({})->status().headY) &&
               "a full line overprints: no feed without A-6");
        assert(fed->status().headY > plain->status().headY &&
               "ESC D SP NUL (A-6) restores the automatic line feed");
    }

    // [B] TR p. 60: margins are counted at 9 positions/inch in pica
    // proportional and 10 in elite proportional, not at cpi_'s 10 and 12.
    // A CR parks the head on the left margin, so it reads the margin back.
    {
        auto iw = run({ 0x1B, 'p', 0x1B, 'L', '0', '0', '9', 0x0D });
        assert(near(iw->status().headX, home + 9.0 / 9.0) &&
               "pica proportional counts margin columns at 9 to the inch");
    }
    {
        auto iw = run({ 0x1B, 'P', 0x1B, 'L', '0', '1', '0', 0x0D });
        assert(near(iw->status().headX, home + 10.0 / 10.0) &&
               "elite proportional counts them at 10 to the inch");
    }
    // Control: a FIXED pitch still counts at cpi_.
    {
        auto iw = run({ 0x1B, 'E', 0x1B, 'L', '0', '1', '2', 0x0D });
        assert(near(iw->status().headX, home + 12.0 / 12.0) &&
               "fixed elite is unchanged: 12 columns to the inch");
    }
    std::printf("  ok: ImageWriter power-on defaults follow the TR, not the reference\n");
}

int main()
{
    // ── The Diablo 630 tab racks are BOUNDED ─────────────────────────────
    // `ESC 1` (horizontal) and `ESC -` (vertical) cost no mechanism time, so
    // the paced drain's only brake is kCatchUpBytes — ~8192 pushes per UI
    // frame. Unbounded, and with `ESC -` re-sorting the whole vector on every
    // insert, `1B 2D` in a loop grew it without limit at O(N^2) ON THE UI
    // THREAD: the window stops painting, cancel button included. A real 630's
    // rack is one bit per column position, and POM2's two other heads bound
    // theirs at 32/16.
    {
        auto iw = std::make_unique<ImageWriter>(144, ImageWriter::PaperSize::Letter);
        iw->setModel(pom2::IwModel::LaserWriterDiablo);
        std::vector<uint8_t> job;
        for (int i = 0; i < 2000; ++i) { job.push_back(0x1B); job.push_back('-'); }
        for (int i = 0; i < 2000; ++i) { job.push_back(0x1B); job.push_back('1'); }
        iw->queueBytes(job.data(), job.size());
        for (int i = 0; i < 4000 && iw->pendingBytes(); ++i) iw->tick(0.05);
        // Both at an UNCHANGED position, so dedup alone would hold them at 1;
        // the cap is what holds them if a job really walks the page.
        assert(iw->diabloVTabStops() <= 16);
        assert(iw->diabloTabStops() <= 32);
    }
    testDiablo630();
    testImageWriterManualDefaults();
    testDiabloEscSandX();
    testDiabloRemoteReset();
    testEpsonControlsAndLists();
    testRepeatCountAcceptsHighBitDigits();
    testEpsonOneLineExpanded();
    std::printf("imagewriter_escape: OK\n");
    return 0;
}
