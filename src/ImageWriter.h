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

// ImageWriter — host-side Apple ImageWriter II / LQ printer.
//
// This is the *printer*, not the interface card. The Apple II talks to a
// printer through a card (`PrinterCard`, `GrapplerCard`, later an SSC);
// every byte those cards spool is fed here, and this class interprets the
// ImageWriter control language and paints dots onto a page raster that the
// UI shows in a window (`ImageWriter_ImGui`) and saves as PNG.
//
// Source of truth
// ---------------
// Ported from greg-kennedy/ImageWriter (`imagewriter.cpp`, the GSport /
// KEGS / DOSBox lineage by Christopher G. Mason), itself written against
// Apple's "ImageWriter II Technical Reference Manual" (ISBN 0-201-17766-8)
// and "ImageWriter LQ Reference Manual" (ISBN 0-201-17751-X). Command
// dispatch, the soft-switch model, the density tables and the subtractive
// colour-ribbon encoding are line-for-line equivalents; the cited line
// ranges appear on each ported block below.
//
// Three deliberate deviations from the reference, all forced by POM2's
// "no new third-party dependency" rule (the reference needs SDL 1.2 +
// FreeType, neither of which POM2 links):
//
//   1. Glyphs come from the repo's own 8x8 CP437 dot-matrix font
//      (`hgrpaint::kBBFontCp437`, 7 px wide + 1 px gap) instead of a
//      FreeType-rendered TrueType face. This is *closer* to the real
//      hardware, not further: an ImageWriter draft character really is an
//      8-dot-wide cell at the pitch's graphics density, so a character and
//      a graphics column go through the same dot plotter here.
//   2. Dots are painted as the page-pixel interval they cover
//      (`fillDots`) instead of the reference's `pixsize` heuristic with its
//      "Primative scaling function" fudge (imagewriter.cpp:1556-1573).
//      Adjacent dots abut exactly at any page DPI, so graphics dumps have
//      no seams and no double-width columns.
//   3. `resetPrinter()` leaves bold OFF. The reference switches it on
//      (imagewriter.cpp:289) to thicken a thin TrueType face; on a real
//      dot-matrix cell that just smears every glyph.
//
// Proportional mode (ESC p / ESC P) selects the stated pitch but keeps the
// fixed 7-dot cell — the font has no per-glyph advance table.
//
// Page raster encoding (verbatim from the reference, imagewriter.cpp:203-208)
// -------------------------------------------------------------------------
// One byte per page pixel, `yyyxxxxx`: the low 5 bits are ink intensity
// (31 = full) and the top 3 bits are the colour-ribbon band. The bands are
// chosen so that OR-ing two inks mixes them subtractively the way overprint
// on a real four-band ribbon does:
//
//     001 magenta   010 cyan     100 yellow
//     011 blue      101 red      110 green      111 black
//
// so magenta|yellow = 101 = red, cyan|yellow = 110 = green, and all three
// = 111 = black. Index 0 is blank paper (white).
//
// Threading
// ---------
// UI-thread only. The emulator core never touches this object; the UI
// drains the interface card's spool (which *is* mutex-guarded) and feeds
// the bytes here between frames.

#ifndef POM2_IMAGEWRITER_H
#define POM2_IMAGEWRITER_H

#include "ImageWriterRom.h"   // character ROM banks (generated)
#include "PrinterSoundSink.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pom2 {

/// Which printer this is. The three share the C. Itoh 8510 command set — the
/// ImageWriter II is a backward-compatible SUPERSET that added the four-band
/// colour ribbon, the draft/NLQ font tiers, half-height, super/subscript and
/// MouseText. So the other two are the II *minus* things, which is capability
/// DATA rather than a class hierarchy: see `IwModelProfile`.
///
/// The Epson FX-80 is deliberately absent. It is a different lineage (ESC/P)
/// with its own parser, not a capability mask on this one — see
/// docs/printer_plan.md § 6.
enum class IwModel : uint8_t {
    ImageWriterII = 0,
    ImageWriterI,
    AppleDMP,
    /// Epson FX-80 — a DIFFERENT LINEAGE. It shares this class's page, dot
    /// plotter, ribbon and pacing, but not its command parser: ESC/P and the
    /// C. Itoh set collide outright (`ESC G` is graphics on one and
    /// double-strike on the other, `ESC A` is 1/6" spacing on one and n/72"
    /// on the other), so it gets its own dispatch rather than a capability
    /// mask. See `execEpsonEscape`.
    EpsonFX80,
    /// C. Itoh Prowriter 8510A — the Apple DMP without Apple's badge or its
    /// firmware restrictions, so it answers the ESC codes the DMP's ROM has
    /// no hardware for. Same core, same faces.
    Prowriter8510A,
    /// NEC PC-8023A — the same C. Itoh 8510 mechanism NEC rebadged. The
    /// Grappler+ groups it with the 8510 on one DIP position for exactly
    /// that reason (`GrapplerCard::PrinterType::CItoh8510`).
    NecPc8023A,
    /// Epson MX-80 — the ORIGINAL, pre-Graftrax. ESC/P's ancestor: single
    /// density graphics only, no italics, no master select, no scripts.
    EpsonMX80,
    /// Epson MX-80 with the Graftrax-Plus ROM upgrade — adds the other
    /// graphics densities, italics and the scripts, but still no `ESC *`.
    EpsonMX80Graftrax,
    /// Epson RX-80 — an FX-80 minus proportional spacing and user-defined
    /// characters, at a slower carriage.
    EpsonRX80,
    /// Apple LaserWriter in its DIABLO 630 emulation mode — the daisywheel
    /// command set its back-panel switch offered so unmodified software
    /// could print text without speaking PostScript. A THIRD lineage: it is
    /// neither C. Itoh nor ESC/P, it has no graphics at all, and its motion
    /// commands are indices (HMI/VMI in 1/120 and 1/48 in) rather than
    /// pitches. See `execDiabloEscape`.
    ///
    /// This is the honest in-scope LaserWriter. PostScript is not a command
    /// set POM2 can parse — it is a Turing-complete stack language with a
    /// full graphics model — so it is delegated to an external interpreter
    /// instead; see docs/printer_plan_2.md.
    LaserWriterDiablo,
    /// Apple LaserWriter speaking PostScript — the other position of the same
    /// back-panel switch. There is no parser here and there cannot be one:
    /// see PostScriptRender.h. The bytes are intercepted UPSTREAM of this
    /// class, rendered by an external interpreter, and the finished page
    /// arrives through `adoptRenderedPage`.
    ///
    /// Anything that does reach the parser prints as text, which is exactly
    /// what a PostScript job sent to a printer that cannot render it does —
    /// so a failed interception is diagnosable rather than silent.
    LaserWriterPostScript,
    Count
};

