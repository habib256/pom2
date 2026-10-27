// POM2 Apple II Emulator — GPL-3.0-or-later
//
// ImageWriter_EscP — the Epson ESC/P head (FX-80, MX-80, RX-80): its byte
// grammar over the shared ImageWriter mechanism. Split out of ImageWriter.cpp
// (at its size budget) when the 2026-10-27 hunt brought the control codes,
// tab stops and parameterised commands up to the FX manuals.

#include "ImageWriter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pom2 {

namespace {
constexpr uint8_t kSwitchACharsetMask = 0x07;   // as ImageWriter.cpp
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
    if (epsonSkip_ > 0) { --epsonSkip_; return true; }       // ESC ^ / ESC & data
    if (epsonListCmd_)  { epsonListByte(ch); return true; }  // ESC D / B / b list

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
        case 0x0E: epsonLineWide_ = true; style_ |= kStyleDoubleWidth; updateMetrics(); return true; // ESC SO
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
        // ESC l n (left margin) and ESC Q n (right margin) are the two the
        // header, DEV.md and printer_plan.md all claimed were "CONSUMED with
        // their parameters" and were not: they fell through to `default`,
        // which drops the ESC and the letter but NOT the count byte, so the
        // margin column number printed as a glyph — `ESC l 5` put a stray
        // ENQ-turned-space and `ESC Q 70` an 'F' into the text. Consuming
        // them is the whole point of listing an unimplemented command.
        case 0x51:   // ESC Q n  right margin, column n
        case 0x6C:   // ESC l n  left margin, column n
        // ESC p n — proportional spacing. It had NO case at all: the ESC and
        // the 'p' were dropped by `default` and `n` fell through to the text
        // path, so `ESC p CHR$(1)` printed a stray glyph and proportional
        // never engaged. `kEscPProportional` — the whole difference between
        // the FX-80 profile and the RX-80's `kEscPFX80 & ~kEscPProportional`
        // — was dead data with nothing reading it (bug hunt #10).
        case 0x70:   // ESC p n  proportional spacing
        case 0x49: case 0x55: case 0x69: case 0x73:   // consumed, no effect
            epsonNeed_ = 1;
            return true;

        // Graphics: two count bytes then that many column bytes.
        case 0x4B: case 0x4C: case 0x59: case 0x5A:
            epsonNeed_ = 2;
            return true;

        // Lists ending on NUL or a non-ascending value (FX vol. 2 App. B):
        // ESC D horizontal tabs, ESC B vertical tabs, ESC b c VFU channel c.
        // They used to drop ESC and the letter and print every stop as text.
        case 0x44: case 0x42: case 0x62:
            epsonListCmd_     = ch;
            epsonListChannel_ = (ch == 0x62);
            epsonListPrev_    = -1;
            if (ch == 0x44) epsonHTabs_.assign(1, -1.0);     // marker: list started
            if (ch == 0x42) { epsonVTabs_.clear(); epsonVTabsSet_ = true; }
            return true;
        // Fixed-length commands with no effect here, CONSUMED so their bytes
        // never print: ESC / c (VFU channel), ESC ? s n (graphics letter),
        // ESC % n 0 (RAM characters), ESC $ / ESC \ (later ESC/P), ESC : 0 0 0
        // (copy ROM to RAM); ESC e m n and ESC f m n (RX-80 tabbing / skip)
        // are acted on; ESC ^ d n1 n2 (nine-pin graphics, 2n data bytes) and
        // ESC & 0 c1 c2 (user characters, 12 bytes each) swallow their body.
        case 0x2F:
            epsonNeed_ = 1;
            return true;
        case 0x3F: case 0x25: case 0x24: case 0x5C: case 0x65: case 0x66:
            epsonNeed_ = 2;
            return true;
        case 0x3A: case 0x5E: case 0x26:
            epsonNeed_ = 3;
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
        curX_ = leftMargin_; endEpsonLineWide();
        if (!autoFeedActive()) return true;
        lineFeed();
        crJustFed_ = true;      // an LF right after this one is the guest's
        return true;            // own — see the LF case
    case 0x0A: endEpsonLineWide(); curX_ = leftMargin_;               // LF (ESC/P C-27: to the margin too)
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
    case 0x0C:                                                        // FF
        // `newPage(true, …)`, NOT `formFeed()`. `formFeed()` is the FRONT
        // PANEL button and carries the panel's rule — "don't eject a sheet
        // nothing was printed on". A form feed in the DATA STREAM is paper
        // motion the guest asked for, and the C. Itoh head at :1642 ejects
        // unconditionally for exactly that reason (pinned by
        // imagewriter_smoke's `ESC R 999 <FF>` case, which asserts all 999
        // sheets come out of a page that never had ink on it). Routing this
        // head's FF through the panel entry point made the same byte mean
        // two different things depending on which model was fitted: `"A" FF
        // FF "B" FF` is three sheets on an ImageWriter II and two on an
        // FX-80, so an ESC/P job's page count silently disagreed with the
        // PDF export and the print history.
        endEpsonLineWide(); newPage(true, true);
        return true;
    case 0x08: curX_ = std::max(leftMargin_, curX_ - 1.0 / actcpi_);  // BS
               return true;
    case 0x0E: epsonLineWide_ = true; style_ |= kStyleDoubleWidth; updateMetrics(); return true; // SO
    case 0x14: endEpsonLineWide(); return true;                         // DC4
    case 0x0F: style_ |= kStyleCondensed;  updateMetrics(); return true;  // SI
    case 0x12: style_ &= ~kStyleCondensed; updateMetrics(); return true;  // DC2
    case 0x09: epsonTab();          return true;                        // HT
    case 0x0B: epsonVerticalTab();  return true;                        // VT
    case 0x07: case 0x11: case 0x13: case 0x18: case 0x7F:
        return true;                                   // BEL/DC1/DC3/CAN/DEL
    default:
        // FX vol. 2 App. B: the other codes below $20 (NUL included — it ends
        // ESC D/B lists and is otherwise ignored) are NOT printed unless
        // ESC I 1. They used to draw CP437 art and move the head.
        return ch < 0x20;
    }
}

