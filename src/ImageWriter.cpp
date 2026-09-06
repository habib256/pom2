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

// ImageWriter — see ImageWriter.h for the port provenance, the deliberate
// deviations from greg-kennedy/ImageWriter, and the page encoding.

#include "ImageWriter.h"

#include "ImageWriterRom.h"
#include "hgrpaint/HgrFont.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace pom2 {

namespace {

// Paper sizes in PostScript points (iw_charmaps.h:62-77).
struct PaperDef { const char* name; int wPt; int hPt; };
constexpr PaperDef kPapers[] = {
    { "US Letter (8.5 x 11 in)",     612,  792 },
    { "US Legal (8.5 x 14 in)",      612, 1008 },
    { "ISO A4 (210 x 297 mm)",       595,  842 },
    { "ISO B5 (176 x 250 mm)",       499,  709 },
    { "Wide fanfold (14 x 11 in)",  1071,  792 },
    { "Ledger (11 x 17 in)",         792, 1224 },
    { "ISO A3 (297 x 420 mm)",       842, 1191 },
};
static_assert(sizeof(kPapers) / sizeof(kPapers[0]) ==
                  static_cast<size_t>(ImageWriter::PaperSize::Count),
              "kPapers must cover every PaperSize");

// Soft switch A (imagewriter.cpp:87-98).
constexpr uint8_t kSwitchACharsetMask    = 0x07;
constexpr uint8_t kSwitchAPerforationSkip= 0x10;
constexpr uint8_t kSwitchALfAfterCr      = 0x80;

// Ten ASCII positions the international charset switches (A-1..A-3) remap
// (imagewriter.cpp:450-460): # @ [ \ ] ` { | } ~.
constexpr uint8_t kIntlSlots[10] = {
    0x23, 0x40, 0x5b, 0x5c, 0x5d, 0x60, 0x7b, 0x7c, 0x7d, 0x7e
};

// iw_charmaps.h:50-60, transcribed from Unicode to the CP437 code points
// the bundled 8x8 font is indexed by. Where CP437 has no glyph (O-slash,
// diaeresis) the closest ASCII stand-in is used — the alternative would be
// a blank cell.
constexpr uint8_t kIntlCharSets[8][10] = {
    { 0x23, 0x40, 0x5b, 0x5c, 0x5d, 0x60, 0x7b, 0x7c, 0x7d, 0x7e }, // USA
    { 0x9c, 0x15, 0xf8, 0x87, 0x82, 0x97, 0x85, 0x95, 0x8a, 0x8d }, // Italian
    { 0x23, 0x40, 0x92, 0x4f, 0x8f, 0x60, 0x91, 0x6f, 0x86, 0x7e }, // Danish
    { 0x9c, 0x40, 0x5b, 0x5c, 0x5d, 0x60, 0x7b, 0x7c, 0x7d, 0x7e }, // UK
    { 0x23, 0x15, 0x8e, 0x99, 0x9a, 0x60, 0x84, 0x94, 0x81, 0xe1 }, // German
    { 0x23, 0x40, 0x8e, 0x99, 0x8f, 0x60, 0x84, 0x94, 0x86, 0x7e }, // Swedish
    { 0x9c, 0x85, 0xf8, 0x87, 0x15, 0x60, 0x82, 0x97, 0x8a, 0x22 }, // French
    { 0x9c, 0x15, 0xad, 0xa5, 0xa8, 0x60, 0xf8, 0xa4, 0x87, 0x7e }, // Spanish
};

// ── Character ROM banks (src/ImageWriterRom.h) ──────────────────────────
//
// POM2 used to draw every glyph with the bundled 8x8 CP437 font, which meant
// NLQ was only a *speed* and proportional mode kept a fixed cell. These are
// the real ImageWriter faces, so the quality the guest asks for with `ESC a`
// now changes what lands on paper.
//
// `rows` and `rowPitch` come in pairs: draft/correspondence are 9 wires at
// 1/72 in, NLQ is 18 rows at 1/144 in. Both make a cell 1/8 in tall, which is
// why a mixed-quality line still sits on one baseline.
const IwRomBank kBankStdFixed { iwrom::kIw2StdFixed,  iwrom::kIw2StdFixedOverrides,
                              iwrom::kIw2StdFixedOverridesCount,  9, 1.0 / 72.0,  false };
const IwRomBank kBankStdProp  { iwrom::kIw2StdProp,   iwrom::kIw2StdPropOverrides,
                              iwrom::kIw2StdPropOverridesCount,   9, 1.0 / 72.0,  true  };
const IwRomBank kBankDraft    { iwrom::kIw2Draft,     iwrom::kIw2DraftOverrides,
                              iwrom::kIw2DraftOverridesCount,     9, 1.0 / 72.0,  false };
const IwRomBank kBankNlqFixed { iwrom::kIw2NlqFixed,  iwrom::kIw2NlqFixedOverrides,
                              iwrom::kIw2NlqFixedOverridesCount, 18, 1.0 / 144.0, false };
const IwRomBank kBankNlqProp  { iwrom::kIw2NlqProp,   iwrom::kIw2NlqPropOverrides,
                              iwrom::kIw2NlqPropOverridesCount,  18, 1.0 / 144.0, true  };
const IwRomBank kBankIw1Fixed { iwrom::kIw1Fixed,     iwrom::kIw1FixedOverrides,
                              iwrom::kIw1FixedOverridesCount,     9, 1.0 / 72.0,  false };
const IwRomBank kBankIw1Prop  { iwrom::kIw1Prop,      iwrom::kIw1PropOverrides,
                              iwrom::kIw1PropOverridesCount,      9, 1.0 / 72.0,  true  };
const IwRomBank kBankDmpFixed { iwrom::kDmpFixed,     iwrom::kDmpFixedOverrides,
                              iwrom::kDmpFixedOverridesCount,     9, 1.0 / 72.0,  false };
const IwRomBank kBankDmpProp  { iwrom::kDmpProp,      iwrom::kDmpPropOverrides,
                              iwrom::kDmpPropOverridesCount,      9, 1.0 / 72.0,  true  };
// Epson FX-80 Roman: 12 columns x 9 wires, like the C. Itoh draft cell. The
// FX-80's proportional mode derives width at render time rather than from a
// second bitmap, so there is one bank and `stdProp` points at it too.
const IwRomBank kBankEpson    { iwrom::kEpsonFx,      nullptr, 0, 9, 1.0 / 72.0, false };

// ── Per-model ESC codes with no hardware behind them ────────────────────
//
// Consumed WITH their parameter bytes. The manuals' rule is that an
// unrecognised code is dropped along with the ESC; letting the parameter fall
// through would print it as ordinary text, which is how a "MouseText" command
// ends up spraying a stray 'm' into somebody's letter.
//
//   w W   half-height              x y z  super/subscript
//   m M   alternate character map (MouseText)      & MouseText enable
//   a     font (quality) select    K      colour select
constexpr uint8_t kIw1Ignored[] = {
    0x77, 0x57, 0x78, 0x79, 0x7A, 0x6D, 0x4D, 0x26, 0x61, 0x4B,
};
// The DMP drops all of the above plus its own absences (Appendix B):
//   e  13.4 cpi semicondensed is not one of its seven pitches
//   s  proportional spacing is spelled ESC 1..6 on this head
//   c  it resets only via the INPUT.PRIME line, never in software
//   g  it has ESC G / ESC V only
//   u  its tabbing is ESC ( / ESC ) / ESC 0 only
constexpr uint8_t kDmpIgnored[] = {
    0x77, 0x57, 0x78, 0x79, 0x7A, 0x6D, 0x4D, 0x26, 0x61, 0x4B,
    0x65, 0x73, 0x63, 0x67, 0x75,
};

// ── The three heads ─────────────────────────────────────────────────────
//
// Sources: Apple ImageWriter II Technical Reference; ImageWriter I User's
// Manual Part I (single 120 cps face, Elite power-on default, App. E); Apple
// DMP Reference (rebadged C. Itoh 8510, Pica power-on, App. B command list).
// Cross-checked against mikedaley/web-a2e's `imagewriter-i.js` / `apple-dmp.js`.
const IwModelProfile kModels[static_cast<size_t>(IwModel::Count)] = {
    // ImageWriter II — the superset.
    { "ImageWriter II", /*colour*/ true, /*tiers*/ true,
      &kBankStdFixed, &kBankStdProp, &kBankDraft, &kBankNlqFixed, &kBankNlqProp,
      12.0, 2, 96, 180.0, 45.0, IwLineage::CItoh, 0, nullptr, 0 },
    // ImageWriter I — mono, one face, powers up at Elite 12 cpi.
    { "ImageWriter I", false, false,
      &kBankIw1Fixed, &kBankIw1Prop, nullptr, nullptr, nullptr,
      12.0, 2, 96, 120.0, 120.0, IwLineage::CItoh, 0,
      kIw1Ignored, sizeof(kIw1Ignored) / sizeof(kIw1Ignored[0]) },
    // Apple DMP — mono, one face, powers up at Pica 10 cpi.
    { "Apple DMP", false, false,
      &kBankDmpFixed, &kBankDmpProp, nullptr, nullptr, nullptr,
      10.0, 1, 80, 120.0, 120.0, IwLineage::CItoh, 0,
      kDmpIgnored, sizeof(kDmpIgnored) / sizeof(kDmpIgnored[0]) },
    // Epson FX-80 — ESC/P. Pica 10 cpi at power-on, 160 cps draft
    // (FX-80 User's Manual App. A), mono. `escP` routes it to the other
    // parser; the ignored-command list is unused for it, since ESC/P has its
    // own dispatch and its own idea of what is unknown.
    { "Epson FX-80", false, false,
      &kBankEpson, &kBankEpson, nullptr, nullptr, nullptr,
      10.0, 1, 80, 160.0, 160.0, IwLineage::EscP, kEscPFX80, nullptr, 0 },
    // C. Itoh Prowriter 8510A — the mechanism Apple rebadged as the DMP. The
    // faces are the DMP's because they ARE the DMP's; what differs is the
    // firmware, which has no Apple-imposed gaps, so the ignored list is
    // empty where the DMP's is not. Pica 10 cpi, 120 cps.
    { "C. Itoh Prowriter 8510A", false, false,
      &kBankDmpFixed, &kBankDmpProp, nullptr, nullptr, nullptr,
      10.0, 1, 80, 120.0, 120.0, IwLineage::CItoh, 0, nullptr, 0 },
    // NEC PC-8023A — the same 8510 mechanism under NEC's badge, quoted at
    // 100 cps. Shares the Prowriter's command set and faces; carried as its
    // own row so a future divergence lands here rather than silently
    // inheriting, the same reasoning the IW-I/DMP banks are kept apart under.
    { "NEC PC-8023A", false, false,
      &kBankDmpFixed, &kBankDmpProp, nullptr, nullptr, nullptr,
      10.0, 1, 80, 100.0, 100.0, IwLineage::CItoh, 0, nullptr, 0 },
    // Epson MX-80 (1980, pre-Graftrax) — ESC/P before it was ESC/P. Single
    // density graphics (ESC K) and nothing else: no ESC L/Y/Z, no ESC *, no
    // italics, no master select, no scripts, no proportional. 80 cps, Pica.
    { "Epson MX-80", false, false,
      &kBankEpson, &kBankEpson, nullptr, nullptr, nullptr,
      10.0, 1, 80, 80.0, 80.0, IwLineage::EscP,
      kEscPSkipPerf, nullptr, 0 },
    // Epson MX-80 Graftrax-Plus (1981 ROM upgrade) — the other graphics
    // densities, italics and the scripts arrive. `ESC *` and proportional
    // still do not; those are FX-generation.
    { "Epson MX-80 Graftrax+", false, false,
      &kBankEpson, &kBankEpson, nullptr, nullptr, nullptr,
      10.0, 1, 80, 80.0, 80.0, IwLineage::EscP,
      kEscPGraphicsLYZ | kEscPItalics | kEscPScripts | kEscPSkipPerf,
      nullptr, 0 },
    // Epson RX-80 (1983) — the FX-80's cheaper sibling: same command set
    // minus proportional spacing (and minus the user-defined characters POM2
    // does not implement on either head), at 100 cps.
    { "Epson RX-80", false, false,
      &kBankEpson, &kBankEpson, nullptr, nullptr, nullptr,
      10.0, 1, 80, 100.0, 100.0, IwLineage::EscP,
      kEscPFX80 & ~kEscPProportional, nullptr, 0 },
    // Apple LaserWriter, Diablo 630 emulation mode. A page printer pretending
    // to be a daisywheel, which is what its back-panel switch offered so
    // software that could not speak PostScript could still print text.
    //
    // 10 cpi is the 630's Courier pitch and what the LaserWriter's emulation
    // came up in; 12 cpi is a switch away (ESC RS). The carriage rate is the
    // SERIAL LINE, not a mechanism: this machine is fed at 9600 baud over an
    // SSC, which is ~960 char/s, and the page itself lands at 8 ppm however
    // fast the bytes arrive. Quoting the line rate is what makes the pacing
    // feel right — there is no head to move.
    //
    // The face is the correspondence bank, and that is a SUBSTITUTE: a real
    // LaserWriter set Courier from its own font ROM, which POM2 has no dump
    // of. Same honesty as the CP437 fallback the ROM banks replaced — the
    // shape of the page (pitch, margins, motion) is right, the letterforms
    // are borrowed.
    { "LaserWriter (Diablo 630)", false, false,
      &kBankStdFixed, &kBankStdProp, nullptr, nullptr, nullptr,
      10.0, 1, 80, 960.0, 960.0, IwLineage::Diablo, 0, nullptr, 0 },
    // Apple LaserWriter, PostScript mode. The pacing figure is the serial
    // line again, and the pitch only matters for the fallback described in
    // IwModel::LaserWriterPostScript — a page that renders never touches
    // either, because the interpreter decides where every mark goes.
    { "LaserWriter (PostScript)", false, false,
      &kBankStdFixed, &kBankStdProp, nullptr, nullptr, nullptr,
      10.0, 1, 80, 960.0, 960.0, IwLineage::PostScript, 0, nullptr, 0 },
};

/// POM2's soft-switch A charset index → the ROM's locale enum. The order of
/// `kIntlCharSets` above is the reference's, not the ROM's, so this table is
/// the join between them.
constexpr iwrom::IwLocale kCharsetToLocale[8] = {
    iwrom::IwLocale::US, iwrom::IwLocale::IT, iwrom::IwLocale::DK,
    iwrom::IwLocale::UK, iwrom::IwLocale::DE, iwrom::IwLocale::SE,
    iwrom::IwLocale::FR, iwrom::IwLocale::ES,
};

// ESC K n → ribbon band (imagewriter.cpp:935-949). n is an ASCII digit:
// 0 black, 1 yellow, 2 magenta, 3 cyan, 4 orange(red), 5 green, 6 purple.
constexpr uint8_t kRibbonBand[7] = { 7, 4, 1, 2, 5, 6, 3 };

const char* bandName(uint8_t band)
{
    switch (band & 7) {
        case 0: return "none";
        case 1: return "magenta";
        case 2: return "cyan";
        case 3: return "purple";
        case 4: return "yellow";
        case 5: return "orange";
        case 6: return "green";
        default: return "black";
    }
}

// Multi-byte ImageWriter parameters arrive as ASCII digit strings.
inline int paramDigit(uint8_t p)
{
    // Anything that is not an ASCII digit reads as 0: one corrupted byte
    // inside an ESC G/S/C count used to go negative, and the uint32_t cast
    // in setupBitImage turned that into ~4 G bytes of "graphics data" that
    // wedged the parser for the rest of the session.
    return (p >= '0' && p <= '9') ? static_cast<int>(p) - '0' : 0;
}

// ─── Mechanism speed (ImageWriter II Owner's Manual, "Specifications") ──
// 250 cps draft / 45 cps NLQ, both quoted at the 12 cpi default pitch —
// so the carriage crosses cps/cpi inches per second. Graphics passes are
// unidirectional (the head only prints left-to-right so the dot columns
// stay in register), which halves the effective rate. Paper transport is
// quoted as a 5 in/s slew.
constexpr double kQuotedCpi = 12.0;
constexpr double kFeedIps  = 5.0;
/// Never bank more than this much mechanism time: a hidden window or a
/// long host stall must not dump half a page in one frame.
constexpr double kMaxCredit = 1.0;

/// Bit-image horizontal density (dots per inch) per pitch index.
/// 0-7 = ImageWriter II pitches (8 pins/column, 72 dpi vertical),
/// 8-15 = ImageWriter LQ (24 pins/column over three bytes, 216 dpi).
/// SIXTEEN entries: `printRes_` reaches 8-15 on the LQ pitches, and the
/// status line used to index a truncated 8-entry copy with `printRes_ & 7`,
/// reporting 72 dpi for a 144 dpi pass.
constexpr uint16_t kBitImageHorizDpi[16] = {
    72, 80, 96, 107, 120, 136, 144, 160,
    144, 160, 192, 216, 240, 272, 288, 320
};

} // namespace