/// Which command grammar a head speaks. Three lineages now share this one
/// mechanism (page, dot plotter, margins, paper, pacing, PDF, history) and
/// disagree only above it — the same finding that kept the C. Itoh models a
/// table instead of a hierarchy, applied one level up.
enum class IwLineage : uint8_t {
    CItoh = 0,   ///< ImageWriter I/II, Apple DMP, Prowriter, NEC
    EscP,        ///< Epson MX / RX / FX
    Diablo,      ///< Diablo 630 daisywheel (LaserWriter emulation mode)
    /// Not a grammar at all — the job goes to an external interpreter. Named
    /// here so the head can be SELECTED and so nothing silently treats it as
    /// one of the three that are parsed.
    PostScript,
};

/// ESC/P heads only: what this one actually has hardware for. The ESC/P
/// lineage grew feature by feature across the MX → RX → FX generations, and
/// every one of those differences is capability DATA — the same finding that
/// made the C. Itoh side a table instead of a class hierarchy.
///
/// A command the head does NOT have is treated exactly like an unknown ESC:
/// dropped along with its ESC, and whatever follows prints as text. That is
/// what the real firmware did, and reproducing it is deliberate — POM2
/// already prints the wrong-DIP garbage a real Grappler desk would show
/// (`GrapplerCard::PrinterType`), because a driver aimed at the wrong head
/// looking wrong is the diagnosis.
enum IwEscPFeature : uint32_t {
    kEscPGraphicsLYZ  = 1u << 0,  ///< ESC L / Y / Z (120/120/240 dpi)
    kEscPGraphicsStar = 1u << 1,  ///< ESC * m — the generic density selector
    kEscPItalics      = 1u << 2,  ///< ESC 4 / ESC 5
    kEscPMasterSelect = 1u << 3,  ///< ESC ! n
    kEscPScripts      = 1u << 4,  ///< ESC S n / ESC T
    kEscPProportional = 1u << 5,  ///< ESC p n
    kEscPSkipPerf     = 1u << 6,  ///< ESC N n / ESC O
};
/// Everything the FX-80 has. `ESC K` is not a flag: every ESC/P head back to
/// the 1980 MX-80 has single-density graphics.
constexpr uint32_t kEscPFX80 =
    kEscPGraphicsLYZ | kEscPGraphicsStar | kEscPItalics | kEscPMasterSelect |
    kEscPScripts | kEscPProportional | kEscPSkipPerf;

/// One character-ROM bank: the glyph table, its locale substitutions, and the
/// cell geometry that goes with it. Draft and correspondence are 9 wires at
/// 1/72 in; NLQ is 18 rows at 1/144 in. Both make a cell 1/8 in tall, which is
/// why a line mixing qualities still sits on one baseline.
///
/// Declared here rather than in the .cpp because ImageWriter's private
/// helpers return one.
struct IwRomBank {
    const iwrom::IwGlyph*    glyphs;
    const iwrom::IwOverride* overrides;
    std::size_t              overrideCount;
    int                      rows;
    double                   rowPitch;
    bool                     proportional;
};

/// Everything that differs between the three C. Itoh heads. Adding a member
/// here is how a model gets a new difference; adding a code path is not.
struct IwModelProfile {
    const char* name;
    /// Four-band colour cartridge. II only — the I and the DMP were black.
    bool colourRibbon;
    /// Draft / NLQ tiers selectable with `ESC a`. II only; the others have a
    /// single face, so the command is swallowed and quality is pinned.
    bool qualityTiers;
    /// Character banks. `draft` / `nlqFixed` / `nlqProp` are null on a
    /// single-face model.
    const IwRomBank* stdFixed;
    const IwRomBank* stdProp;
    const IwRomBank* draft;
    const IwRomBank* nlqFixed;
    const IwRomBank* nlqProp;
    /// Power-on pitch: cpi, the graphics-density index, and the unit `ESC F`
    /// and the proportional advance are measured in. The IW-I powers up at
    /// Elite 12 cpi (DIP SW1-6 closed), not Pica.
    double  defaultCpi;
    uint8_t defaultPrintRes;
    int     defaultUnit;
    /// Carriage rate. A single-face model has one speed; `nlqCps` is ignored
    /// unless `qualityTiers`.
    double  draftCps;
    double  nlqCps;
    /// Which command grammar this head speaks. Anything but `CItoh` needs a
    /// different parser entirely — the grammars collide outright (`ESC G` is
    /// graphics on the C. Itoh family and double-strike on the Epson).
    IwLineage lineage;
    /// ESC/P capability mask (`IwEscPFeature`). Ignored unless ESC/P.
    uint32_t escPFeatures;
    /// ESC codes this head has no hardware for. They are still CONSUMED with
    /// their parameter bytes — the manual's rule is that an unrecognised code
    /// is dropped along with the ESC, and letting the parameter fall through
    /// would print it as text.
    const uint8_t* ignoredEsc;
    size_t         ignoredEscCount;
};