// HT: the next stop right of the head, measured from the left margin. Power-on
// stops are every 8 Pica columns (FX-80 ch. 9); ignored when there is none, or
// it is past the right margin (ESC/P C-43).
void ImageWriter::epsonTab()
{
    const double rel = curX_ - leftMargin_ + 1e-9;
    double next = -1.0;
    if (epsonHTabs_.empty()) {
        next = (std::floor(rel / 0.8) + 1.0) * 0.8;
    } else {
        for (double t : epsonHTabs_) if (t > rel) { next = t; break; }
    }
    if (next < 0.0 || leftMargin_ + next >= rightMargin_) return;
    curX_ = leftMargin_ + next;
}

// VT: CR, then the next vertical stop from top of form; past the last, top
// of the next form. Power-on stops every 2 lines at 1/6"; after ESC B NUL
// there are none and VT is a plain CR (ESC/P C-45). Ends one-line SO.
void ImageWriter::epsonVerticalTab()
{
    endEpsonLineWide();
    curX_ = leftMargin_;
    if (epsonVTabsSet_ && epsonVTabs_.empty()) return;
    const double rel = curY_ - topMargin_ + 1e-9;
    double next = -1.0;
    if (!epsonVTabsSet_) {
        next = (std::floor(rel / (2.0 / 6.0)) + 1.0) * (2.0 / 6.0);
    } else {
        for (double t : epsonVTabs_) if (t > rel) { next = t; break; }
    }
    if (next < 0.0 || topMargin_ + next > bottomMargin_ - lineSpacing_) {
        newPage(true, true);
        return;
    }
    if (sound_) sound_->paperFeed(topMargin_ + next - curY_);
    curY_ = topMargin_ + next;
}