const char* ImageWriter::paperSizeName(PaperSize s)
{
    const auto i = static_cast<size_t>(s);
    return i < static_cast<size_t>(PaperSize::Count) ? kPapers[i].name
                                                     : kPapers[0].name;
}

ImageWriter::ImageWriter(int dpi, PaperSize paper)
    : dpi_(std::clamp(dpi, kMinDpi, kMaxDpi)), paper_(paper)
{
    const auto i = static_cast<size_t>(paper_) <
                       static_cast<size_t>(PaperSize::Count)
                   ? static_cast<size_t>(paper_) : 0u;
    defaultPageWidth_  = kPapers[i].wPt / 72.0;
    defaultPageHeight_ = kPapers[i].hPt / 72.0;

    rebuildPage();
    resetPrinter();
}

ImageWriter::~ImageWriter()
{
    stopTrace();        // flushes the partial hex row + writes the footer
}

// ─────────────────────────────────────────────────────────────────────────
// Host-side configuration
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::setDpi(int dpi)
{
    const int d = std::clamp(dpi, kMinDpi, kMaxDpi);
    if (d == dpi_) return;
    dpi_ = d;
    rebuildPage();
    resetPrinter();
}

void ImageWriter::setPaperSize(PaperSize s)
{
    if (s == paper_ || static_cast<size_t>(s) >=
                           static_cast<size_t>(PaperSize::Count)) return;
    paper_ = s;
    const auto i = static_cast<size_t>(paper_);
    defaultPageWidth_  = kPapers[i].wPt / 72.0;
    defaultPageHeight_ = kPapers[i].hPt / 72.0;
    rebuildPage();
    resetPrinter();
}

void ImageWriter::setPaperDimensions(double widthIn, double lengthIn,
                                     double* committedWidth,
                                     double* committedLength)
{
    // Quarter-inch steps: that is the granularity a tractor is adjusted in,
    // and it keeps the page raster on whole pixels at every DPI POM2 offers.
    auto snap = [](double v, double lo, double hi) {
        v = std::round(v * 4.0) / 4.0;
        return std::clamp(v, lo, hi);
    };

    const double w = snap(widthIn,  kMinPaperWidthIn,  kMaxPaperWidthIn);
    const double l = snap(lengthIn, kMinPaperLengthIn, kMaxPaperLengthIn);

    if (committedWidth)  *committedWidth  = w;
    if (committedLength) *committedLength = l;
    if (w == defaultPageWidth_ && l == defaultPageHeight_) return;

    defaultPageWidth_  = w;
    defaultPageHeight_ = l;
    // The sheet on the platen is a different size now, so there is nothing
    // meaningful to preserve on it.
    rebuildPage();
    resetPrinter();
}

void ImageWriter::setPowered(bool on)
{
    if (on == powered_) return;
    powered_ = on;
    if (sound_) sound_->power(on);
    // DELIBERATELY not resetPrinter() and not newPage(): switching a real
    // printer off does not eject the sheet or wipe what is on it. That is
    // what `powerCycle()` is for, and conflating the two is why "power off"
    // in an emulator so often loses the user's page.
}

void ImageWriter::rebuildPage()
{
    current_.w = std::max(1, static_cast<int>(defaultPageWidth_  * dpi_));
    current_.h = std::max(1, static_cast<int>(defaultPageHeight_ * dpi_));
    current_.pix.assign(static_cast<size_t>(current_.w) * current_.h, 0);
    current_.dpi = dpi_;
    ++revision_;
}

// ─────────────────────────────────────────────────────────────────────────
// Reset / paper handling (imagewriter.cpp:262-325, 1222-1252)
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::resetPrinter()
{
    color_        = 7 << 5;             // COLOR_BLACK
    curX_ = curY_ = 0.0;
    escSeen_ = fsSeen_ = false;
    escCmd_  = 0;
    numParam_ = neededParam_ = 0;
    topMargin_    = 0.0;
    // Practically every Apple II printer driver (including GS/OS) assumes
    // the ImageWriter's 1/4 inch left margin (imagewriter.cpp:281).
    leftMargin_   = 0.25;
    rightMargin_  = pageWidthIn_  = defaultPageWidth_;
    bottomMargin_ = pageHeightIn_ = defaultPageHeight_;
    lineSpacing_  = 1.0 / 6.0;
    // Power-on pitch is the HEAD's, not a constant: the ImageWriter I comes
    // up at Elite 12 cpi (DIP SW1-6) and the Apple DMP at Pica 10 cpi.
    {
        const IwModelProfile& mp = modelProfile();
        cpi_         = mp.defaultCpi;
        printRes_    = mp.defaultPrintRes;
        definedUnit_ = mp.defaultUnit;
    }
    quality_      = Quality::Correspondence;   // ESC a default (Table 4-1)
    // The ESC/P collector as well: a reset arriving mid-command would
    // otherwise leave the parser expecting parameters and eat the start of
    // the next job.
    epsonNeed_    = 0;
    epsonCount_   = 0;
    // Same trap, third parser: a reset arriving mid-command must not leave
    // the collector armed, or it eats the first byte of the next job. The
    // tab rack is part of the reset too — the 630's ESC 2 exists precisely
    // because tabs otherwise survive everything.
    diabloNeed_   = 0;
    diabloCmd_    = 0;
    diabloTabs_.clear();
    // Reference sets STYLE_BOLD here to fatten a thin TrueType face; on a
    // dot-matrix cell that smears every glyph — see ImageWriter.h.
    style_        = 0;
    extraIntraSpace_ = 0.0;
    bitGraph_.remBytes = 0;
    bitGraph_.readBytesColumn = 0;
    // A repeat run still owed is part of the parser state a reset clears —
    // and leaving it armed would keep `busy()` asserted with nothing left
    // in the input buffer to explain it. (`updateSwitch()` below is what
    // restores `msb_`, so ESC V/U's deferred restore is covered too.)
    repeatRemaining_   = 0;
    repeatPatPos_      = 0;
    repeatRestoresMsb_ = false;
    hmi_          = -1.0;
    switcha_      = 0;                  // SWITCHA_CHARSET_US
    switchb_      = ' ';
    verticalDot_  = 0;
    numHorizTabs_ = 0;
    numVertTabs_  = 0;
    // Re-arm the CR/LF detector. `ESC c` is "initialize printer" — a new
    // job announcing itself — and it is the ONLY thing a guest can send
    // that re-arms this. Leaving the latch here scoped it to the host
    // session instead of the job: once one CR+LF driver had latched CR
    // "don't feed", every later `PR#n : LIST` in the same session printed
    // its whole listing overprinted onto a single black line, and nothing
    // in the guest could clear it — only the panel's power button.
    //
    // Print Shop's colour passes are safe: it separates them with a BARE
    // CR, never `ESC c`, so the latch it relies on survives its own job.
    // Both cases are pinned in testAutoLineFeedDetection.
    feedLatchedOff_ = false;
    crJustFed_      = false;

    selectDefaultMap();
    updateMetrics();
    updateSwitch();

    // Keep whatever is already on the platen. The reference discards it
    // (imagewriter.cpp:315) — it could afford to, because it wrote each
    // page out to disk as it went; here the sheet exists nowhere else, so
    // discarding is silent data loss. A short report with no trailing form
    // feed vanished the moment the next program sent its `ESC c` init.
    // Eject it instead: that is also what the paper does on a real desk —
    // you do not get the sheet back by pressing reset.
    newPage(!currentPageBlank(), true);
}

void ImageWriter::resetPrinterHard()
{
    // Power cycle — whatever was still in the input buffer is gone with it.
    pending_.clear();
    pendingHead_ = 0;
    credit_      = 0.0;
    stalledFor_  = 0.0;           // and so is the stall watchdog
    catchUp_     = false;         // and the backlog it was chasing
    resetPrinter();               // re-arms the CR/LF detector
}

void ImageWriter::clearAll()
{
    pages_.clear();
    droppedPages_ = 0;
    bytesIn_      = 0;
    resetPrinterHard();
}

void ImageWriter::selectDefaultMap()
{
    // The bundled font is already CP437-indexed, so the base map is the
    // identity (the reference walks a CP437→Unicode table for FreeType,
    // imagewriter.cpp:345-362).
    for (int i = 0; i < 256; ++i) curMap_[i] = static_cast<uint8_t>(i);
}

void ImageWriter::updateSwitch()
{
    // imagewriter.cpp:447-479.
    const int charmap = switcha_ & kSwitchACharsetMask;
    for (int i = 0; i < 10; ++i)
        curMap_[kIntlSlots[i]] = kIntlCharSets[charmap][i];

    if (switcha_ & kSwitchAPerforationSkip) {
        topMargin_    = 0.25;
        bottomMargin_ = pageHeightIn_ - 0.25;
    } else {
        topMargin_    = 0.0;
        bottomMargin_ = pageHeightIn_;
    }

    // Switch B-6 selects whether bit 7 reaches the character generator.
    msb_ = (switchb_ & 32) ? 0 : 255;
}

void ImageWriter::updateMetrics()
{
    // Effective pitch, distilled from the reference's font-sizing block
    // (imagewriter.cpp:394-425) with the FreeType point maths dropped —
    // only `actcpi` survives into a dot-matrix cell.
    actcpi_ = cpi_;
    if (!(style_ & kStyleProp)) {
        if (cpi_ == 10.0 && (style_ & kStyleCondensed)) actcpi_ = 17.14;
        if (cpi_ == 12.0 && (style_ & kStyleCondensed)) actcpi_ = 20.0;
    } else if (style_ & kStyleCondensed) {
        actcpi_ *= 2.0;
    }
    if (style_ & kStyleDoubleWidth) actcpi_ /= 2.0;
    if (actcpi_ <= 0.0) actcpi_ = 12.0;
}

void ImageWriter::newPage(bool save, bool resetx)
{
    if (trace_) {
        traceFlushRow();
        std::fprintf(trace_,
            "[%8.3f] PAGE %s (head was at %.2f\" x %.2f\", %zu on the stack)\n",
            traceClock_, save ? "sheet ejected" : "sheet restarted",
            curX_, curY_, pages_.size());
        std::fflush(trace_);
    }
    if (save) {
        if (pages_.size() >= kMaxPages) {
            pages_.erase(pages_.begin());
            ++droppedPages_;
        }
        pages_.push_back(current_);
        // Monotonic (unlike pages_.size(), which the cap holds at 32) so
        // the catch-up drain can budget ejects — each one copies a whole
        // page raster, so a form-feed storm is dear per byte.
        ++sheetsEjected_;
    }
    if (resetx) curX_ = leftMargin_;
    curY_ = topMargin_;
    std::fill(current_.pix.begin(), current_.pix.end(), uint8_t{0});
    ++revision_;
}

void ImageWriter::formFeed()
{
    // FORM FEED button — don't eject a sheet nothing was printed on
    // (imagewriter.cpp:1606-1613).
    newPage(!currentPageBlank(), true);
}

bool ImageWriter::currentPageBlank() const
{
    for (uint8_t v : current_.pix)
        if (v != 0) return false;
    return true;
}