const IwModelProfile& iwModelProfile(IwModel m);

class ImageWriter
{
public:
    // ─── Style bits (imagewriter.h:74-83) ────────────────────────────────
    enum : uint16_t {
        kStyleProp         = 0x001,
        kStyleCondensed    = 0x002,
        kStyleBold         = 0x004,
        kStyleDoubleStrike = 0x008,
        kStyleDoubleWidth  = 0x010,
        kStyleItalics      = 0x020,
        kStyleUnderline    = 0x040,
        kStyleSuperscript  = 0x080,
        kStyleSubscript    = 0x100,
        kStyleHalfHeight   = 0x200,
    };

    /// Paper sizes offered by the GS/OS ImageWriter LQ driver, in
    /// PostScript points (iw_charmaps.h:62-77).
    enum class PaperSize : uint8_t {
        Letter = 0,     // 8.5 x 11 in
        Legal,          // 8.5 x 14 in
        A4,             // 210 x 297 mm
        B5,             // 176 x 250 mm
        WideFanfold,    // 14 x 11 in
        Ledger,         // 11 x 17 in
        A3,             // 297 x 420 mm
        Count
    };
    static const char* paperSizeName(PaperSize s);

    /// A rendered sheet. `pix` is `w*h` bytes in the `yyyxxxxx` encoding
    /// described in the file header. `dpi` is the raster density the sheet
    /// was printed at — completed sheets keep theirs even if the host
    /// changes the printer DPI afterwards, so an exporter can recover the
    /// physical page size (PDF media box) from `w/dpi` x `h/dpi` inches.
    struct Page {
        int                  w = 0;
        int                  h = 0;
        std::vector<uint8_t> pix;
        int                  dpi = 144;   // = kDefaultDpi (declared below)
    };

    /// Front-panel / print-head readout for the UI status line.
    struct Status {
        double      headX      = 0.0;   // inches from the left paper edge
        double      headY      = 0.0;   // inches from the top paper edge
        double      cpi        = 0.0;   // effective characters per inch
        double      lineSpacing= 0.0;   // inches
        const char* colorName  = "";    // current ribbon band
        std::string styleText;          // "bold underline …" ("normal" if none)
        int         graphicsDpi = 0;    // horizontal density of the active pitch
        bool        inGraphics  = false;// mid bit-image (remBytes > 0)
    };

    static constexpr int    kDefaultDpi = 144;
    static constexpr int    kMinDpi     = 72;
    static constexpr int    kMaxDpi     = 300;
    /// Completed sheets kept in RAM. A Letter page at 144 dpi is ~1.9 MB
    /// indexed, so 32 sheets is ~62 MB worst case; older sheets are
    /// dropped (counted by `droppedPageCount()`) rather than growing
    /// without bound when a guest form-feeds in a loop.
    static constexpr size_t kMaxPages   = 32;

    explicit ImageWriter(int dpi = kDefaultDpi,
                         PaperSize paper = PaperSize::Letter);

    /// Closes the trace if one is open. Without this a quit while tracing
    /// left the last partial hex row stranded in `traceRow_` (it never
    /// reached stdio, so the C runtime's exit flush could not save it) and
    /// the file ended with no `# trace closed` footer — you could not tell
    /// a complete trace from one cut short by a crash. That matters most
    /// on the `POM2_TRACE_PRINTER=1` path, which is the one used to
    /// capture a trace for a bug report.
    ~ImageWriter();

    /// Owns a `FILE*`; copying it would `fclose` the same handle twice.
    ImageWriter(const ImageWriter&)            = delete;
    ImageWriter& operator=(const ImageWriter&) = delete;

    // ─── Configuration (host side — not driven by the guest) ─────────────
    int       dpi()       const { return dpi_; }
    PaperSize paperSize() const { return paper_; }
    /// Both rebuild the page raster, so the sheet in progress is lost.
    /// Completed sheets are kept (they carry their own size).
    void setDpi(int dpi);
    void setPaperSize(PaperSize s);

    /// Which head this is. Changing it is a power cycle: the ribbon, the
    /// pitch and the available faces all change, so nothing on the platen
    /// would still mean what it did.
    void    setModel(IwModel m);
    IwModel model() const { return model_; }
    const IwModelProfile& modelProfile() const { return iwModelProfile(model_); }
    static const char* modelName(IwModel m) { return iwModelProfile(m).name; }

    // ── Custom paper, power and the front panel (printer plan phase D) ────

