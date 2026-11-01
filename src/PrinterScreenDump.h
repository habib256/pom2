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

// PrinterScreenDump — "print what is on screen", as a dot-matrix bit-image
// stream.
//
// Ported from the design in mikedaley/web-a2e's `src/js/printer/screen-dump.js`
// (MIT, (c) 2025 Mike Daley), whose central insight is worth restating because
// it is what makes this honest rather than a screenshot pasted onto paper:
//
//   THE DUMP SYNTHESISES THE WIRE FORMAT PERIOD SOFTWARE WOULD HAVE SENT, AND
//   PUSHES IT THROUGH THE PRINTER'S REAL PARSER.
//
// Nothing here paints a page pixel. It emits the same `ESC G` byte stream a
// Grappler ROM or Print Shop drives the head with, hands it to
// `ImageWriter::queueBytes`, and lets the existing bit-image path do the rest.
// So the dump exercises the graphics parser, obeys the ribbon and the pacing,
// lands in the paper tray and the PDF export, and cannot drift away from what
// a real driver produces.
//
// ── The scan ──────────────────────────────────────────────────────────────
//
// The head prints a horizontal BAND at a time: 8 vertical dots packed into one
// column byte, one byte per horizontal position, then a line feed to the next
// band. The framebuffer walk is identical for every printer in the family;
// only the wire format differs, which is why the protocol constants sit in one
// place (`ScreenDumpProtocol`) rather than being sprinkled through the loop.
//
// C. Itoh / ImageWriter / Apple DMP:   `ESC G` + 4 ASCII digits, bit 0 = top
// Epson FX-80 (not implemented yet):   `ESC *` + 2 binary bytes, bit 7 = top
//
// ── Threshold and inversion ───────────────────────────────────────────────
//
// The screen is light-on-dark and paper is dark-on-light, so a naive dump of a
// text screen would flood the page with ink. `autoInvert` picks by lit
// density: mostly-dark screens (graphics) print their lit pixels, mostly-lit
// ones invert. A caller that knows better can force it.

#ifndef POM2_PRINTER_SCREEN_DUMP_H
#define POM2_PRINTER_SCREEN_DUMP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

struct ScreenDumpOptions {
    /// Luminance (0-255) at or above which a pixel counts as lit.
    int  threshold = 128;
    /// Force ink polarity. Ignored unless `autoInvert` is false.
    bool invert = false;
    /// Decide polarity from how much of the screen is lit — the sensible
    /// default, and what makes a text screen print black-on-white while an
    /// HGR picture prints its own lit pixels.
    bool autoInvert = true;
    /// Emit a form feed once the image is done, so the sheet is ejected and
    /// the next job starts clean.
    bool formFeed = true;
    /// ImageWriter / DMP family only: the pitch command to send once the
    /// image is done (ImageWriter::pitchCommand()), or 0 to leave the dump's
    /// own pitch selected. The dump picks a pitch for its graphics density
    /// (`ESC n`/`E`/`p`/`P`), and pitch persists: without this the next text
    /// job printed at 9 cpi, or proportional.
    uint8_t restorePitch = 0;
};

/// Build the byte stream for the ImageWriter / Apple DMP family (`ESC G`,
/// 72 dpi, bit 0 = topmost dot of the band).
///
/// `pixels` is 32-bit 0xAABBGGRR (RGBA little-endian, R in the low byte —
/// Apple2Display's native framebuffer format), `stride` in PIXELS (pass `w`
/// for a packed buffer). Appends to `out`; does not clear it.
void buildScreenDumpImageWriter(const uint32_t* pixels, int w, int h,
                                int stride, const ScreenDumpOptions& opt,
                                std::vector<uint8_t>& out);

/// Which graphics command an ESC/P head can actually be driven with. The
/// generic density selector `ESC *` is FX-generation; an MX-80 has only the
/// single-density `ESC K`, so a dump built with `ESC *` prints its own bytes
/// as text there instead of a picture.
enum class EpsonDumpCmd {
    Star72,   ///< `ESC * 5` — 72 dpi, one screen pixel per dot (FX-80, RX-80)
    K60,      ///< `ESC K`   — 60 dpi single density (every ESC/P head, incl. MX)
};

/// Build the byte stream for an Epson ESC/P head (a binary count and bit 7 as
/// the topmost dot of the band — both the opposite of the C. Itoh family,
/// which is exactly why this is a second builder and not a flag).
void buildScreenDumpEpson(const uint32_t* pixels, int w, int h, int stride,
                          const ScreenDumpOptions& opt,
                          std::vector<uint8_t>& out,
                          EpsonDumpCmd cmd = EpsonDumpCmd::Star72);

/// Whether `buildScreenDumpImageWriter` would invert, given these options and
/// this image. Exposed so the UI can say which way it is about to go.
bool screenDumpWouldInvert(const uint32_t* pixels, int w, int h, int stride,
                           const ScreenDumpOptions& opt);

} // namespace pom2

#endif // POM2_PRINTER_SCREEN_DUMP_H