void ImageWriter::lineFeed()
{
    if (sound_) sound_->paperFeed(lineSpacing_);
    curY_ += lineSpacing_;
    // Reverse feeds (ESC r) stop at the top edge of the sheet — the head
    // position must never walk off the raster into negative territory.
    if (curY_ < 0.0) curY_ = 0.0;
    if (curY_ > bottomMargin_ - lineSpacing_) newPage(true, false);
}

// ─────────────────────────────────────────────────────────────────────────
// Dot plotting
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::fillDots(double xInch, double yInch,
                           double wInch, double hInch)
{
    if (current_.pix.empty()) return;

    int x0 = static_cast<int>(std::floor(xInch * dpi_ + 0.5));
    int y0 = static_cast<int>(std::floor(yInch * dpi_ + 0.5));
    int x1 = static_cast<int>(std::floor((xInch + wInch) * dpi_ + 0.5));
    int y1 = static_cast<int>(std::floor((yInch + hInch) * dpi_ + 0.5));
    if (x1 <= x0) x1 = x0 + 1;          // a dot is never sub-pixel
    if (y1 <= y0) y1 = y0 + 1;

    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, current_.w); y1 = std::min(y1, current_.h);
    if (x0 >= x1 || y0 >= y1) return;

    const uint8_t ink = static_cast<uint8_t>(color_ | 0x1F);
    for (int y = y0; y < y1; ++y) {
        uint8_t* row = current_.pix.data() + static_cast<size_t>(y) * current_.w;
        for (int x = x0; x < x1; ++x) row[x] |= ink;
    }
    ++revision_;
}

// ─────────────────────────────────────────────────────────────────────────
// Text
// ─────────────────────────────────────────────────────────────────────────

const IwModelProfile& iwModelProfile(IwModel m)
{
    const size_t i = static_cast<size_t>(m);
    return kModels[i < static_cast<size_t>(IwModel::Count) ? i : 0];
}

void ImageWriter::setModel(IwModel m)
{
    if (m == model_ || static_cast<size_t>(m) >=
                           static_cast<size_t>(IwModel::Count)) return;
    model_ = m;

    // A different head means a different ribbon, a different power-on pitch
    // and different faces, so nothing already on the platen still means what
    // it did. Power-cycle rather than pretend continuity.
    const IwModelProfile& p = modelProfile();
    if (!p.colourRibbon) ribbon_ = Ribbon::Black;
    resetPrinter();
}

bool ImageWriter::modelHasEscP(uint32_t feature) const
{
    return modelProfile().lineage == IwLineage::EscP &&
           (modelProfile().escPFeatures & feature) != 0;
}

bool ImageWriter::modelIgnoresEsc(uint8_t cmd) const
{
    const IwModelProfile& p = modelProfile();
    for (size_t i = 0; i < p.ignoredEscCount; ++i)
        if (p.ignoredEsc[i] == cmd) return true;
    return false;
}

const IwRomBank& ImageWriter::currentBank() const
{
    // ESC a picks the quality; ESC p / ESC P pick proportional. Draft has no
    // proportional face on the real machine, so asking for both lands on the
    // correspondence proportional bank rather than silently staying fixed.
    //
    // A single-face head (ImageWriter I, Apple DMP) has no draft or NLQ bank
    // at all — `quality_` is pinned to Correspondence for it in the ESC a
    // handler, and the null checks here are the belt to that braces.
    const IwModelProfile& p = modelProfile();
    const bool prop = (style_ & kStyleProp) != 0;

    if (prop) {
        // NLQ has its own proportional bank; everything else uses the
        // correspondence one, which is also all a single-face head has.
        if (quality_ == Quality::NLQ && p.nlqProp) return *p.nlqProp;
        return *p.stdProp;
    }

    switch (quality_) {
    case Quality::Draft:  return p.draft    ? *p.draft    : *p.stdFixed;
    case Quality::NLQ:    return p.nlqFixed ? *p.nlqFixed : *p.stdFixed;
    case Quality::Correspondence:
    default:              return *p.stdFixed;
    }
}

const iwrom::IwGlyph* ImageWriter::romGlyph(uint8_t ch) const
{
    // Bit 7 is masked by the character generator (soft switch B-6) before it
    // reaches the ROM, exactly as `curMap_` handles it for the fallback font.
    const uint8_t code = static_cast<uint8_t>(ch & 0x7F);
    if (code < iwrom::kIwFirstCode || code > iwrom::kIwLastCode) return nullptr;

    const IwRomBank& bank = currentBank();
    const iwrom::IwLocale loc =
        kCharsetToLocale[switcha_ & kSwitchACharsetMask];

    // A locale substitution replaces one of the ten alternate-language code
    // points outright — this is the real ROM's own mechanism, and it is why
    // POM2 no longer has to approximate them with the nearest CP437 glyph.
    if (const iwrom::IwGlyph* o = iwrom::findOverride(
            bank.overrides, bank.overrideCount, loc, code))
        return o->width ? o : nullptr;

    const iwrom::IwGlyph& g = bank.glyphs[code - iwrom::kIwFirstCode];
    return g.width ? &g : nullptr;
}

double ImageWriter::glyphAdvance(uint8_t ch) const
{
    const IwRomBank& bank = currentBank();
    if (!bank.proportional) return 0.0;          // caller uses the pitch cell
    const iwrom::IwGlyph* g = romGlyph(ch);
    if (!g) return 0.0;
    // THE point of a proportional face: the advance is the glyph's own
    // escapement, measured in the dot units the pitch command established
    // (`ESC p` = 144/in, `ESC P` = 160/in). Before the ROMs landed, POM2
    // selected the pitch and then advanced by a fixed cell anyway, so
    // proportional text came out monospaced.
    const double unit = (definedUnit_ > 0) ? static_cast<double>(definedUnit_)
                                           : 144.0;
    return static_cast<double>(g->width) / unit;
}

void ImageWriter::renderGlyph(uint8_t ch)
{
    const IwRomBank& bank = currentBank();
    const iwrom::IwGlyph* g = romGlyph(ch);

    // The cell the head will advance by, so the glyph fills exactly what the
    // pitch (or its own escapement) reserved.
    double cellW = 1.0 / actcpi_;
    if (bank.proportional) {
        const double adv = glyphAdvance(ch);
        if (adv > 0.0) cellW = adv;
    }

    const int    cols = g ? g->width : 8;
    const double dotW = cellW / (cols > 0 ? cols : 8);
    const int    rows = g ? bank.rows : hgrpaint::kBBFontGlyphH;
    double dotH = g ? bank.rowPitch : 1.0 / 72.0;
    double top  = curY_;

    if (style_ & (kStyleSuperscript | kStyleSubscript | kStyleHalfHeight)) {
        dotH *= 2.0 / 3.0;
        // Superscript hugs the ascender line; the other two sit on the
        // baseline of the full-height cell.
        if (!(style_ & kStyleSuperscript))
            top += (8.0 / 72.0) - static_cast<double>(rows) * dotH;
    }

    // Bold on a real head is a second pass shifted half a dot — here the
    // dot is simply 1.5x wide, which is what that pass leaves on paper.
    const double inkW = (style_ & kStyleBold) ? dotW * 1.5 : dotW;
    const double shearSpan = (rows > 1) ? 1.0 / (rows - 1) : 0.0;

    if (g) {
        if (sound_) {
            // Pins struck, not columns: a full stop and a 'W' do not make the
            // same noise, and the sink turns pin COUNT into loudness.
            int pins = 0;
            for (int gx = 0; gx < cols; ++gx)
                for (int gy = 0; gy < rows; ++gy)
                    if (g->cols[gx] & (1u << gy)) ++pins;
            if (pins) sound_->strike(std::min(pins, 9));
        }
        for (int gx = 0; gx < cols; ++gx) {
            const uint32_t col = g->cols[gx];
            if (!col) continue;
            for (int gy = 0; gy < rows; ++gy) {
                if (!(col & (1u << gy))) continue;
                // Italics shear the cell by one dot over its height, matching
                // the reference's 0.20 FreeType x-shear (imagewriter.cpp:437-442).
                const double shear = (style_ & kStyleItalics)
                                   ? dotW * (rows - 1 - gy) * shearSpan : 0.0;
                fillDots(curX_ + gx * dotW + shear, top + gy * dotH, inkW, dotH);
            }
        }
        return;
    }

    // No ROM glyph for this code (control codes, the user-defined range, or a
    // bank that simply does not carry it) — fall back to the bundled CP437
    // font. Kept deliberately: it is the graceful degradation, and it is what
    // keeps the printer working if the ROM tables ever have to be dropped.
    const uint8_t fallback = curMap_[ch];
    for (int gy = 0; gy < hgrpaint::kBBFontGlyphH; ++gy) {
        for (int gx = 0; gx < hgrpaint::kBBFontGlyphW; ++gx) {
            if (!hgrpaint::bbFontPixel(fallback, gx, gy)) continue;
            const double shear = (style_ & kStyleItalics)
                               ? dotW * (7 - gy) * (1.0 / 7.0) : 0.0;
            fillDots(curX_ + gx * dotW + shear, top + gy * dotH, inkW, dotH);
        }
    }
}

void ImageWriter::acceptByte(uint8_t ch)
{
    // The front panel comes first. Powered off, the head is dead and the byte
    // is gone; deselected (offline), the printer is alive but the software
    // cannot reach it. Both DROP the byte rather than buffering it — a real
    // printer has no backlog to flush when you switch it back on, and
    // pretending otherwise would print a burst of stale output.
    if (!powered_ || !online_) return;

    ++bytesIn_;
    // Rolling raw capture — drops the oldest half when full so a runaway
    // job can't grow it without bound but the recent stream survives.
    if (raw_.size() >= kRawCaptureBytes)
        raw_.erase(raw_.begin(),
                   raw_.begin() + static_cast<std::ptrdiff_t>(raw_.size() / 2));
    raw_.push_back(ch);
    if (trace_) traceByte(ch);
    printCharInternal(ch);
}

void ImageWriter::armRepeat(const uint8_t* pat, uint8_t len, uint32_t count,
                            bool restoresMsb)
{
    repeatPatLen_ = (len == 0 || len > 3) ? 1 : len;
    for (uint8_t i = 0; i < repeatPatLen_; ++i) repeatPat_[i] = pat[i];
    repeatPatPos_      = 0;
    repeatRemaining_   = count;
    repeatRestoresMsb_ = restoresMsb;
    if (count == 0 && restoresMsb) msb_ = 0;   // `nnnn = 0000`: nothing owed
}

void ImageWriter::printRepeatUnit()
{
    if (repeatRemaining_ == 0) return;
    --repeatRemaining_;
    const uint8_t ch = repeatPat_[repeatPatPos_];
    if (++repeatPatPos_ >= repeatPatLen_) repeatPatPos_ = 0;
    printCharInternal(ch);
    if (repeatRemaining_ == 0 && repeatRestoresMsb_) {
        msb_ = 0;                     // ESC V / ESC U — see repeatPat_
        repeatRestoresMsb_ = false;
    }
}

void ImageWriter::printChar(uint8_t ch)
{
    // The immediate entry point — no mechanism between the byte and the
    // paper (`printBytes`, `flushPending`, `Speed::Instant`), so a repeat
    // run this byte starts is run out before returning. The paced drain in
    // tick() calls `acceptByte` instead and spends the run over as many
    // ticks as the mechanism needs (see repeatPat_).
    acceptByte(ch);
    while (repeatRemaining_ > 0) printRepeatUnit();
}

void ImageWriter::printCharInternal(uint8_t ch)
{
    // Bit 7 is masked for text but never for graphics data
    // (imagewriter.cpp:1260-1263).
    if (msb_ != 255 && bitGraph_.remBytes == 0) ch &= 0x7F;

    if (bitGraph_.remBytes > 0) { printBitGraph(ch); return; }
    if (processCommandChar(ch))  return;

    if (ch == 0x01) ch = 0x20;

    const double lineStart = curX_;
    renderGlyph(ch);

    // Slashed zero when soft switch B-1 is closed (imagewriter.cpp:1325).
    // Overstrike, not substitution: the real switch prints a slash THROUGH
    // the zero, and with a ROM face both glyphs come from the same bank, so
    // simply rendering '/' on top at the same origin is what the head does.
    if ((switchb_ & 1) && ch == '0') {
        if (romGlyph('0')) {
            renderGlyph('/');
        } else {
            const uint8_t saved = curMap_['0'];
            curMap_['0'] = curMap_['/'];
            renderGlyph('0');
            curMap_['0'] = saved;
        }
    }

    // A proportional face advances by the glyph's own escapement; a fixed
    // one by the pitch cell. `hmi_` (ESC 1..6 / the LQ drivers' explicit
    // motion index) still overrides both — it is the guest saying "move
    // exactly this far", which outranks the font.
    double advance = (hmi_ > 0.0) ? hmi_ : 1.0 / actcpi_;
    if (hmi_ <= 0.0) {
        const double prop = glyphAdvance(ch);
        if (prop > 0.0) advance = prop;
    }
    advance += extraIntraSpace_;
    curX_ += advance;

    if (style_ & kStyleUnderline) {
        // One pin below the cell, across the character just printed.
        fillDots(lineStart, curY_ + 8.0 / 72.0,
                 curX_ - lineStart, 1.0 / 72.0);
    }

    // Wrap when the next character would cross the right margin.
    if ((curX_ + advance) > rightMargin_) {
        if (sound_) sound_->carriageReturn(curX_ - leftMargin_);
        curX_ = leftMargin_;
        lineFeed();
    }
}

void ImageWriter::printBytes(const uint8_t* data, size_t n)
{
    if (!data) return;
    for (size_t i = 0; i < n; ++i) printChar(data[i]);
}

// ─────────────────────────────────────────────────────────────────────────
// Trace log
// ─────────────────────────────────────────────────────────────────────────