    /// Tractor-feed paper the ImageWriter II actually accepts: 4.0"-10.0"
    /// overall, and a form length `ESC H` can set anywhere from 1" to ~69"
    /// in 1/144" steps (Tech Ref Table 5-3). Sizes are committed in QUARTER
    /// INCH increments, which is how the tractor is adjusted.
    static constexpr double kMinPaperWidthIn  = 4.0;
    static constexpr double kMaxPaperWidthIn  = 10.0;
    static constexpr double kMinPaperLengthIn = 1.0;
    static constexpr double kMaxPaperLengthIn = 69.0;

    /// Set an arbitrary sheet size. Values are snapped to 1/4" and clamped to
    /// the ranges above; the COMMITTED values are written back through the
    /// out-parameters, because a caller that asked for something impossible
    /// needs to know what it actually got rather than silently disagreeing
    /// with the printer.
    void setPaperDimensions(double widthIn, double lengthIn,
                            double* committedWidth = nullptr,
                            double* committedLength = nullptr);
    double paperWidthIn()  const { return defaultPageWidth_; }
    double paperLengthIn() const { return defaultPageHeight_; }

    /// Put an EXTERNALLY rendered page onto the platen — the LaserWriter's
    /// PostScript path, where the page comes back from an interpreter rather
    /// than from a head striking paper (see PostScriptRender.h).
    ///
    /// `gray` is one byte per pixel, 0 = full ink and 255 = bare paper,
    /// `w` bytes per row (Ghostscript's `pgmraw` convention). It is blitted
    /// 1:1 and clipped, NOT scaled — ask the interpreter for the page's own
    /// pixel geometry (`pageRasterWidth/Height`) and the two agree exactly.
    ///
    /// Does NOT eject. The caller decides, because "the interpreter produced
    /// a page" and "the sheet is finished" are different events on a printer
    /// whose job may carry several `showpage`s.
    bool adoptRenderedPage(const uint8_t* gray, int w, int h);

    /// The current sheet's raster geometry, i.e. what to ask a renderer for.
    int pageRasterWidth()  const { return current_.w; }
    int pageRasterHeight() const { return current_.h; }

    /// Mechanical sound sink (head buzz, carriage sweep, platen motor).
    /// Optional; null = a silent printer, which is what every headless build
    /// and test gets. See PrinterSoundSink.h for why these events carry no
    /// emuCycles stamp, unlike the floppy's.
    void setSoundSink(PrinterSoundSink* s) { sound_ = s; }

    /// Front-panel POWER. Off ignores every incoming byte — and KEEPS THE
    /// PAPER, which is what distinguishes it from `powerCycle()`: pulling the
    /// plug does not eject or erase what is already on the platen.
    void setPowered(bool on);
    bool powered() const { return powered_; }

    /// Front-panel SELECT. Offline stops accepting data (the printer is still
    /// on, the software just cannot reach it), which is what "deselected"
    /// means to a driver and the usual reason a real one appears to hang.
    void setOnline(bool on) { online_ = on; }
    bool online() const { return online_; }

    /// The ImageWriter's "line feed after carriage return" DIP switch
    /// (SW A-8) — the setting that decides whether a printout comes out
    /// right, double-spaced, or overprinted onto one line, and the one no
    /// user should have to guess.
    ///
    ///   * A bare Apple II `PR#n : PRINT` emits CR and never LF, so the
    ///     printer has to supply the feed.
    ///   * Real drivers (and the Grappler+ firmware) send CR **and** LF,
    ///     so the printer must NOT add one, or everything double-spaces.
    ///   * Colour drivers go further: Print Shop separates its yellow /
    ///     cyan / magenta passes with a bare CR precisely so they
    ///     overprint the same line. Feed there and the passes stagger
    ///     down the page in a coloured staircase.
    ///
    /// `Auto` (the default) settles it from the stream itself: it feeds
    /// on CR until it sees the guest send its own LF right after one, and
    /// then stops for the rest of the job. All three cases come out
    /// right with nothing to configure. `On` / `Off` pin the switch.
    enum class AutoFeed : uint8_t { Auto = 0, On, Off, Count };

    /// Which ribbon cartridge is fitted. There is no "colour mode" on an
    /// ImageWriter II: colour is a *four-band ribbon* you physically
    /// install, and the guest picks a band with `ESC K n`. With the plain
    /// black cartridge the printer still accepts ESC K — it just has one
    /// band, so everything comes out black. Modelled the same way here.
    ///
    /// Note this only decides what the ribbon *can* do. Software has to
    /// ask: Print Shop, for one, only emits ESC K when its Setup names an
    /// "Apple Imagewriter II **(C)**" — the (M) driver never sends colour
    /// no matter what is in the printer.
    enum class Ribbon : uint8_t { FourColour = 0, Black, Count };
    static const char* ribbonName(Ribbon r);
    void   setRibbon(Ribbon r) { ribbon_ = r; }
    Ribbon ribbon() const { return ribbon_; }

    static const char* autoFeedName(AutoFeed m);
    void     setAutoFeedMode(AutoFeed m);
    AutoFeed autoFeedMode() const { return feedMode_; }
    /// What the mechanism is actually doing right now (Auto resolved).
    bool     autoFeedActive() const {
        return feedMode_ == AutoFeed::On ||
               (feedMode_ == AutoFeed::Auto && !feedLatchedOff_);
    }
    /// True once Auto has seen the guest manage its own line feeds.
    bool     autoFeedLatchedOff() const { return feedLatchedOff_; }