// One byte of an ESC D / ESC B / ESC b list. NUL, or a value not above the
// previous one, ends it (and is consumed). At most 32 horizontal and 16
// vertical stops; set in the CURRENT pitch / line spacing.
void ImageWriter::epsonListByte(uint8_t ch)
{
    if (epsonListChannel_) { epsonListChannel_ = false; return; }   // ESC b channel
    const int v = ch;
    if (v == 0 || v <= epsonListPrev_) {
        if (epsonListCmd_ == 0x44 && !epsonHTabs_.empty() && epsonHTabs_[0] < 0.0)
            epsonHTabs_.erase(epsonHTabs_.begin());
        epsonListCmd_ = 0;
        return;
    }
    epsonListPrev_ = v;
    if (epsonListCmd_ == 0x44) {
        if (!epsonHTabs_.empty() && epsonHTabs_[0] < 0.0) epsonHTabs_.clear();
        if (epsonHTabs_.size() < 32) epsonHTabs_.push_back(v / actcpi_);
    } else if (epsonListCmd_ == 0x42) {
        if (epsonVTabs_.size() < 16) epsonVTabs_.push_back(v * lineSpacing_);
    }
    // ESC b (channels 1-7) has no VT selector here; channel 0 is ESC B.
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
        { 0x70, kEscPProportional },   // ESC p — RX-80 and MX-80 have none
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
        // Bit 6 (italic) and bit 1 (proportional) are part of the mask the
        // FX-80 manual gives, and neither was set OR cleared here: `ESC ! 0`
        // — the reset every ESC/P driver sends before a run of plain text —
        // left an `ESC 4` italic switched on for the rest of the job, and
        // `ESC ! $40` never italicised anything (bug hunt #10).
        style_ &= ~(kStyleCondensed | kStyleBold | kStyleDoubleStrike |
                    kStyleDoubleWidth | kStyleUnderline | kStyleItalics |
                    kStyleProp);
        cpi_ = (p0 & 0x01) ? 12.0 : 10.0;
        printRes_    = (p0 & 0x01) ? 2 : 1;
        definedUnit_ = (p0 & 0x01) ? 96 : 80;
        if (p0 & 0x04) style_ |= kStyleCondensed;
        if (p0 & 0x08) style_ |= kStyleBold;
        if (p0 & 0x10) style_ |= kStyleDoubleStrike;
        epsonWide_ = (p0 & 0x20) != 0; if (epsonWide_ || epsonLineWide_) style_ |= kStyleDoubleWidth;
        if (p0 & 0x80) style_ |= kStyleUnderline;
        // Gated on the head actually having them, like every other ESC/P
        // capability: an RX-80 has no proportional mode to select.
        if ((p0 & 0x02) && modelHasEscP(kEscPProportional))
            style_ |= kStyleProp;
        if ((p0 & 0x40) && modelHasEscP(kEscPItalics))
            style_ |= kStyleItalics;
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
        // Only the CHARSET half of updateSwitch() belongs to this head. The
        // other half re-derives the page window from the C. Itoh perforation
        // switch B-3, which an FX-80 has no equivalent of — it spells the
        // same thing ESC N / ESC O. So selecting a character set silently
        // cancelled a skip-over-perforation the driver had already set, and
        // the form came back an inch longer for the rest of the job.
        const double keepTop = topMargin_, keepBottom = bottomMargin_;
        updateSwitch();
        topMargin_ = keepTop;
        bottomMargin_ = keepBottom;
        break;
    }
    case 0x53:                                     // ESC S n  super/subscript
        style_ &= ~(kStyleSuperscript | kStyleSubscript);
        style_ |= (p0 & 0x01) ? kStyleSubscript : kStyleSuperscript;
        break;
    case 0x57:                                     // ESC W n  expanded
        epsonWide_ = (p0 & 0x01) != 0;
        if (!epsonWide_) epsonLineWide_ = false;   // ESC W 0 also cancels SO (FX-80 p. 56)
        style_ = (epsonWide_ || epsonLineWide_) ? (style_ | kStyleDoubleWidth) : (style_ & ~kStyleDoubleWidth);
        updateMetrics();
        break;
    case 0x70:                                     // ESC p n  proportional
        // The Epson bank is monospaced, so the ADVANCE is unchanged — what
        // this buys is that the parameter byte stops printing as text and
        // that the state the status line (and any future proportional bank)
        // reads is the state the guest asked for.
        if (p0 & 0x01) style_ |=  kStyleProp;
        else           style_ &= ~kStyleProp;
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

    // Margins, in columns of the current pitch from the power-on home (the
    // head's leftmost position, kHomeX) — setting either resets the tab
    // stops to their defaults (FX-80 p. 121).
    case 0x6C:                                     // ESC l n  left margin
        leftMargin_ = std::min(kHomeX + p0 / actcpi_, rightMargin_ - 1.0 / actcpi_);
        if (curX_ < leftMargin_) curX_ = leftMargin_;
        epsonHTabs_.clear();
        break;
    case 0x51:                                     // ESC Q n  right margin
        if (p0 > 0) rightMargin_ = std::max(leftMargin_ + 1.0 / actcpi_,
                                            std::min(pageWidthIn_, kHomeX + p0 / actcpi_));
        epsonHTabs_.clear();
        break;
    case 0x66:                                     // ESC f m n  skip (RX-80)
        if (p0 == 0) {
            curX_ = std::min(rightMargin_, curX_ + epsonParams_[1] / actcpi_);
        } else {
            endEpsonLineWide();
            for (int i = 0; i < epsonParams_[1]; ++i) lineFeed();
        }
        break;
    case 0x65:                                     // ESC e m n  tab increment (RX-80)
        if (p0 == 0 && epsonParams_[1] > 0) {
            epsonHTabs_.clear();
            for (int c = epsonParams_[1]; epsonHTabs_.size() < 32; c += epsonParams_[1])
                epsonHTabs_.push_back(c / 10.0);            // Pica columns
        } else if (p0 == 1 && epsonParams_[1] > 0) {
            epsonVTabs_.clear(); epsonVTabsSet_ = true;
            for (int l = epsonParams_[1]; epsonVTabs_.size() < 16; l += epsonParams_[1])
                epsonVTabs_.push_back(l * lineSpacing_);
        }
        break;
    case 0x5E:                                     // ESC ^ d n1 n2: 2n data bytes
        epsonSkip_ = 2u * (static_cast<uint32_t>(epsonParams_[1]) |
                           (static_cast<uint32_t>(epsonParams_[2]) << 8));
        break;
    case 0x26:                                     // ESC & 0 c1 c2: 12 bytes a char
        if (epsonParams_[2] >= epsonParams_[1])
            epsonSkip_ = 12u * (epsonParams_[2] - epsonParams_[1] + 1u);
        break;

    default:
        break;                                     // consumed, no effect
    }

    epsonCount_ = 0;
    escCmd_     = 0;
}

} // namespace pom2