bool ImageWriter::startTrace(const std::string& path, std::string& err)
{
    stopTrace();
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path(), ec);
    trace_ = std::fopen(path.c_str(), "w");
    if (!trace_) {
        err = "cannot open " + path + " for writing";
        return false;
    }
    tracePath_   = path;
    traceOffset_ = 0;
    traceRowLen_ = 0;
    std::fprintf(trace_,
        "# POM2 ImageWriter II trace\n"
        "# Every byte the printer consumed, decoded. Columns:\n"
        "#   [t]      seconds of mechanism time since the trace opened\n"
        "#   RX       hex dump of the input stream (offset = byte index)\n"
        "#   CMD      a completed escape sequence, with its parameters\n"
        "#   GFX      bit-image setup (density / columns / bytes per column)\n"
        "#   PAGE     sheet ejected\n"
        "#   HOST     host-side event (queue depth, BUSY, stalls)\n"
        "#\n"
        "# If a printout is noise, look at CMD: a driver talking another\n"
        "# printer's dialect shows up as commands this printer never got\n"
        "# (or as RX bytes that should have been graphics data).\n\n");
    std::fflush(trace_);
    return true;
}

void ImageWriter::stopTrace()
{
    if (!trace_) return;
    traceFlushRow();
    std::fprintf(trace_, "# trace closed after %llu bytes\n",
                 static_cast<unsigned long long>(traceOffset_));
    std::fclose(trace_);
    trace_ = nullptr;
}

void ImageWriter::traceEvent(const char* fmt, ...)
{
    if (!trace_) return;
    traceFlushRow();
    std::fprintf(trace_, "[%8.3f] HOST ", traceClock_);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(trace_, fmt, ap);
    va_end(ap);
    std::fputc('\n', trace_);
    std::fflush(trace_);
}