    // Back-compat shims (tests + old settings): On/Off only.
    void setAutoFeed(bool on) {
        setAutoFeedMode(on ? AutoFeed::On : AutoFeed::Off);
    }
    bool autoFeed() const { return autoFeedActive(); }

    // ─── Data path ───────────────────────────────────────────────────────
    void printChar(uint8_t ch);
    void printBytes(const uint8_t* data, size_t n);

    // ─── Mechanism pacing ────────────────────────────────────────────────
    // The interface card hands bytes over at bus speed — a `PRINT` loop
    // spools a whole page in a millisecond of emulated time. A real
    // ImageWriter II eats them at the speed of its carriage and platen,
    // so `queueBytes` + `tick` model the mechanism: bytes wait in the
    // printer's input buffer and are printed as the head can reach them.
    //
    // Speeds are Apple's published figures (ImageWriter II Owner's
    // Manual, "Specifications"): 250 cps draft, 45 cps near-letter-
    // quality. `Instant` is the old behaviour — everything prints the
    // frame it arrives.
    /// Print quality the GUEST selected with `ESC a n` (Table 4-1:
    /// 0 = correspondence, 1 = draft, 2 = NLQ). Distinct from `Speed`
    /// below, which is the HOST's pacing knob — the two were conflated
    /// before the character ROMs landed, when NLQ could only mean "slower".
    enum class Quality : uint8_t { Correspondence = 0, Draft, NLQ, Count };
    static const char* qualityName(Quality q);
    Quality quality() const { return quality_; }

    enum class Speed : uint8_t { Instant = 0, Draft, NLQ, Count };
    static const char* speedName(Speed s);

    void  setSpeed(Speed s);
    Speed speed() const { return speed_; }

    /// Hand `n` bytes to the printer's input buffer. They print over the
    /// next ticks; nothing is rendered here.
    void queueBytes(const uint8_t* data, size_t n);
    /// Advance the mechanism by `dt` seconds of host time, printing
    /// whatever the head had time to lay down.
    void tick(double dt);
    /// Print what is still queued right now ("Print now" button, and every
    /// tick under `Speed::Instant`) — bounded by `kCatchUpSheets` ejects per
    /// call. Instant skips the *mechanism delay*, not the page-raster copy an
    /// eject costs, and `ESC R 999 <FF>` is one six-byte sequence asking for
    /// 999 of them. When the budget runs out `catchingUp()` stays true and
    /// the rest lands on the following ticks, so check `busy()` rather than
    /// assuming this drained the queue.
    /// Drain the queue with no mechanism delay.
    ///
    /// `budgetSheets` caps how many sheets one call may eject, leaving the
    /// rest armed for the next call. The AUTOMATIC per-frame path
    /// (`Speed::Instant`, which routes every `tick()` straight here) passes
    /// true, because the work behind a six-byte queue is unbounded —
    /// `ESC R 999 <FF>` asks for 999 ejects, ~2.1 s of raster copying at
    /// 300 dpi inside one UI frame, and it blows past PrinterHistory's
    /// eight-sheets-in-flight assumption on the way.
    ///
    /// The EXPLICIT "Print now" button passes false and runs the job out:
    /// the user asked for the whole thing once, and silently leaving pages
    /// on the platen after an explicit flush is worse than the wait.
    void flushPending(bool budgetSheets = false);

    /// Bytes accepted but not yet printed. This is the input *buffer* only
    /// — an `ESC R` run outstanding behind it is six bytes on the wire
    /// however many characters it still owes, which is also what the real
    /// buffer holds, so back-pressure keys off this and not off `busy()`.
    size_t pendingBytes() const { return pending_.size() - pendingHead_; }
    /// True while the mechanism still owes work: queued bytes, or an
    /// `ESC R` run only partly expanded. The repeat is paced like anything
    /// else it prints, so it outlives the byte that asked for it and
    /// `tick()` must keep being called until this clears.
    bool   busy() const {
        return pendingHead_ < pending_.size() || repeatRemaining_ > 0;
    }
    /// Bytes an `ESC R` / `ESC V` / `ESC U` run still owes. They never sat
    /// in the input buffer — the whole run arrived as one short sequence —
    /// so `pendingBytes()` cannot show them and a status line that reports
    /// only that would read "0 B queued" for a printer mid-run.
    size_t pendingRepeats() const { return repeatRemaining_; }
    /// Input bytes dropped to hold the backlog under its hard ceiling —
    /// see `kHardBacklog`. Nonzero means a printout came out truncated.
    size_t droppedInputBytes() const { return droppedInput_; }
    /// True while the mechanism is running flat out to clear a backlog
    /// (Draft/NLQ pacing suspended until the queue empties).
    bool   catchingUp() const { return catchUp_; }

    /// Stock ImageWriter II input buffer. The interface card raises BUSY
    /// to the Apple II while more than this is outstanding, which is what
    /// makes a printing guest actually wait for the printer.
    static constexpr size_t kInputBufferBytes = 2048;

    // ─── Trace log ───────────────────────────────────────────────────────
    // A printout that comes out as noise is a *protocol* problem: the
    // guest's driver and this printer disagree about the command set. The
    // only way to see that is the byte stream itself, decoded. The trace
    // writes every byte the mechanism consumes as a hex dump interleaved
    // with the decoded command, every page event, and every host event the
    // caller reports through `traceEvent` (BUSY, queue depth).
    //
    // Enable from the panel, or set `POM2_TRACE_PRINTER=1` before launch
    // (`POM2_TRACE_PRINTER=<path>` to choose the file).
    bool startTrace(const std::string& path, std::string& err);
    void stopTrace();
    bool tracing() const { return trace_ != nullptr; }
    const std::string& tracePath() const { return tracePath_; }
    /// Log a host-side line (printf-style). No-op when not tracing.
    void traceEvent(const char* fmt, ...);

    /// Rolling capture of the raw bytes the printer received, always on
    /// (last `kRawCaptureBytes`). The trace has to be armed *before* the
    /// job; this doesn't — so a printout that came out wrong can still be
    /// handed over byte-for-byte afterwards ("Save raw stream" in the
    /// panel). Cheap: an append to a capped vector per byte.
    static constexpr size_t kRawCaptureBytes = 256 * 1024;
    const std::vector<uint8_t>& rawStream() const { return raw_; }
    void clearRawStream() { raw_.clear(); }

    // ─── Front panel ─────────────────────────────────────────────────────
    /// FORM FEED button: eject the sheet (ignored when it is still blank).
    void formFeed();
    /// Power cycle: every setting back to factory defaults, sheet cleared.
    void resetPrinterHard();
    /// Power cycle *and* throw away the completed-sheet stack.
    void clearAll();

    // ─── Raster access ───────────────────────────────────────────────────
    int          pageWidth()  const { return current_.w; }
    int          pageHeight() const { return current_.h; }
    const Page&  currentPage() const { return current_; }
    bool         currentPageBlank() const;
    /// Sheets ejected since power-on. MONOTONIC, unlike completedPageCount()
    /// whose stack is capped at kMaxPages and reused — an archiver comparing
    /// counts must use this or it silently misses pages that fell off the
    /// stack between two of its polls.
    size_t       sheetsEjected() const { return sheetsEjected_; }
    size_t       completedPageCount() const { return pages_.size(); }
    const Page&  completedPage(size_t idx) const { return pages_[idx]; }
    size_t       droppedPageCount() const { return droppedPages_; }

    /// Expand an indexed page into top-down RGBA8 (4 bytes/pixel).
    static void pageToRgba(const Page& p, std::vector<uint8_t>& out);
    /// Palette entry for one indexed byte (see the file header encoding).
    static void indexToRgb(uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b);

    // ─── Odometer / UI helpers ───────────────────────────────────────────
    uint64_t bytesReceived() const { return bytesIn_; }
    /// Bumped on every change to the current raster, so the viewer can
    /// skip re-uploading an unchanged texture.
    uint32_t revision() const { return revision_; }
    Status   status() const;

private:
    // ─── Page geometry ───────────────────────────────────────────────────
    int       dpi_;
    PaperSize paper_;
    IwModel   model_   = IwModel::ImageWriterII;
    PrinterSoundSink* sound_ = nullptr;
    bool      powered_ = true;
    bool      online_  = true;
    double    defaultPageWidth_  = 8.5;   // inches
    double    defaultPageHeight_ = 11.0;

    Page              current_;
    std::vector<Page> pages_;
    size_t            droppedPages_ = 0;
    uint32_t          revision_     = 1;
    uint64_t          bytesIn_      = 0;

    // ─── Printer state (imagewriter.h:214-294) ───────────────────────────
    uint8_t  color_ = 7 << 5;             // COLOR_BLACK
    uint8_t  switcha_ = 0, switchb_ = ' ';
    uint8_t  msb_ = 0;                    // 255 = pass bit 7 through

    double   curX_ = 0.0, curY_ = 0.0;    // print head, inches

    uint16_t escCmd_  = 0;
    bool     escSeen_ = false, fsSeen_ = false;
    uint8_t  numParam_ = 0, neededParam_ = 0;
    uint8_t  params_[20]{};

    uint16_t style_ = 0;
    double   cpi_ = 12.0, actcpi_ = 12.0;
    uint8_t  verticalDot_ = 0;

    double   topMargin_ = 0.0, bottomMargin_ = 11.0;
    double   leftMargin_ = 0.25, rightMargin_ = 8.5;
    double   pageWidthIn_ = 8.5, pageHeightIn_ = 11.0;
    double   lineSpacing_ = 1.0 / 6.0;

    double   horiztabs_[32]{};
    uint8_t  numHorizTabs_ = 0;
    double   verttabs_[16]{};
    uint8_t  numVertTabs_ = 0;

    uint8_t  printRes_ = 2;               // pitch → graphics density index
    Quality  quality_  = Quality::Correspondence;   // ESC a n
    double   extraIntraSpace_ = 0.0;
    double   definedUnit_ = 96.0;
    double   hmi_ = -1.0;                 // horizontal motion index override

    Ribbon   ribbon_ = Ribbon::FourColour;   // see setRibbon()

    // ─── Line-feed-after-CR switch (see setAutoFeedMode) ─────────────────
    AutoFeed feedMode_       = AutoFeed::Auto;
    bool     feedLatchedOff_ = false;   // Auto saw the guest's own LF
    bool     crJustFed_      = false;   // last byte was a CR that fed

    // ─── Mechanism pacing (see queueBytes/tick) ──────────────────────────
    Speed                speed_ = Speed::Draft;
    std::vector<uint8_t> pending_;
    size_t               pendingHead_ = 0;
    double               credit_ = 0.0;   // banked mechanism seconds

    /// Seconds the mechanism needs to consume `ch` in the current state
    /// (glyph, dot column, carriage return, paper feed, or a free
    /// command byte). 0 in `Instant` mode.
    double byteCost(uint8_t ch) const;