void ImageWriter::traceFlushRow()
{
    if (!trace_ || traceRowLen_ == 0) return;
    std::fprintf(trace_, "[%8.3f] RX   %06llX  ", traceClock_,
                 static_cast<unsigned long long>(traceOffset_ - traceRowLen_));
    for (int i = 0; i < 16; ++i) {
        if (i < traceRowLen_) std::fprintf(trace_, "%02X ", traceRow_[i]);
        else                  std::fprintf(trace_, "   ");
        if (i == 7) std::fputc(' ', trace_);
    }
    std::fputc('|', trace_);
    for (int i = 0; i < traceRowLen_; ++i) {
        const uint8_t c = traceRow_[i] & 0x7F;
        std::fputc((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.', trace_);
    }
    std::fprintf(trace_, "|\n");
    traceRowLen_ = 0;
    std::fflush(trace_);
}

void ImageWriter::traceByte(uint8_t ch)
{
    if (!trace_) return;
    traceRow_[traceRowLen_++] = ch;
    ++traceOffset_;
    if (traceRowLen_ == 16) traceFlushRow();
}

void ImageWriter::traceCommand()
{
    if (!trace_) return;
    traceFlushRow();
    const bool isFs = (escCmd_ & 0x800) != 0;
    const uint8_t c = static_cast<uint8_t>(escCmd_ & 0xFF);
    std::string params;
    for (uint8_t i = 0; i < numParam_; ++i) {
        const uint8_t p = params_[i] & 0x7F;
        params += (p >= 0x20 && p < 0x7F) ? static_cast<char>(p) : '.';
    }
    std::fprintf(trace_, "[%8.3f] CMD  %s %c ($%02X)%s%s\n", traceClock_,
                 isFs ? "US " : "ESC",
                 (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?', c,
                 params.empty() ? "" : "  params=", params.c_str());
    std::fflush(trace_);
}

// ─────────────────────────────────────────────────────────────────────────
// Mechanism pacing — bytes arrive at bus speed, dots land at head speed
// ─────────────────────────────────────────────────────────────────────────

const char* ImageWriter::ribbonName(Ribbon r)
{
    return r == Ribbon::Black ? "Black (single band)"
                              : "Four-colour (yellow/magenta/cyan/black)";
}

const char* ImageWriter::autoFeedName(AutoFeed m)
{
    switch (m) {
        case AutoFeed::On:  return "Always (printer supplies the feed)";
        case AutoFeed::Off: return "Never (the guest supplies it)";
        default:            return "Auto (follow what the guest sends)";
    }
}

void ImageWriter::setAutoFeedMode(AutoFeed m)
{
    if (m >= AutoFeed::Count) m = AutoFeed::Auto;
    feedMode_       = m;
    feedLatchedOff_ = false;      // re-arm the detector on any change
    crJustFed_      = false;
}

const char* ImageWriter::qualityName(Quality q)
{
    switch (q) {
        case Quality::Draft:  return "Draft";
        case Quality::NLQ:    return "Near letter quality";
        case Quality::Correspondence:
        default:              return "Correspondence";
    }
}

const char* ImageWriter::speedName(Speed s)
{
    switch (s) {
        case Speed::Draft: return "Draft (250 cps)";
        case Speed::NLQ:   return "Near letter quality (45 cps)";
        default:           return "Instant (no mechanism delay)";
    }
}

void ImageWriter::setSpeed(Speed s)
{
    if (s >= Speed::Count) s = Speed::Draft;
    speed_ = s;
    // Switching to Instant must not strand a half-printed job.
    if (speed_ == Speed::Instant) flushPending();
}

void ImageWriter::queueBytes(const uint8_t* data, size_t n)
{
    if (!data || n == 0) return;
    pending_.insert(pending_.end(), data, data + n);

    // Past kMaxBacklog the mechanism gives up on Draft/NLQ pacing — but it
    // catches up in tick(), at a bounded rate. Printing it here would run
    // the whole backlog on the UI thread in one frame (see kMaxBacklog).
    if (pendingBytes() > kMaxBacklog) catchUp_ = true;

    // A guest that sustainably outruns even the catch-up rate — a form
    // feed loop, where every one-byte FF asks for a whole sheet — still
    // may not grow the heap without bound. Past the hard ceiling the
    // oldest input is dropped, which is the same rule the page stack
    // (kMaxPages / droppedPageCount) and the SSC tap spool already follow:
    // bound the memory, count what was lost, and say so in the trace.
    // Truncating a printout is bad; freezing the emulator is worse.
    if (pendingBytes() > kHardBacklog) {
        const size_t drop = pendingBytes() - kHardBacklog;
        pendingHead_  += drop;
        droppedInput_ += drop;
        traceEvent("HOST: backlog over %zu KiB — dropped %zu input byte%s "
                   "(%zu total)", kHardBacklog >> 10, drop,
                   drop == 1 ? "" : "s", droppedInput_);
        // Advancing the cursor only makes the bytes unreachable — the
        // vector still holds them. Compact, or the "hard ceiling" bounds
        // nothing at all between two ticks.
        compactPending();
    }
}

void ImageWriter::compactPending()
{
    if (pendingHead_ >= pending_.size()) {
        pending_.clear();
        pendingHead_ = 0;
    } else if (pendingHead_ >= 8192) {
        // Compact instead of growing without bound on a long job.
        pending_.erase(pending_.begin(),
                       pending_.begin() +
                           static_cast<std::ptrdiff_t>(pendingHead_));
        pendingHead_ = 0;
    }
}

void ImageWriter::flushPending()
{
    // What a partly-expanded repeat still owes comes off the head first —
    // it was asked for before anything queued behind it.
    while (repeatRemaining_ > 0) printRepeatUnit();
    while (pendingHead_ < pending_.size()) printChar(pending_[pendingHead_++]);
    pending_.clear();
    pendingHead_ = 0;
    credit_      = 0.0;
    stalledFor_  = 0.0;   // the queue is gone; don't arm the next job's
                          // watchdog with time this one spent stalled
    catchUp_     = false; // nothing left to catch up on
}

double ImageWriter::byteCost(uint8_t ch) const
{
    if (speed_ == Speed::Instant) return 0.0;

    // A single-face head has one mechanical speed; only the ImageWriter II
    // pays the NLQ penalty, because only it has an NLQ pass to make.
    const IwModelProfile& mp = modelProfile();
    const double cps = (mp.qualityTiers && speed_ == Speed::NLQ)
                           ? mp.nlqCps : mp.draftCps;
    const double ips = cps / kQuotedCpi;      // carriage inches per second

    // Bit-image data: the byte is a dot column (or a third of one on the
    // LQ's 24-pin pitches), not a glyph. Same head, half the sweep rate.
    if (bitGraph_.remBytes > 0) {
        // Past the right margin the carriage is against the stop and the
        // column is discarded (printBitGraph), so it costs no head travel.
        // Charging for it is what made an over-long `ESC V`/`ESC U` run hold
        // the guest's BUSY line for tens of emulated seconds printing nothing.
        if (curX_ >= rightMargin_) return 0.0;
        const double colsPerSec = (ips * 0.5) * bitGraph_.horizDens;
        const double perColumn  = (colsPerSec > 0.0) ? 1.0 / colsPerSec : 0.0;
        const uint8_t perCol    = bitGraph_.bytesColumn ? bitGraph_.bytesColumn : 1;
        return perColumn / perCol;
    }

    // Mid-escape-sequence bytes (the command letter and its ASCII digit
    // parameters) never move the mechanism — they land in a register.
    if (escSeen_ || fsSeen_ || numParam_ < neededParam_) return 0.0;

    switch (ch & 0x7F) {
        case 0x0D: {   // CR — carriage back to the left margin
            // Draft is bidirectional (the head prints on the return
            // sweep), so a CR costs only the direction change; NLQ is
            // unidirectional and pays the full slew back.
            double t = (speed_ == Speed::NLQ)
                     ? std::max(0.0, curX_ - leftMargin_) / ips
                     : 0.02;
            // Paper transport time is positive in both directions (ESC r
            // makes lineSpacing_ negative; a negative cost *credited* the
            // pacing budget and dumped the whole queue in one frame).
            if (autoFeedActive()) t += std::fabs(lineSpacing_) / kFeedIps;
            return t;
        }
        case 0x0A:     // LF — one line of paper transport
            return std::fabs(lineSpacing_) / kFeedIps;
        case 0x0C:     // FF — slew whatever is left of the sheet
            return std::max(0.5, bottomMargin_ - curY_) / kFeedIps;
        default:
            break;
    }
    if ((ch & 0x7F) < 0x20) return 0.0;        // other control codes
    return 1.0 / cps;                          // one printed character
}

double ImageWriter::nextUnitCost() const
{
    // An outstanding repeat byte is a unit of work that consumes no input
    // byte, so it — not `pending_[pendingHead_]` — is what the credit cap,
    // the drain and the stall watchdog have to price when one is pending.
    if (repeatRemaining_ > 0) return byteCost(repeatPat_[repeatPatPos_]);
    if (pendingHead_ < pending_.size()) return byteCost(pending_[pendingHead_]);
    return 0.0;
}

void ImageWriter::tick(double dt)
{
    if (!busy()) {
        pending_.clear();
        pendingHead_ = 0;
        credit_      = 0.0;
        stalledFor_  = 0.0;   // idle printer: the next job starts fresh
        catchUp_     = false;
        return;
    }
    if (speed_ == Speed::Instant) { flushPending(); return; }

    // Behind by more than kMaxBacklog: pacing is suspended and the head
    // runs flat out — but only for a bounded slice of this tick, so the
    // window keeps repainting while it catches up. It stays armed until
    // the queue is EMPTY, not merely back under the threshold: stopping
    // at the threshold would hand ~512 KiB back to the 250 cps model and
    // spend half an hour of wall clock printing it, which is neither what
    // the memory bound wanted nor what the user is waiting for.
    if (catchUp_) {
        // Budgeted in units of WORK, not in queue positions: an outstanding
        // repeat byte is a unit that consumes no input byte, and `pending_`
        // may even be empty while a run is still owed.
        size_t       work        = 0;
        const size_t sheetBudget = sheetsEjected_ + kCatchUpSheets;
        while (busy() && work < kCatchUpBytes && sheetsEjected_ < sheetBudget) {
            ++work;
            if (repeatRemaining_ > 0) { printRepeatUnit(); continue; }
            acceptByte(pending_[pendingHead_++]);
        }
        credit_     = 0.0;
        stalledFor_ = 0.0;   // nothing is unaffordable while un-paced
        if (!busy()) catchUp_ = false;
        compactPending();
        return;
    }

    // Bank the elapsed time, capped so a hidden window or a long host
    // stall doesn't dump half a page in one frame. The cap has to leave
    // room for the byte at the head of the queue: a form feed near the top
    // of a Letter sheet costs 2.2 s of paper transport, and a flat 1 s cap
    // meant that byte could NEVER be afforded — the queue stalled forever,
    // BUSY stayed asserted, and the guest spun in its firmware ACK loop
    // (Print Shop froze on every page eject).
    if (dt > 0.0) {
        traceClock_ += dt;
        credit_ = std::min(credit_ + dt, std::max(kMaxCredit, nextUnitCost()));
    }

    // The paced loop needs the SAME work budget as the catch-up one. Credit
    // bounds the mechanism SECONDS a tick may spend, and most bytes cost
    // some — but not all do: an escape sequence's parameter digits are free,
    // and so is every bit-image column past the right margin (the carriage
    // is against the stop and the column is discarded). An `ESC V 9999` flood
    // is therefore millions of zero-cost units that credit can never stop,
    // drained in one UI-thread tick with the window frozen for it.
    size_t units = 0;
    while (busy() && units < kCatchUpBytes) {
        const double cost = nextUnitCost();    // state-dependent: read first
        if (cost > credit_) break;
        credit_ -= cost;
        ++units;
        // A repeat byte the previous unit left outstanding comes before
        // the next input byte — the run is what the head is busy with.
        if (repeatRemaining_ > 0) { printRepeatUnit(); continue; }
        acceptByte(pending_[pendingHead_++]);
    }

    // Watchdog. Nothing should ever be unaffordable for this long — but a
    // wedged printer takes the guest down with it (it waits on ACK), so a
    // cost-model mistake must degrade to "printed late", never to a hang.
    if (units == 0 && dt > 0.0) {
        stalledFor_ += dt;
        // The threshold has to clear the dearest byte the cost model can
        // legitimately produce, or the watchdog fires on a byte that was
        // merely SLOW. `ESC H 9999` + FF is 69" of paper at 5 ips = 13.9 s
        // of honest mechanism time, and a flat 10 s cap cut it short and
        // logged a STALL for a printer that was working correctly. The
        // credit cap already grows to `head` (see above), so waiting that
        // long is the paced answer; the watchdog is only for a cost the
        // model can never afford at all.
        const double patience = std::max(kStallSeconds, nextUnitCost() * 1.5);
        if (stalledFor_ >= patience) {
            if (repeatRemaining_ > 0) {
                traceEvent("STALL: repeat byte $%02X unaffordable for %.1f s "
                           "(cost %.3f s, credit %.3f s) — forcing it through "
                           "(%u left)",
                           repeatPat_[repeatPatPos_], stalledFor_,
                           nextUnitCost(), credit_, repeatRemaining_);
                printRepeatUnit();
            } else {
                const uint8_t ch = pending_[pendingHead_];
                traceEvent("STALL: byte $%02X unaffordable for %.1f s "
                           "(cost %.3f s, credit %.3f s) — forcing it through",
                           ch, stalledFor_, byteCost(ch), credit_);
                ++pendingHead_;
                acceptByte(ch);
            }
            credit_     = 0.0;
            stalledFor_ = 0.0;
        }
    } else {
        stalledFor_ = 0.0;
    }

    compactPending();
}

// ─────────────────────────────────────────────────────────────────────────
// Bit-image graphics (imagewriter.cpp:1432-1603)
// ─────────────────────────────────────────────────────────────────────────

void ImageWriter::setupBitImage(uint8_t dens, uint32_t numCols)
{
    if (dens > 15) return;              // reference logs and drops

    bitGraph_.horizDens   = kBitImageHorizDpi[dens];
    bitGraph_.swallow     = false;
    bitGraph_.vertDens    = (dens < 8) ? 72 : 216;
    bitGraph_.adjacent    = true;
    bitGraph_.bytesColumn = (dens < 8) ? 1 : 3;
    bitGraph_.remBytes    = numCols * bitGraph_.bytesColumn;
    bitGraph_.readBytesColumn = 0;
    bitGraph_.msbTop      = false;      // C. Itoh: bit 0 is the top dot

    if (trace_) {
        traceFlushRow();
        std::fprintf(trace_,
            "[%8.3f] GFX  %u dpi x %u dpi, %u columns, %u byte%s/column "
            "(%u data bytes follow) at %.2f\"\n",
            traceClock_, bitGraph_.horizDens, bitGraph_.vertDens, numCols,
            bitGraph_.bytesColumn, bitGraph_.bytesColumn == 1 ? "" : "s",
            bitGraph_.remBytes, curX_);
        std::fflush(trace_);
    }
}

void ImageWriter::printBitGraph(uint8_t ch)
{
    bitGraph_.column[bitGraph_.readBytesColumn++] = ch;
    --bitGraph_.remBytes;

    // Only paint once a whole column has arrived.
    if (bitGraph_.readBytesColumn < bitGraph_.bytesColumn) return;

    // The carriage stops at the right margin. Every other head-motion path in
    // this file honours it — the text advance wraps on it (printCharInternal
    // below), ESC F / ESC ' refuse a move past it — but the graphics advance
    // did not, so a long bit-image run walked `curX_` arbitrarily far off the
    // sheet: `ESC V 9060 <col>` parks the head 113 inches out on an 8.5-inch
    // page. Nothing reached the paper (fillDots clips to the raster), and yet
    // the mechanism model charged the full dot-column rate for every one of
    // those columns — ~22 emulated seconds of BUSY for a line the hardware
    // finishes in two, with a status line reporting a head position no
    // ImageWriter can reach. `rightMargin_` is only ever the paper width (no
    // command narrows it), so discarding here removes exactly the columns
    // `fillDots` was already throwing away: the ink on the sheet is
    // unchanged, which is what `imagewriter_smoke` now pins.
    //
    // DISCARDED, not wrapped: wrapping a bit image would corrupt it, and an
    // over-long graphics line on the real printer loses its excess columns.
    if (curX_ >= rightMargin_) {
        curX_ = rightMargin_;          // parked against the stop, not past it
        bitGraph_.readBytesColumn = 0;
        return;
    }

    if (sound_) {
        int pins = 0;
        for (uint8_t i = 0; i < bitGraph_.bytesColumn; ++i) {
            uint8_t v = bitGraph_.column[i];
            while (v) { pins += (v & 1); v = static_cast<uint8_t>(v >> 1); }
        }
        if (pins) sound_->strike(pins);
    }

    if (bitGraph_.swallow) {                 // eaten, not printed
        bitGraph_.readBytesColumn = 0;
        return;
    }

    const double oldY  = curY_;
    const double dotH  = 1.0 / bitGraph_.vertDens;
    const double dotW  = 1.0 / bitGraph_.horizDens;

    // ESC t shifts the LQ column down by n/216 in (imagewriter.cpp:1574-1577).
    if (printRes_ > 7 && verticalDot_ != 0)
        curY_ += static_cast<double>(verticalDot_) / bitGraph_.vertDens;

    for (uint8_t i = 0; i < bitGraph_.bytesColumn; ++i) {
        // C. Itoh packs pin 1 (top) in bit 0; Epson ESC/P packs it in bit 7.
        // Walking the wrong way mirrors every image vertically in 8-pixel
        // stripes — which still looks like a picture, so it is pinned by a
        // round trip rather than by eye.
        for (int b = 0; b < 8; ++b) {
            const unsigned mask = bitGraph_.msbTop ? (0x80u >> b) : (1u << b);
            if (bitGraph_.column[i] & mask) fillDots(curX_, curY_, dotW, dotH);
            curY_ += dotH;
        }
    }

    curY_ = oldY;
    bitGraph_.readBytesColumn = 0;
    curX_ += dotW;
}

// ─────────────────────────────────────────────────────────────────────────
// Command interpreter (imagewriter.cpp:497-1217, ported verbatim)
// ─────────────────────────────────────────────────────────────────────────

bool ImageWriter::processCommandChar(uint8_t ch)
{
    // The Epson speaks a different grammar over the same mechanism. Routed
    // here rather than inside the switch below because the two command sets
    // COLLIDE: ESC G is graphics on the C. Itoh and double-strike on the
    // Epson, ESC A is 1/6 in spacing on one and n/72 in on the other.
    switch (modelProfile().lineage) {
    case IwLineage::EscP:   return processEpsonChar(ch);
    case IwLineage::Diablo: return processDiabloChar(ch);
    // PostScript is intercepted upstream; anything arriving here is a job
    // that missed the interception, and printing it as text is the
    // diagnosable outcome (see IwModel::LaserWriterPostScript).
    case IwLineage::PostScript: return false;
    case IwLineage::CItoh:  break;
    }

    // "The previous byte was a CR we line-fed for" — only an LF landing
    // immediately after one counts as the guest supplying its own feed.
    const bool wasCrFed = crJustFed_;
    crJustFed_ = false;

    // ── Phase 1: the byte right after ESC / US selects the command ──────
    if (escSeen_ || fsSeen_) {
        escCmd_ = ch;
        if (fsSeen_) escCmd_ |= 0x800;
        escSeen_ = fsSeen_ = false;
        numParam_ = 0;

        switch (escCmd_) {
        case 0x21: case 0x22: case 0x24: case 0x2b: case 0x2e:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
        case 0x35: case 0x36: case 0x3c: case 0x3e: case 0x3f:
        case 0x41: case 0x42: case 0x45: case 0x4d: case 0x4e:
        case 0x4f: case 0x50: case 0x51: case 0x57: case 0x58:
        case 0x59: case 0x63: case 0x65: case 0x66: case 0x6b:
        case 0x6d: case 0x6e: case 0x6f: case 0x70: case 0x71:
        case 0x72: case 0x77: case 0x78: case 0x79: case 0x7a:
            neededParam_ = 0;
            break;
        case 0x3d:  // ESC = n  internal font ID
        case 0x40:  // ESC @ n  select output bin
        case 0x4b:  // ESC K n  select printing colour
        case 0x61:  // ESC a n  select font
        case 0x6c:  // ESC l n  insert CR before LF/FF
        case 0x73:  // ESC s n  intercharacter space
        case 0x74:  // ESC t n  shift printing down n/216 in
        case 0x833: // US n     feed n blank lines
            neededParam_ = 1;
            break;
        case 0x44:  // ESC D nn  close (set) soft switches
        case 0x54:  // ESC T nn  line spacing nn/144 in
        case 0x5a:  // ESC Z nn  open (clear) soft switches
            neededParam_ = 2;
            break;
        case 0x4c:  // ESC L nnn  left margin at column nnn
        case 0x67:  // ESC g nnn  graphics, nnn*8 data bytes
        case 0x75:  // ESC u nnn  add one tab stop
            neededParam_ = 3;
            break;
        case 0x28:  // ESC ( nnn,  set horizontal tabs
            numHorizTabs_ = 0;
            [[fallthrough]];
        case 0x29:  // ESC ) nnn,  delete horizontal tabs
        case 0x43:  // ESC C nnnn  hi-res graphics
        case 0x47:  // ESC G nnnn  graphics
        case 0x46:  // ESC F nnnn  head nnnn dots from left margin
        case 0x48:  // ESC H nnnn  page length nnnn/144
        case 0x53:  // ESC S nnnn  graphics
        case 0x52:  // ESC R nnn c repeat character
        case 0x68:  // ESC h nnnn  head nnnn hi-res dots from left margin
            neededParam_ = 4;
            break;
        case 0x56:  // ESC V nnnn c   repeat dot column
            neededParam_ = 5;
            msb_ = 255;
            break;
        case 0x55:  // ESC U nnnn abc repeat hi-res dot column
            neededParam_ = 7;
            msb_ = 255;
            break;
        case 0x27:  // ESC '  select user-defined set
        case 0x49:  // ESC I  define user-defined characters
            // Not supported by the reference either. neededParam_ must be
            // cleared here: leaving the previous command's count armed made
            // phase 2 swallow the next 1-6 printable bytes as parameters.
            neededParam_ = 0;
            escCmd_ = 0;
            return true;
        default:
            neededParam_ = 0;
            escCmd_      = 0;
            return true;
        }

        if (neededParam_ > 0) return true;
    }

    // ── Phase 2: accumulate parameters ──────────────────────────────────
    if (numParam_ < neededParam_) {
        params_[numParam_++] = ch;
        if (numParam_ < neededParam_) return true;
    }
    if (escCmd_ == 0) {
        // ── Phase 3: bare control codes ─────────────────────────────────
        switch (ch) {
        case 0x00: return true;                 // NUL ignored
        case 0x07: return true;                 // BEL
        case 0x08: {                            // BS
            const double step = (hmi_ > 0.0) ? hmi_ : 1.0 / actcpi_;
            if (curX_ - step >= leftMargin_) curX_ -= step;
            return true;
        }
        case 0x09: {                            // HT
            // NEAREST stop to the right, not the farthest. The reference
            // (imagewriter.cpp:1131-1141) keeps overwriting `moveTo` as it
            // scans, so it lands on the LAST stop past the head — with
            // stops at 10/20/30 chars the first TAB jumped to 30 and every
            // later TAB was a no-op, which turns any columnar report into
            // one ragged column. A deliberate deviation from the
            // reference, matching the ImageWriter II Technical Reference.
            double moveTo = -1.0;
            for (uint8_t i = 0; i < numHorizTabs_; ++i)
                if (horiztabs_[i] > curX_ &&
                    (moveTo < 0.0 || horiztabs_[i] < moveTo))
                    moveTo = horiztabs_[i];
            if (moveTo > 0.0 && moveTo < rightMargin_) curX_ = moveTo;
            return true;
        }
        case 0x0b:                              // VT
            if (numVertTabs_ == 0) {
                curX_ = leftMargin_;            // all tabs cancelled → CR
            } else {
                double moveTo = -1.0;              // nearest stop below —
                for (uint8_t i = 0; i < numVertTabs_; ++i)   // see HT above
                    if (verttabs_[i] > curY_ &&
                        (moveTo < 0.0 || verttabs_[i] < moveTo))
                        moveTo = verttabs_[i];
                if (moveTo > bottomMargin_ - lineSpacing_ || moveTo < 0.0)
                    newPage(true, false);
                else
                    curY_ = moveTo;
            }
            return true;
        case 0x0c:                              // FF
            newPage(true, true);
            return true;
        case 0x0d:                              // CR
            curX_ = leftMargin_;
            if (switcha_ & kSwitchALfAfterCr) lineFeed();
            if (!autoFeedActive()) return true;
            lineFeed();
            crJustFed_ = true;    // an LF right after this one is the
            return true;          // guest's own — see the LF case
        case 0x0a:                              // LF
            if (wasCrFed) {
                // CR+LF from the guest: it manages its own line feeds, so
                // the feed the CR just did was ours to give and this LF
                // would double-space. Swallow it — and in Auto mode stop
                // feeding on CR at all from here on, which is what lets a
                // colour driver overprint its passes (Print Shop puts a
                // bare CR between its yellow/cyan/magenta passes).
                if (feedMode_ == AutoFeed::Auto && !feedLatchedOff_) {
                    feedLatchedOff_ = true;
                    if (trace_)
                        traceEvent("auto line-feed OFF — the guest sent its "
                                   "own LF after a CR");
                }
                return true;
            }
            lineFeed();
            return true;
        case 0x0e:                              // SO  double width on
            style_ |= kStyleDoubleWidth;  updateMetrics(); return true;
        case 0x0f:                              // SI  double width off
            style_ &= ~kStyleDoubleWidth; updateMetrics(); return true;
        case 0x11: return true;                 // DC1 select printer
        case 0x12:                              // DC2 cancel condensed
            hmi_ = -1.0;
            style_ &= ~kStyleCondensed;   updateMetrics(); return true;
        case 0x13: return true;                 // DC3 deselect printer
        case 0x14: return true;                 // DC4
        case 0x18: return true;                 // CAN
        case 0x1b: escSeen_ = true;  return true;   // ESC
        case 0x1f: fsSeen_  = true;  return true;   // US
        default:   return false;                // printable
        }
    }

    // ── Phase 4: execute the completed command ──────────────────────────
    if (trace_) traceCommand();
    // Several commands take their parameters as ASCII digit strings with
    // leading spaces; normalise those to '0' the way the reference does.
    // Unlike the reference (which blanket-converts params[0..3]) the count
    // is the number of *digit* positions, so `ESC R nnn ' '` still repeats
    // a space instead of printing zeros.
    auto spacesToZeros = [&](int digits) {
        for (int i = 0; i < digits && i < 20; ++i)
            if (params_[i] == ' ') params_[i] = '0';
    };
    auto param3 = [&]() {
        return paramDigit(params_[0]) * 100 + paramDigit(params_[1]) * 10 +
               paramDigit(params_[2]);
    };
    auto param4 = [&]() {
        return paramDigit(params_[0]) * 1000 + paramDigit(params_[1]) * 100 +
               paramDigit(params_[2]) * 10 + paramDigit(params_[3]);
    };

    // A command this head has no hardware for is DROPPED here, after its
    // parameter bytes have been collected — the manuals' rule is that an
    // unrecognised code goes away with its ESC, and letting the parameter
    // fall through to the text path would print it.
    // NOTE the escCmd_ = 0: the tail of this function clears it, and an early
    // return that skips that leaves the parser still armed — every following
    // byte is then eaten as a parameter and the rest of the job prints
    // nothing at all. (Which is exactly what happened first time round.)
    if (modelIgnoresEsc(escCmd_)) { escCmd_ = 0; return true; }

    switch (escCmd_) {
    case 0x61: {                                // ESC a n  select quality
        // Table 4-1: 0 = correspondence, 1 = draft, 2 = NLQ. Tolerates the
        // ASCII digit and the raw value, as the real firmware does.
        // A single-face head has no tiers, so the parameter is consumed (see
        // the ignored-command list) and the quality stays pinned.
        if (!modelProfile().qualityTiers) break;
        const uint8_t sel = static_cast<uint8_t>(params_[0] & 0x0F);
        if      (sel == 1) quality_ = Quality::Draft;
        else if (sel == 2) quality_ = Quality::NLQ;
        else               quality_ = Quality::Correspondence;
        break;
    }

    case 0x73:                                  // ESC s n  intercharacter space
        if (style_ & kStyleProp) {
            extraIntraSpace_ = paramDigit(params_[0]) / 120.0;
            updateMetrics();
        }
        break;

    case 0x46: {                                // ESC F nnnn  absolute X
        spacesToZeros(4);
        const double unit = definedUnit_ < 0 ? 72.0 : definedUnit_;
        const double newX = leftMargin_ + param4() / unit;
        if (newX <= rightMargin_) curX_ = newX;
        break;
    }
    case 0x68: {                                // ESC h nnnn  absolute hi-res X
        spacesToZeros(4);
        const double unit = definedUnit_ < 0 ? 72.0 : definedUnit_ * 2.0;
        const double newX = leftMargin_ + param4() / unit;
        if (newX <= rightMargin_) curX_ = newX;
        break;
    }

    case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: {
        // ESC 1..6 — add n/120" of intercharacter space (proportional
        // only), the same knob `ESC s n` sets. The reference
        // (imagewriter.cpp:665-678) assigns `curX_ = n/unit` instead: an
        // ABSOLUTE position a fraction of an inch from the left edge, so
        // `ESC 3` mid-line threw the head from 1.25" back to 0.02" —
        // outside the left margin — and destroyed every justified line a
        // proportional driver (AppleWorks, the LQ GS/OS driver) produced.
        // A deliberate deviation from the reference, matching the
        // ImageWriter II Technical Reference.
        if (style_ & kStyleProp) {
            extraIntraSpace_ = (escCmd_ - '0') / 120.0;
            updateMetrics();
        }
        break;
    }

    case 0x47: case 0x53:                       // ESC G/S nnnn  graphics
        spacesToZeros(4);
        printRes_ &= ~8;
        setupBitImage(printRes_, static_cast<uint32_t>(param4()));
        break;
    case 0x43:                                  // ESC C nnnn  hi-res graphics
        spacesToZeros(4);
        printRes_ |= 8;
        setupBitImage(printRes_, static_cast<uint32_t>(param4()));
        break;
    case 0x67:                                  // ESC g nnn  graphics (*8)
        spacesToZeros(3);
        printRes_ &= ~8;
        setupBitImage(printRes_, static_cast<uint32_t>(param3()) * 8u);
        break;

    // ESC V / ESC U are ESC G / ESC C with the data bytes implied: nnnn
    // columns of one repeated pattern. Setting the bit image up ONCE for
    // the whole run (rather than per column, as the reference does) leaves
    // the outstanding bytes flowing through `printBitGraph`, which is what
    // prices them at the dot-column rate and lets the drain yield mid-run.
    // A stream of `ESC U 9999` used to cost 1.4 s inside one catch-up tick,
    // against 9 ms for the same backlog of plain text — see repeatPat_.
    case 0x56: {                                // ESC V nnnn c  repeat column
        spacesToZeros(4);
        printRes_ &= ~8;
        setupBitImage(printRes_, static_cast<uint32_t>(param4()));
        armRepeat(&params_[4], 1, bitGraph_.remBytes, true);
        break;
    }
    case 0x55: {                                // ESC U nnnn abc  hi-res repeat
        spacesToZeros(4);
        printRes_ |= 8;
        setupBitImage(printRes_, static_cast<uint32_t>(param4()));
        armRepeat(&params_[4], 3, bitGraph_.remBytes, true);
        break;
    }

    case 0x74:                                  // ESC t n  shift down n/216
        verticalDot_ = static_cast<uint8_t>(paramDigit(params_[0]));
        break;

    // Pitch selection — each also fixes the graphics density and the unit
    // used by ESC F / ESC h (imagewriter.cpp:765-836).
    case 0x6e: cpi_ =  9.0; printRes_ = 0; definedUnit_ =  72;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x4e: cpi_ = 10.0; printRes_ = 1; definedUnit_ =  80;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x45: cpi_ = 12.0; printRes_ = 2; definedUnit_ =  96;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x65: cpi_ = 13.4; printRes_ = 3; definedUnit_ = 107;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x71: cpi_ = 15.0; printRes_ = 4; definedUnit_ = 120;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x51: cpi_ = 17.0; printRes_ = 5; definedUnit_ = 136;
               style_ &= ~kStyleProp; extraIntraSpace_ = 0; updateMetrics(); break;
    case 0x70: cpi_ = 10.0; printRes_ = 6; definedUnit_ = 144;
               style_ |= kStyleProp;  updateMetrics(); break;
    case 0x50: cpi_ = 12.0; printRes_ = 7; definedUnit_ = 160;
               style_ |= kStyleProp;  updateMetrics(); break;

    case 0x54:                                  // ESC T nn  nn/144 in spacing
        lineSpacing_ = (paramDigit(params_[0]) * 10 +
                        paramDigit(params_[1])) / 144.0;
        break;
    case 0x42: lineSpacing_ = 1.0 / 8.0; break; // ESC B
    case 0x41: lineSpacing_ = 1.0 / 6.0; break; // ESC A

    case 0x58: style_ |= kStyleUnderline;  break;   // ESC X
    case 0x59: style_ &= ~kStyleUnderline; break;   // ESC Y

    case 0x3c: case 0x3e: break;                // uni/bidirectional — no head
    case 0x63: resetPrinter(); break;           // ESC c  initialize

    case 0x48:                                  // ESC H nnnn  page length
        spacesToZeros(4);
        // Clamped to something a sheet can be. `ESC H 0000` set a
        // zero-length page, so every single line feed ejected: three LFs
        // produced three sheets and a real job blew the whole 32-page
        // stack away in blanks. The upper end is the physical raster —
        // past it the head runs off the bottom and the ink is dropped
        // silently, with no eject to show for it.
        pageHeightIn_ = std::clamp(param4() / 144.0,
                                   1.0, defaultPageHeight_);
        bottomMargin_ = pageHeightIn_;
        topMargin_    = 0.0;
        updateSwitch();
        break;

    case 0x21: style_ |= kStyleBold;  updateMetrics(); break;   // ESC !
    case 0x22: style_ &= ~kStyleBold; updateMetrics(); break;   // ESC "
    case 0x78: style_ |= kStyleSuperscript; break;              // ESC x
    case 0x79: style_ |= kStyleSubscript;   break;              // ESC y
    case 0x7a: style_ &= ~(kStyleSuperscript | kStyleSubscript); break; // ESC z
    case 0x77: style_ |= kStyleHalfHeight;  break;              // ESC w
    case 0x57: style_ &= ~kStyleHalfHeight; break;              // ESC W

    case 0x72: if (lineSpacing_ > 0) lineSpacing_ *= -1; break; // ESC r reverse
    case 0x66: if (lineSpacing_ < 0) lineSpacing_ *= -1; break; // ESC f forward

    // ESC a is handled above (quality select); ESC m / ESC M pick a
    // typeface variant POM2 has only one bank for.
    case 0x6d: case 0x4d: break;                // typeface select — one font
    case 0x3d: break;                           // internal font ID
    case 0x24: break;                           // cancel MSB + MouseText
    case 0x3f: break;                           // ESC ?  ID string: no back-channel
    case 0x4f: case 0x6f: break;                // paper-out detector
    case 0x6b: break;                           // optional font
    case 0x6c: break;                           // CR before LF/FF
    case 0x40: break;                           // output bin
    case 0x2b: case 0x2e: break;                // custom char width

    case 0x4c:                                  // ESC L nnn  left margin
        spacesToZeros(3);
        // Clamped to the sheet. `ESC L 000` gave a NEGATIVE margin (the
        // -1 is the 1-based column) and clipped the first character;
        // `ESC L 999` put it 83" out and every page came out blank, with
        // no diagnostic either way. A margin off the paper is a garbled
        // parameter, not an instruction.
        leftMargin_ = std::clamp((param3() - 1.0) / cpi_,
                                 0.0, std::max(0.0, pageWidthIn_ - 1.0 / cpi_));
        if (curX_ < leftMargin_) curX_ = leftMargin_;
        break;

    case 0x4b: {                                // ESC K n  ribbon colour
        const int n = paramDigit(params_[0]);
        // A black cartridge has one band: the printer takes the command
        // and prints black anyway, like the real thing.
        if (n >= 0 && n <= 6)
            color_ = static_cast<uint8_t>(
                (ribbon_ == Ribbon::Black ? 7 : kRibbonBand[n]) << 5);
        break;
    }

    case 0x52: {                                // ESC R nnn c  repeat char
        spacesToZeros(3);
        // Left OUTSTANDING, not expanded here: `c` may be a form feed, so
        // one input byte can ask for 999 page ejects. The drain spends the
        // run one copy at a time against the mechanism budget — see
        // repeatPat_ in the header for the measurements.
        armRepeat(&params_[3], 1, static_cast<uint32_t>(param3()), false);
        break;
    }

    case 0x30: numHorizTabs_ = 0; break;        // ESC 0  clear all tabs

    case 0x28: {                                // ESC ( nnn,  set tabs
        spacesToZeros(3);
        const double stop = param3() * (1.0 / cpi_);
        if (params_[3] == ',' && numHorizTabs_ < 32) {
            horiztabs_[numHorizTabs_++] = stop;
            numParam_    = 0;
            neededParam_ = 4;                   // another stop follows
            return true;
        }
        if (numHorizTabs_ < 32) horiztabs_[numHorizTabs_++] = stop;
        break;
    }
    case 0x29: {                                // ESC ) nnn,  delete tabs
        spacesToZeros(3);
        const double stop = param3() * (1.0 / cpi_);
        for (uint8_t i = 0; i < numHorizTabs_; ++i)
            if (horiztabs_[i] == stop) horiztabs_[i] = 0.0;
        if (params_[3] == ',') {
            numParam_    = 0;
            neededParam_ = 4;
            return true;
        }
        break;
    }
    case 0x75: {                                // ESC u nnn  add one tab stop
        spacesToZeros(3);
        const double stop = param3() * (1.0 / cpi_);
        bool haveStop = false;
        int  lastEmpty = (numHorizTabs_ == 32) ? 33 : numHorizTabs_;
        for (uint8_t i = 0; i < numHorizTabs_; ++i) {
            if (horiztabs_[i] == stop)  haveStop  = true;
            if (horiztabs_[i] == 0.0)   lastEmpty = i;
        }
        if (!haveStop && lastEmpty < 33) {
            horiztabs_[lastEmpty] = stop;
            if (lastEmpty == numHorizTabs_) ++numHorizTabs_;
        }
        break;
    }

    case 0x5a:                                  // ESC Z nn  open switches
        switcha_ &= ~params_[0];
        switchb_ &= ~params_[1];
        updateSwitch();
        break;
    case 0x44:                                  // ESC D nn  close switches
        switcha_ |= params_[0];
        switchb_ |= params_[1];
        updateSwitch();
        break;

    case 0x833: {                               // US n  feed n blank lines
        const int n = paramDigit(params_[0]);
        for (int i = 0; i < n; ++i) lineFeed();
        break;
    }

    default:
        break;
    }

    escCmd_ = 0;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════
// Epson FX-80 — ESC/P (printer plan phase C3)
// ═════════════════════════════════════════════════════════════════════════
//
// A second command grammar over the SAME mechanism: the page, the dot
// plotter, the ribbon, the pacing and the paper below this layer are the ones
// the C. Itoh heads use. Only the byte grammar differs — and it differs
// enough that sharing a dispatch is impossible (`ESC G` is graphics on one
// family and double-strike on the other).
//
// Reference: Epson FX-80 User's Manual, Appendix A (command summary),
// cross-checked against mikedaley/web-a2e's `src/js/printer/epson-fx80.js`.
//
// WHAT IS NOT IMPLEMENTED, deliberately and visibly: user-defined characters
// (ESC &, ESC %, ESC :), the vertical forms unit (ESC b, ESC /, ESC B),
// nine-pin graphics (ESC ^), horizontal tab lists (ESC D), margins (ESC l,
// ESC Q) and graphics-letter reassignment (ESC ?). Each is CONSUMED with its
// parameters where the length is knowable, so a stray parameter never prints
// as text — the failure mode that makes a partial ESC/P parser look like
// gibberish rather than like a missing feature.

namespace {
/// ESC * density modes → horizontal dots per inch (FX-80 App. A, Table A-2).
/// Modes 5-7 are the FX's "plotter" densities; the FX-80 prints 4 and 6 at
/// the same dot pitch as 0 and 1 but at half speed, which POM2 does not model
/// separately because it changes no ink.
int epsonStarDpi(uint8_t mode)
{
    switch (mode) {
    case 0: return 60;
    case 1: return 120;
    case 2: return 120;    // double-speed: same pitch, adjacent dots skipped
    case 3: return 240;
    case 4: return 80;
    case 5: return 72;
    case 6: return 90;
    case 7: return 144;
    default: return 60;
    }
}
} // namespace

void ImageWriter::setupEpsonBitImage(int dotsPerInch, uint32_t columns)
{
    bitGraph_.horizDens       = static_cast<uint16_t>(dotsPerInch);
    bitGraph_.vertDens        = 72;          // 8 of the 9 wires, 1/72 in apart
    bitGraph_.adjacent        = true;
    bitGraph_.bytesColumn     = 1;
    bitGraph_.remBytes        = columns;
    bitGraph_.readBytesColumn = 0;
    bitGraph_.msbTop          = true;        // ESC/P: bit 7 is the TOP dot
    bitGraph_.swallow         = false;
}

bool ImageWriter::processEpsonChar(uint8_t ch)
{
    // "The previous byte was a CR we line-fed for" — only an LF landing
    // immediately after one counts as the guest supplying its own feed.
    //
    // processCommandChar() keeps the same latch, but it does so AFTER routing
    // here (:1271 vs :1275), so this head has to maintain its own copy. While
    // it did not, every CR+LF stream fed the platen twice per line — bands of
    // a screen dump landed 2/9" apart instead of the 1/9" they were computed
    // to abut at — and Auto mode could never latch off.
    const bool wasCrFed = crJustFed_;
    crJustFed_ = false;

    // ── Parameter collection ────────────────────────────────────────────
    if (epsonNeed_ > 0) {
        epsonParams_[epsonCount_++] = ch;
        if (--epsonNeed_ > 0) return true;
        execEpsonEscape();
        return true;
    }

    // ── ESC selects the command ─────────────────────────────────────────
    if (escSeen_) {
        escSeen_  = false;
        escCmd_   = ch;
        epsonCount_ = 0;

        switch (ch) {
        // No parameters.
        case 0x40: resetPrinter();                     return true;  // ESC @
        case 0x45: style_ |=  kStyleBold;              return true;  // ESC E
        case 0x46: style_ &= ~kStyleBold;              return true;  // ESC F
        case 0x47: style_ |=  kStyleDoubleStrike;      return true;  // ESC G
        case 0x48: style_ &= ~kStyleDoubleStrike;      return true;  // ESC H
        // Italics and the scripts arrived with Graftrax; on a bare MX-80
        // these fall through to `default`, i.e. dropped with their ESC.
        case 0x34: if (!modelHasEscP(kEscPItalics)) break;             // ESC 4
                   style_ |=  kStyleItalics;           return true;
        case 0x35: if (!modelHasEscP(kEscPItalics)) break;             // ESC 5
                   style_ &= ~kStyleItalics;           return true;
        case 0x54: if (!modelHasEscP(kEscPScripts)) break;             // ESC T
                   style_ &= ~(kStyleSuperscript |
                               kStyleSubscript);       return true;
        case 0x30: lineSpacing_ = 1.0 / 8.0;           return true;  // ESC 0
        case 0x31: lineSpacing_ = 7.0 / 72.0;          return true;  // ESC 1
        case 0x32: lineSpacing_ = 1.0 / 6.0;           return true;  // ESC 2
        case 0x4D: cpi_ = 12.0; printRes_ = 2; definedUnit_ = 96;     // ESC M
                   style_ &= ~kStyleCondensed; updateMetrics(); return true;
        case 0x50: cpi_ = 10.0; printRes_ = 1; definedUnit_ = 80;     // ESC P
                   style_ &= ~kStyleCondensed; updateMetrics(); return true;
        case 0x0F: style_ |=  kStyleCondensed; updateMetrics(); return true; // ESC SI
        case 0x0E: style_ |=  kStyleDoubleWidth; updateMetrics(); return true; // ESC SO
        case 0x4F: if (!modelHasEscP(kEscPSkipPerf)) break;            // ESC O
                   topMargin_ = 0.0; bottomMargin_ = pageHeightIn_;
                   return true;

        // One parameter.
        case 0x21:   // ESC ! n  master select
        case 0x2D:   // ESC - n  underline
        case 0x33:   // ESC 3 n  n/216 in line spacing
        case 0x41:   // ESC A n  n/72 in line spacing
        case 0x43:   // ESC C n  form length in lines (n = 0 → inches follow)
        case 0x4A:   // ESC J n  immediate n/216 in feed
        case 0x4E:   // ESC N n  skip-over-perforation
        case 0x52:   // ESC R n  international charset
        case 0x53:   // ESC S n  super/subscript
        case 0x57:   // ESC W n  expanded
        case 0x6A:   // ESC j n  reverse feed n/216 in
        case 0x49: case 0x55: case 0x69: case 0x73:   // consumed, no effect
            epsonNeed_ = 1;
            return true;

        // Graphics: two count bytes then that many column bytes.
        case 0x4B: case 0x4C: case 0x59: case 0x5A:
            epsonNeed_ = 2;
            return true;

        // ESC * m n1 n2 — density byte then the count.
        case 0x2A:
            epsonNeed_ = 3;
            return true;

        default:
            break;
        }
        // Unknown ESC — or one this head has no hardware for, which the
        // firmware cannot tell apart from unknown. Dropped with its ESC, as
        // the manual says; any following bytes print as text, which is what
        // real iron does and what a driver aimed at the wrong head deserves
        // to look like.
        return true;
    }

    // ── Control characters ──────────────────────────────────────────────
    switch (ch) {
    case 0x1B: escSeen_ = true;                       return true;   // ESC
    case 0x0D:                                                        // CR
        curX_ = leftMargin_;
        if (!autoFeedActive()) return true;
        lineFeed();
        crJustFed_ = true;      // an LF right after this one is the guest's
        return true;            // own — see the LF case
    case 0x0A:                                                        // LF
        if (wasCrFed) {
            // CR+LF from the guest: it manages its own line feeds, so the
            // feed the CR just did was ours to give and this LF would
            // double-space. Swallow it — and in Auto mode stop feeding on CR
            // at all from here on. Same rule as the C. Itoh head at :1411.
            if (feedMode_ == AutoFeed::Auto && !feedLatchedOff_) {
                feedLatchedOff_ = true;
                if (trace_)
                    traceEvent("auto line-feed OFF — the guest sent its "
                               "own LF after a CR");
            }
            return true;
        }
        lineFeed();
        return true;
    case 0x0C: formFeed();                            return true;   // FF
    case 0x08: curX_ = std::max(leftMargin_, curX_ - 1.0 / actcpi_);  // BS
               return true;
    case 0x0E: style_ |= kStyleDoubleWidth; updateMetrics(); return true; // SO
    case 0x14: style_ &= ~kStyleDoubleWidth; updateMetrics(); return true; // DC4
    case 0x0F: style_ |= kStyleCondensed;  updateMetrics(); return true;  // SI
    case 0x12: style_ &= ~kStyleCondensed; updateMetrics(); return true;  // DC2
    case 0x07: case 0x11: case 0x13: case 0x18: case 0x7F:
        return true;                                   // BEL/DC1/DC3/CAN/DEL
    default:
        return false;                                  // printable
    }
}

void ImageWriter::execEpsonEscape()
{
    const uint8_t p0 = epsonParams_[0];

    // A head that lacks the command still had to COLLECT its parameters —
    // they were already consumed by the time we know — so the gate is here,
    // after collection, rather than in the arming switch. The bytes vanish
    // instead of printing, which is the one place this diverges from real
    // iron and does so on purpose: an MX-80 fed `ESC ! 0x08` printed "!" and
    // a backspace-ish control, and reproducing THAT is noise, not diagnosis.
    static constexpr struct { uint8_t cmd; uint32_t feature; } kEscPGates[] = {
        { 0x21, kEscPMasterSelect },   // ESC !
        { 0x53, kEscPScripts     },    // ESC S
        { 0x4E, kEscPSkipPerf    },    // ESC N
        { 0x4C, kEscPGraphicsLYZ },    // ESC L
        { 0x59, kEscPGraphicsLYZ },    // ESC Y
        { 0x5A, kEscPGraphicsLYZ },    // ESC Z
        { 0x2A, kEscPGraphicsStar },   // ESC *
    };
    for (const auto& g : kEscPGates) {
        if (g.cmd != escCmd_) continue;
        if (modelHasEscP(g.feature)) break;
        // Graphics is the case that matters: dropping the command while its
        // DATA bytes still stream would print a screenful of them. The count
        // has already been parsed, so consume the body and print nothing.
        if (escCmd_ == 0x4C || escCmd_ == 0x59 || escCmd_ == 0x5A) {
            const uint32_t cols = static_cast<uint32_t>(epsonParams_[0]) |
                                  (static_cast<uint32_t>(epsonParams_[1]) << 8);
            setupEpsonBitImage(60, cols);
            bitGraph_.swallow = true;
        } else if (escCmd_ == 0x2A) {
            const uint32_t cols = static_cast<uint32_t>(epsonParams_[1]) |
                                  (static_cast<uint32_t>(epsonParams_[2]) << 8);
            setupEpsonBitImage(60, cols);
            bitGraph_.swallow = true;
        }
        epsonCount_ = 0;
        escCmd_     = 0;
        return;
    }

    switch (escCmd_) {
    case 0x21: {                                   // ESC ! n  master select
        // A bitmask that sets several styles at once (App. A): bit 0 elite,
        // bit 2 condensed, bit 3 emphasized, bit 4 double-strike, bit 5
        // double width, bit 7 underline.
        style_ &= ~(kStyleCondensed | kStyleBold | kStyleDoubleStrike |
                    kStyleDoubleWidth | kStyleUnderline);
        cpi_ = (p0 & 0x01) ? 12.0 : 10.0;
        printRes_    = (p0 & 0x01) ? 2 : 1;
        definedUnit_ = (p0 & 0x01) ? 96 : 80;
        if (p0 & 0x04) style_ |= kStyleCondensed;
        if (p0 & 0x08) style_ |= kStyleBold;
        if (p0 & 0x10) style_ |= kStyleDoubleStrike;
        if (p0 & 0x20) style_ |= kStyleDoubleWidth;
        if (p0 & 0x80) style_ |= kStyleUnderline;
        updateMetrics();
        break;
    }
    case 0x2D:                                     // ESC - n  underline
        if (p0 & 0x01) style_ |=  kStyleUnderline;
        else           style_ &= ~kStyleUnderline;
        break;
    case 0x33:                                     // ESC 3 n  n/216 in
        lineSpacing_ = static_cast<double>(p0) / 216.0;
        break;
    case 0x41:                                     // ESC A n  n/72 in
        lineSpacing_ = static_cast<double>(std::min<uint8_t>(p0, 85)) / 72.0;
        break;
    case 0x4A:                                     // ESC J n  immediate feed
        curY_ += static_cast<double>(p0) / 216.0;
        // Same bottom-of-page rule as lineFeed()/VT: without it, a
        // graphics job pacing itself with `ESC J 24` between bands (the
        // standard ESC/P idiom — it leaves line spacing alone) walked
        // curY_ past the bottom margin and fillDots then clipped every
        // following band silently until the next CR/LF-driven eject.
        if (curY_ > bottomMargin_ - lineSpacing_) newPage(true, false);
        break;
    case 0x6A:                                     // ESC j n  reverse feed
        curY_ = std::max(topMargin_, curY_ - static_cast<double>(p0) / 216.0);
        break;
    case 0x43: {                                   // ESC C n  form length
        // n = 0 means "the NEXT byte is a length in inches" — a second
        // parameter, so re-arm rather than committing a zero-length page,
        // which would eject on every line feed.
        if (p0 == 0 && epsonCount_ == 1) { epsonNeed_ = 1; return; }
        const double lines = (epsonCount_ >= 2)
                                 ? static_cast<double>(epsonParams_[1]) / lineSpacing_
                                 : static_cast<double>(p0);
        const double len = (epsonCount_ >= 2)
                               ? static_cast<double>(epsonParams_[1])
                               : lines * lineSpacing_;
        // Clamped to the PHYSICAL sheet, not to kMaxPaperLengthIn — same rule
        // and same reason as the C. Itoh `ESC H` at :1610. A form longer than
        // the mounted paper has everything past the raster dropped silently by
        // fillDots and its eject deferred to the guest's form length, so a
        // 14"-fanfold driver on a Letter tray lost ~3" of every page.
        pageHeightIn_ = std::clamp(len, 1.0, defaultPageHeight_);
        bottomMargin_ = pageHeightIn_;
        topMargin_    = 0.0;
        break;
    }
    case 0x4E: {                                   // ESC N n  skip perforation
        // The FX-80 manual restricts n to 1..127 AND to less than the form
        // length; real firmware ignores anything else. Honour that: an n at or
        // past the form length collapses the bottom margin to zero, and
        // lineFeed() then ejects a blank sheet on EVERY feed — the same
        // failure `ESC H 0000` was guarded against at :1604.
        const double skip = static_cast<double>(p0) * lineSpacing_;
        if (p0 == 0 || skip >= pageHeightIn_) break;
        bottomMargin_ = pageHeightIn_ - skip;
        break;
    }
    case 0x52: {                                   // ESC R n  charset
        // The FX-80's set numbering is its own; map the ones POM2 has a
        // charset row for and leave the rest at USA.
        static constexpr uint8_t kEpsonToSwitchA[13] = {
            0, 6, 4, 3, 2, 5, 1, 7, 0, 0, 0, 0, 0
        };
        const uint8_t idx = (p0 < 13) ? kEpsonToSwitchA[p0] : 0;
        switcha_ = static_cast<uint8_t>((switcha_ & ~kSwitchACharsetMask) | idx);
        updateSwitch();
        break;
    }
    case 0x53:                                     // ESC S n  super/subscript
        style_ &= ~(kStyleSuperscript | kStyleSubscript);
        style_ |= (p0 & 0x01) ? kStyleSubscript : kStyleSuperscript;
        break;
    case 0x57:                                     // ESC W n  expanded
        if (p0 & 0x01) style_ |=  kStyleDoubleWidth;
        else           style_ &= ~kStyleDoubleWidth;
        updateMetrics();
        break;

    // Graphics. `ESC K/L/Y/Z n1 n2` and `ESC * m n1 n2` differ only in where
    // the density comes from; the count is TWO BINARY BYTES, low first —
    // not the four ASCII digits the C. Itoh family uses.
    case 0x4B: case 0x4C: case 0x59: case 0x5A: {
        const uint32_t cols = static_cast<uint32_t>(epsonParams_[0]) |
                              (static_cast<uint32_t>(epsonParams_[1]) << 8);
        const int dpi = (escCmd_ == 0x4B) ? 60
                      : (escCmd_ == 0x5A) ? 240 : 120;
        setupEpsonBitImage(dpi, cols);
        break;
    }
    case 0x2A: {                                   // ESC * m n1 n2
        const uint32_t cols = static_cast<uint32_t>(epsonParams_[1]) |
                              (static_cast<uint32_t>(epsonParams_[2]) << 8);
        setupEpsonBitImage(epsonStarDpi(epsonParams_[0]), cols);
        break;
    }

    default:
        break;                                     // consumed, no effect
    }

    epsonCount_ = 0;
    escCmd_     = 0;
}

// ─────────────────────────────────────────────────────────────────────────
// Palette
// ─────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────
// Diablo 630 — the LaserWriter's daisywheel emulation mode
// ─────────────────────────────────────────────────────────────────────────
//
// A THIRD grammar over the same mechanism, on the precedent the FX-80 set:
// the page, dot plotter, margins, paper, pacing, PDF export and print history
// below the command layer are the ones every other head uses. What is
// genuinely different is the model of motion. A daisywheel has no pitch and no
// line spacing — it has an HMI and a VMI, indices in 1/120 and 1/48 inch that
// the driver sets explicitly — and it has no graphics commands whatsoever.
//
// This is what made the LaserWriter reachable from an Apple II at all without
// PostScript: the back-panel switch offered Diablo 630 emulation so software
// with a 630 driver (which was most word processors, the 630 being the
// daisywheel of the era) could print text over the serial port. POM2 receives
// exactly that stream — through the Super Serial Card's printer tap, which is
// the transport a real LaserWriter hung off.
//
// DELIBERATELY CONSERVATIVE. Only the commands whose encoding is unambiguous
// are decoded; everything else falls through to "unknown ESC, dropped with its
// ESC", which is what the firmware itself did. That is the safer error on this
// grammar: guessing a parameter COUNT wrong does not lose a command, it prints
// the parameter as a character and desynchronises the rest of the job. The
// LaserWriter's emulation was a subset of the real 630 anyway, so a
// conservative subset is closer to the machine than an eager one.
//
// Not decoded, and why: ESC FF n (form length) and ESC L (bottom margin) —
// their parameter counts differ between the 630 and its clones, and a wrong
// guess desynchronises. ESC 3 / ESC 4 (HyPlot graphics) — the LaserWriter had
// no such hardware.

bool ImageWriter::processDiabloChar(uint8_t ch)
{
    // The effective advance for one character cell: the guest's HMI when it
    // set one, the pitch otherwise. Backspace and tabs both move by it.
    const double cell = (hmi_ > 0.0) ? hmi_ : 1.0 / actcpi_;

    const bool wasCrFed = crJustFed_;
    crJustFed_ = false;

    if (diabloNeed_ > 0) {
        diabloNeed_ = 0;
        params_[0]  = ch;
        execDiabloEscape();
        return true;
    }

    if (escSeen_) {
        escSeen_   = false;
        diabloCmd_ = ch;

        switch (ch) {
        // ── Margins and tabs, all set at the CURRENT position ───────────
        case '9': leftMargin_   = curX_;                     return true;
        case '0': rightMargin_  = curX_;                     return true;
        case 'T': topMargin_    = curY_;                     return true;
        case 'C': topMargin_    = 0.0;                                  // clear
                  bottomMargin_ = pageHeightIn_;             return true;
        case '1': // Set a tab here. Kept sorted so the HT scan is a
                  // straight walk, and deduplicated so setting the same
                  // stop twice does not make HT stall on it.
                  if (std::find_if(diabloTabs_.begin(), diabloTabs_.end(),
                          [&](double t) { return std::fabs(t - curX_) < 1e-9; })
                      == diabloTabs_.end()) {
                      diabloTabs_.push_back(curX_);
                      std::sort(diabloTabs_.begin(), diabloTabs_.end());
                  }
                  return true;
        case '8': // Clear the tab at the current position, if there is one.
                  diabloTabs_.erase(
                      std::remove_if(diabloTabs_.begin(), diabloTabs_.end(),
                          [&](double t) { return std::fabs(t - curX_) < 1e-9; }),
                      diabloTabs_.end());
                  return true;
        case '2': diabloTabs_.clear();                       return true;

        // ── Styles ─────────────────────────────────────────────────────
        case 'E': style_ |=  kStyleUnderline;                return true;
        case 'R': style_ &= ~kStyleUnderline;                return true;
        case 'O': style_ |=  kStyleBold;                     return true;
        case '&': style_ &= ~kStyleBold;                     return true;
        // Proportional spacing on / off. The unit matters as much as the
        // style bit: a proportional glyph advances by its OWN escapement
        // (glyphAdvance), and that escapement is quoted in the dot unit the
        // pitch command established. The 630 emulation powers up at the
        // profile's fixed pitch (1/80"), and dividing the ROM's escapements
        // by that gave every glyph ~1.8× its width — about 5 cpi. The bank
        // it draws from is the C. Itoh proportional face, whose widths are
        // the `ESC p` unit — 1/144", the 10 cpi proportional pitch, which is
        // also the pitch this head comes up in. ESC Q hands the unit back to
        // the profile with the style.
        case 'P': style_ |=  kStyleProp;  definedUnit_ = 144;
                  updateMetrics();                           return true;
        case 'Q': style_ &= ~kStyleProp;
                  definedUnit_ = modelProfile().defaultUnit;
                  updateMetrics();                           return true;

        // ── Fractional and reverse motion ──────────────────────────────
        case 'U':                                        // half line feed
            curY_ += lineSpacing_ / 2.0;
            if (curY_ > bottomMargin_ - lineSpacing_) newPage(true, false);
            return true;
        case 'D':                                        // reverse half feed
            curY_ = std::max(topMargin_, curY_ - lineSpacing_ / 2.0);
            return true;
        case 0x0A:                                       // ESC LF: reverse
            curY_ = std::max(topMargin_, curY_ - lineSpacing_);
            return true;

        // ── One parameter ──────────────────────────────────────────────
        case 0x1E:   // ESC RS n — HMI, (n-1)/120 in
        case 0x1F:   // ESC US n — VMI, (n-1)/48 in
        case 0x09:   // ESC HT n — absolute horizontal tab, in columns
        case 0x0B:   // ESC VT n — absolute vertical tab, in lines
            diabloNeed_ = 1;
            return true;

        default:
            // Unknown, or a 630 command this emulation never carried.
            // Dropped with its ESC; anything after it prints, which is what
            // the firmware did.
            return true;
        }
    }

    switch (ch) {
    case 0x1B: escSeen_ = true;                              return true;
    case 0x08:                                               // BS
        // The 630 had no bold wheel: a driver printed the character, backed
        // up one HMI and printed it again. That lands here, and the page
        // model ORs ink, so the overstrike emerges for free.
        curX_ = std::max(leftMargin_, curX_ - cell);
        return true;
    case 0x09: {                                             // HT
        for (double t : diabloTabs_) {
            if (t > curX_ + 1e-9) { curX_ = std::min(t, rightMargin_); return true; }
        }
        // Past the last stop the carriage just takes one cell, rather than
        // jumping to the right margin and wrapping every tab.
        curX_ = std::min(curX_ + cell, rightMargin_);
        return true;
    }
    case 0x0D:                                               // CR
        curX_ = leftMargin_;
        if (!autoFeedActive()) return true;
        lineFeed();
        crJustFed_ = true;
        return true;
    case 0x0A:                                               // LF
        if (wasCrFed) return true;      // the guest feeds its own lines
        lineFeed();
        return true;
    case 0x0C:                                               // FF
        formFeed();
        return true;
    case 0x00: case 0x7F:
        return true;                    // NUL / DEL: never printed
    default:
        break;
    }
    return false;                       // printable — the caller renders it
}

void ImageWriter::execDiabloEscape()
{
    const uint8_t p0 = params_[0];

    switch (diabloCmd_) {
    case 0x1E:
        // HMI = (n-1)/120 in. n = 1 means zero motion, which a driver uses to
        // stack accents on one cell; guard it anyway so a zero can never make
        // a line loop forever against the right-margin wrap.
        hmi_ = (p0 > 1) ? static_cast<double>(p0 - 1) / 120.0 : 0.0;
        if (hmi_ <= 0.0) hmi_ = -1.0;   // -1 = "no override", back to pitch
        break;
    case 0x1F:
        // VMI = (n-1)/48 in. Same guard: a zero VMI would print every line
        // on top of the last one and never reach the bottom margin.
        if (p0 > 1) lineSpacing_ = static_cast<double>(p0 - 1) / 48.0;
        break;
    case 0x09: {
        // Absolute horizontal tab, counted in columns from the left margin.
        const double cell = (hmi_ > 0.0) ? hmi_ : 1.0 / actcpi_;
        curX_ = std::min(leftMargin_ + static_cast<double>(p0) * cell,
                         rightMargin_);
        break;
    }
    case 0x0B: {
        // Absolute vertical tab, counted in lines from the top margin.
        const double y = topMargin_ + static_cast<double>(p0) * lineSpacing_;
        curY_ = std::min(y, bottomMargin_);
        break;
    }
    default:
        break;
    }
    diabloCmd_ = 0;
}

bool ImageWriter::adoptRenderedPage(const uint8_t* gray, int w, int h)
{
    if (!gray || w <= 0 || h <= 0 || current_.pix.empty()) return false;

    // Band 7 is all three inks, i.e. neutral black, and the intensity ramp is
    // 0..31 (see indexToRgb). So a grey maps straight onto the page's own
    // encoding and anti-aliased PostScript text keeps its edges — the page
    // model was never one-bit, it just had no source of greys before.
    const int cw = std::min(w, current_.w);
    const int chh = std::min(h, current_.h);
    for (int y = 0; y < chh; ++y) {
        const uint8_t* src = gray + static_cast<size_t>(y) * w;
        uint8_t* dst = current_.pix.data() + static_cast<size_t>(y) * current_.w;
        for (int x = 0; x < cw; ++x) {
            const unsigned ink = (255u - src[x]) * 31u / 255u;
            dst[x] = static_cast<uint8_t>((7u << 5) | ink);
        }
    }
    // The head has no meaningful position on a page it did not print, and
    // leaving it mid-sheet would make the next text job start there.
    curX_ = leftMargin_;
    curY_ = topMargin_;
    ++revision_;
    return true;
}

void ImageWriter::indexToRgb(uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b)
{
    // FillPalette (imagewriter.cpp:101-114) in closed form: each ribbon
    // band names the channels the ink *subtracts*, and 30.9 is the
    // reference's divisor for the 0..31 intensity ramp.
    const uint8_t band = static_cast<uint8_t>(v >> 5);
    const float   i    = static_cast<float>(v & 0x1F);

    // band bit 0 = magenta ink (kills green), bit 1 = cyan (kills red),
    // bit 2 = yellow (kills blue).
    const float redMax   = (band & 2) ? 255.0f : 0.0f;
    const float greenMax = (band & 1) ? 255.0f : 0.0f;
    const float blueMax  = (band & 4) ? 255.0f : 0.0f;

    auto ch = [&](float maxv) {
        const float f = 255.0f - (maxv / 30.9f) * i;
        return static_cast<uint8_t>(f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
    };
    r = ch(redMax);
    g = ch(greenMax);
    b = ch(blueMax);
}

void ImageWriter::pageToRgba(const Page& p, std::vector<uint8_t>& out)
{
    out.resize(static_cast<size_t>(p.w) * p.h * 4);
    // 256-entry LUT — the page is megapixels, the palette is not.
    uint8_t lut[256][3];
    for (int i = 0; i < 256; ++i)
        indexToRgb(static_cast<uint8_t>(i), lut[i][0], lut[i][1], lut[i][2]);

    for (size_t i = 0, n = p.pix.size(); i < n; ++i) {
        const uint8_t* c = lut[p.pix[i]];
        out[i * 4 + 0] = c[0];
        out[i * 4 + 1] = c[1];
        out[i * 4 + 2] = c[2];
        out[i * 4 + 3] = 255;
    }
}

// ─────────────────────────────────────────────────────────────────────────

ImageWriter::Status ImageWriter::status() const
{
    Status s;
    s.headX       = curX_;
    s.headY       = curY_;
    s.cpi         = actcpi_;
    s.lineSpacing = lineSpacing_;
    s.colorName   = bandName(static_cast<uint8_t>(color_ >> 5));
    s.graphicsDpi = kBitImageHorizDpi[printRes_ & 15];
    s.inGraphics  = bitGraph_.remBytes > 0;

    struct { uint16_t bit; const char* name; } kNames[] = {
        { kStyleProp,        "proportional" },
        { kStyleCondensed,   "condensed"    },
        { kStyleBold,        "bold"         },
        { kStyleDoubleWidth, "double-width" },
        { kStyleItalics,     "italic"       },
        { kStyleUnderline,   "underline"    },
        { kStyleSuperscript, "superscript"  },
        { kStyleSubscript,   "subscript"    },
        { kStyleHalfHeight,  "half-height"  },
    };
    for (const auto& n : kNames) {
        if (!(style_ & n.bit)) continue;
        if (!s.styleText.empty()) s.styleText += ' ';
        s.styleText += n.name;
    }
    if (s.styleText.empty()) s.styleText = "normal";
    return s;
}

} // namespace pom2