    /// Cost of the next unit of work a drain will do: an outstanding
    /// repeat byte if there is one, else the byte at the head of the
    /// queue. 0 when there is nothing left to do.
    double nextUnitCost() const;

    // ─── Repeat expansion: ESC R / ESC V / ESC U (see printRepeatUnit) ───
    // Three commands ask, from one input byte, for work proportional to a
    // parameter the guest chooses:
    //
    //   ESC R nnn c        up to  999 copies of one character
    //   ESC V nnnn c       up to 9999 copies of one dot column
    //   ESC U nnnn abc     up to 9999 copies of one 24-pin dot column
    //
    // Expanding a run inside the byte that completes the sequence put all
    // of it inside one `tick()`, which neither budget below can bound —
    // both are only checked BETWEEN input bytes — and which `byteCost`
    // charged nothing for, since `numParam_ < neededParam_` holds on the
    // terminating byte (it is a register load). Measured: `ESC R 999 $0C`
    // (999 form feeds) 773 ms in one tick at Letter/144 dpi and 13.8 s at
    // Ledger/288 dpi, rolling the user's whole 32-sheet tray away in the
    // same frame; a stream of `ESC U 9999` 1.4 s in one catch-up tick
    // against 9 ms for the same backlog of plain text.
    //
    // So a run is resumable state instead: a drain emits ONE byte of the
    // pattern per iteration, priced by `byteCost` like any other byte the
    // mechanism consumes, and may yield with the remainder outstanding.
    // Clamping the count is not an alternative — a real printer really does
    // print 999 characters — and truncating a valid job would be silent
    // output corruption.
    //
    // ESC V / ESC U set the bit-image state up ONCE for their whole run
    // (`setupBitImage(printRes_, nnnn)`), so each outstanding byte flows
    // through `printBitGraph` and is priced at the dot-column rate; the
    // pattern is 1 byte for ESC R/V and the 3-byte column for ESC U.
    uint8_t  repeatPat_[3]{};
    uint8_t  repeatPatLen_ = 1;
    uint8_t  repeatPatPos_ = 0;
    uint32_t repeatRemaining_ = 0;      // pattern bytes still owed
    /// ESC V / ESC U force bit 7 through for the duration of their run
    /// (phase 1 sets `msb_ = 255`); the reference restores it when the run
    /// ends (imagewriter.cpp:1160-1200), which is now a tick or more later.
    bool     repeatRestoresMsb_ = false;

    /// Emit one outstanding pattern byte. Goes through
    /// `printCharInternal`, so a repeat run still counts as the handful of
    /// bytes the interface card delivered.
    void printRepeatUnit();

    /// Odometer + raw capture + trace for one byte the card delivered, then
    /// interpret it. Any repeat run the byte starts is left OUTSTANDING for
    /// the caller to drain: `tick()` spends it against the mechanism
    /// budget, `printChar()` runs it out on the spot.
    void acceptByte(uint8_t ch);

    /// Arm a repeat run of `count` bytes cycling through `pat[0..len)`.
    void armRepeat(const uint8_t* pat, uint8_t len, uint32_t count,
                   bool restoresMsb);

    /// How long the byte at the head of the queue has gone unaffordable.
    /// A cost model that can never afford a byte would wedge the printer
    /// *and* the guest waiting on it (that is exactly what a form feed
    /// under a too-low credit cap did), so past `kStallSeconds` the byte
    /// is forced through and the trace says so.
    double stalledFor_ = 0.0;
    static constexpr double kStallSeconds = 10.0;

    // ─── Backlog catch-up (see queueBytes/tick) ──────────────────────────
    // The mechanism may never fall more than `kMaxBacklog` behind the
    // card: hours of queued "mechanism time" parked on the heap is a leak
    // in all but name. Past that, Draft/NLQ pacing yields and the printer
    // catches up — but it must catch up ACROSS TICKS. Draining the
    // backlog inside `queueBytes()` (which is what this used to do) ran it
    // synchronously on the UI thread, from `pumpImageWriter()`: measured
    // 852 ms for plain text and 301 s for a form-feed storm, each in ONE
    // frame — the window stops repainting while audio and the CPU worker
    // carry on, which reads as a hard freeze rather than a slow printer.
    // The credit cap bounds credited *seconds*; only these
    // budgets bound the *work*. Sheets are budgeted separately from bytes
    // because an eject copies a whole page raster (~1.9 MB at Letter/144),
    // so a one-byte FF is four orders of magnitude dearer than a glyph.
    // Budgets sized against measured cost: ~0.8 us per glyph and ~0.5 ms
    // per eject (a Letter/144 raster memcpy), so 16 KiB + 4 sheets is
    // ~13 ms — under a 60 Hz frame — and clears the 1 MiB arming
    // threshold in about a second of wall clock.
    static constexpr size_t kMaxBacklog    = 1u << 20;    // arm catch-up
    static constexpr size_t kHardBacklog   = 4u << 20;    // drop past this
    static constexpr size_t kCatchUpBytes  = 16u << 10;   // per tick
    static constexpr size_t kCatchUpSheets = 4;           // per tick
    bool   catchUp_       = false;
    size_t droppedInput_  = 0;
    size_t sheetsEjected_ = 0;   // monotonic; budgets the eject rate

    /// Drop the consumed prefix of `pending_` (or clear it when the queue
    /// has drained). Shared by the paced and catch-up drains.
    void compactPending();

    // ─── Trace log ───────────────────────────────────────────────────────
    std::vector<uint8_t> raw_;         // see rawStream()
    std::FILE*  trace_       = nullptr;
    std::string tracePath_;
    double      traceClock_  = 0.0;    // seconds of tick() time, for stamps
    uint8_t     traceRow_[16]{};
    int         traceRowLen_ = 0;
    uint64_t    traceOffset_ = 0;
    void traceByte(uint8_t ch);
    void traceFlushRow();
    void traceCommand();

    /// Bit-image state (imagewriter.h:254-262).
    struct BitGraph {
        uint16_t horizDens = 72, vertDens = 72;
        bool     adjacent  = true;
        uint8_t  bytesColumn = 1;
        uint32_t remBytes  = 0;
        uint8_t  column[6]{};
        uint8_t  readBytesColumn = 0;
        /// Bit 7 is the TOP dot (Epson ESC/P) instead of bit 0 (C. Itoh).
        /// Get this wrong and every graphic comes out mirrored vertically in
        /// 8-pixel stripes, which still looks like a picture — so it is
        /// pinned by a round trip rather than by eye.
        bool     msbTop = false;
        /// Consume the body and plot nothing. Set when the FITTED head has no
        /// hardware for the graphics command that opened this run (an `ESC *`
        /// aimed at an MX-80, say): the count has already been parsed, so the
        /// data bytes must be eaten or they print as a screenful of text.
        /// A density of 0 cannot express this — `dotW = 1/horizDens` would go
        /// infinite and fillDots would clamp it to a filled row.
        bool     swallow = false;
    } bitGraph_;

    // ESC/P parameter collection. Separate from `params_` because the two
    // grammars disagree on how many bytes a command takes and on whether
    // they are ASCII digits (C. Itoh) or raw values (Epson).
    uint8_t  epsonParams_[4]{};
    uint8_t  epsonCount_ = 0;
    uint8_t  epsonNeed_  = 0;

    /// ASCII → CP437 glyph index. Identity except for the ten positions
    /// the international soft-switches (A-1..A-3) remap.
    uint8_t  curMap_[256]{};

    // ─── Internals ───────────────────────────────────────────────────────
    void resetPrinter();
    void rebuildPage();
    void newPage(bool save, bool resetx);
    void updateSwitch();
    void updateMetrics();
    void selectDefaultMap();

    /// printChar() minus the byte odometer — `printRepeatUnit` re-enters
    /// here so a 200-character `ESC R` run counts as the six bytes the
    /// interface card actually delivered.
    void printCharInternal(uint8_t ch);

    bool processCommandChar(uint8_t ch);
    void renderGlyph(uint8_t ch);
    /// Character-ROM bank the current quality + proportional state selects,
    /// the glyph it holds for `ch` (nullptr = fall back to the bundled font),
    /// and the advance a proportional face asks for (0 = use the pitch cell).
    const IwRomBank&      currentBank() const;
    /// True when this head has no hardware for `cmd` — swallow it.
    bool                  modelIgnoresEsc(uint8_t cmd) const;
    /// True when the fitted ESC/P head has `feature` (`IwEscPFeature`).
    bool                  modelHasEscP(uint32_t feature) const;

    // ── Diablo 630 (LaserWriter emulation mode) ──────────────────────────
    // A third parser over the SAME mechanism, on the FX-80's precedent.
    // Motion is by INDEX here — an HMI in 1/120 in and a VMI in 1/48 in
    // replace pitch and line spacing — and there are no graphics commands at
    // all, so nothing below the command layer changes.
    bool processDiabloChar(uint8_t ch);
    void execDiabloEscape();
    /// The motion indices land straight in the mechanism's existing `hmi_`
    /// and `lineSpacing_` — `hmi_` is already "the guest saying move exactly
    /// this far, which outranks the font" (see printCharInternal), which is
    /// precisely what a Diablo HMI is. No parallel state.
    uint8_t diabloCmd_ = 0;
    uint8_t diabloNeed_ = 0;
    /// Column tab stops, inches from the left edge. The 630 sets them at the
    /// CURRENT position (ESC 1) rather than by number.
    std::vector<double> diabloTabs_;

    // ── Epson ESC/P (printer plan phase C3) ──────────────────────────────
    // A second parser over the SAME mechanism. Everything below the command
    // layer — the page, the dot plotter, the ribbon, the pacing, the paper —
    // is shared; only the byte grammar differs.
    bool processEpsonChar(uint8_t ch);
    void execEpsonEscape();
    /// Arm an ESC/P bit image: `dotsPerInch` horizontal density and `columns`
    /// column bytes. Epson packs bit 7 as the TOP dot, the opposite of the
    /// C. Itoh family — see `bitGraph_.msbTop`.
    void setupEpsonBitImage(int dotsPerInch, uint32_t columns);
    const iwrom::IwGlyph* romGlyph(uint8_t ch) const;
    double                glyphAdvance(uint8_t ch) const;
    void setupBitImage(uint8_t dens, uint32_t numCols);
    void printBitGraph(uint8_t ch);

    /// Paint the page-pixel rectangle covered by [x, x+w) x [y, y+h)
    /// inches with full-intensity ink of the current ribbon colour.
    void fillDots(double xInch, double yInch, double wInch, double hInch);

    void lineFeed();
};

} // namespace pom2

#endif // POM2_IMAGEWRITER_H
