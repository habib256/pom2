# Printer emulation — gap analysis and implementation plan

**Status: COMPLETE (2026-08-10).** Round two — the Epson generations, the
C. Itoh cousins and the LaserWriter — is in
[`printer_plan_2.md`](printer_plan_2.md).
 All six phases shipped. POM2 now prints with the
real ImageWriter faces, can dump the screen through the printer's own graphics
parser, has a power switch and custom paper, and models three C. Itoh heads
(ImageWriter II / I / Apple DMP) plus the Epson FX-80 with its own ESC/P
parser. What implementation
changed is recorded in [§ 12](#12-what-implementation-changed). The living
description of the printer is [DEV § ImageWriter](../DEV.md#imagewriter-ii-printer-host-side)
(and [§ Print history](../DEV.md#print-history-printerhistory),
[§ Screen dump](../DEV.md#screen-dump-printerscreendump),
[§ Printer sound](../DEV.md#printer-sound-printersounddevice)); this file is
the plan's record.

Reference:
[`mikedaley/web-a2e`](https://github.com/mikedaley/web-a2e) (MIT, C++/JS Apple
//e emulator whose printer subsystem is the most complete open implementation
of this hardware family available), read at commit-of-2026-08-08.

POM2 already has a strong ImageWriter II: a full C. Itoh command parser ported
line-for-line from AppleWin's `imagewriter.cpp`, a subtractive four-band colour
ribbon model, cycle-accurate draft/NLQ pacing, a paper tray and PDF export.
The gaps are not in the *command set* — they are in **what the head puts on
paper**, in **how many heads there are**, and in a handful of host-side
features POM2 never grew.

## Table of contents

- [1. Where POM2 was (pre-plan, 2026-08)](#1-where-pom2-was-pre-plan-2026-08)
- [2. The gaps, ranked (pre-plan)](#2-the-gaps-ranked-pre-plan)
- [3. Licensing and provenance](#3-licensing-and-provenance)
- [4. Phase A — real character ROMs](#4-phase-a--real-character-roms--shipped)
- [5. Phase B — screen dump](#5-phase-b--screen-dump--shipped)
- [6. Phase C — more printer models](#6-phase-c--more-printer-models--shipped)
- [7. Phase D — paper geometry, power and online](#7-phase-d--paper-geometry-power-and-online--shipped)
- [8. Phase E — print history](#8-phase-e--print-history--shipped)
- [9. Phase F — printer sound](#9-phase-f--printer-sound--shipped)
- [10. What NOT to take from web-a2e](#10-what-not-to-take-from-web-a2e)
- [11. Effort and ordering](#11-effort-and-ordering)
- [12. What implementation changed](#12-what-implementation-changed)

## 1. Where POM2 was (pre-plan, 2026-08)

Historical inventory — every row below has since shipped (§ 4-§ 9).
From `src/ImageWriter.h/.cpp` (719 + 2141 lines today):

| Area | State |
|---|---|
| Command parser | **Strong.** ESC C/D/F/G/H/I/K/L/R/S/T/U/V/Z/a/g/h/l/s/t/u/'/(/) …, soft switches, user-defined characters, tabs, margins — ported from AppleWin `imagewriter.cpp` with cited line ranges |
| Colour ribbon | **Strong.** Four-band subtractive model, one byte per page pixel (`yyyxxxxx`: 5 bits ink, 3 bits band) |
| Pacing | **Better than the reference.** Draft/NLQ cps from Apple's published figures, `emuCycles`-driven, with a catch-up model |
| Paper | Fixed `PaperSize` enum (Letter/…); auto-feed DIP modelled with a latch |
| Export | PDF (`ImageWriterPdf`), completed-sheet stack |
| **Glyphs** | **Substitute.** `hgrpaint::kBBFontCp437`, an 8×8 CP437 bitmap font — the header says so plainly |
| **NLQ** | **Pacing only.** `Speed::NLQ` changes cps; the glyph rendered is identical to draft |
| **Proportional** | **Nominal.** `ESC p` selects the pitch but keeps a fixed 7-dot cell — "the font has no per-glyph advance table" |
| International charsets | Remapped into the nearest CP437 glyph |
| Models | ImageWriter II only |
| Screen dump | None |
| Power / online switch | None (there is a power-cycle *reset*, not a switch) |
| Print history | None beyond the in-session sheet stack |
| Printer sound | None |

## 2. The gaps, ranked (pre-plan)

The pre-plan ranking — all six gaps have since shipped (§ 4-§ 9).
Ranked by visible fidelity per unit of work, not by size.

1. **Character ROMs.** Everything POM2 prints as text is drawn with a generic
   bitmap font. This is the difference between "a dot-matrix-ish printout" and
   "an ImageWriter printout", and it is on every single page.
2. **Screen dump.** "Print what is on screen" is a headline period feature
   (Grappler ROM, `PR#1` + Ctrl-I G) and POM2 has no path to it at all.
3. **More heads.** ImageWriter I, Apple DMP (C. Itoh 8510), Epson FX-80. Much
   period software targets the FX-80 specifically.
4. **Paper geometry / power / online.** Small, cheap, and each removes a
   "why does nothing print?" confusion.
5. **Print history.** POM2 exports PDFs but forgets everything between runs.
6. **Printer sound.** Pure delight; POM2 already has the machinery
   (`FloppySoundDevice` + the `emuCycles`-stamped audio bus).

## 3. Licensing and provenance

**web-a2e is MIT** (`Copyright (c) 2025 Mike Daley`). MIT is GPLv3-compatible:
POM2 may incorporate it provided the MIT notice travels with the derived files.
Every ported file must carry the attribution the way POM2 already cites MAME
and AppleWin.

**The font data needs its own paragraph.** The ROM headers state:

> *Source: Apple ImageWriter II Technical Reference Manual, Appendix C.
> Transcribed via rom-editor.html.*

So these are **not chip dumps** — they are the dot patterns Apple *published*
in the printer's own technical reference, transcribed by hand. That is a
weaker claim than a ROM image but it is still Apple-authored typeface data,
and Mike Daley's MIT grant covers his transcription, not Apple's underlying
design. This is the same category POM2 already accepted for the SSI263 phoneme
blob (AppleWin, "the only extant reference"), and it should be recorded the
same way: in `docs/lle_vs_hle.md` and in the file header, stating the source
manual and the transcription, so the provenance is visible rather than
implied.

**DECIDED (2026-08-10): keep the web-a2e tables, under MIT.** The provenance is
recorded in `src/ImageWriterRom.h`, in `tools/import_printer_roms.py` and in
`docs/lle_vs_hle.md`; the MIT notice travels with the derived files. The CP437
fallback stays regardless — it is the graceful degradation for codes no bank
carries, not just an escape hatch.

## 4. Phase A — real character ROMs ✅ SHIPPED

The headline change. Two glyph tiers, from web-a2e's data:

| Bank | Shape | Notes |
|---|---|---|
| Draft | 12 columns × 9 wires (bit 0 = wire 1 … bit 8 = wire 9, so values exceed `0xFF`) | col 11 is the trailing blank spacer |
| NLQ fixed | 16 columns × up to 18 rows | the second head pass is what makes it "letter quality" |
| NLQ proportional | 16 columns + per-glyph advance | this is what makes `ESC p` mean something |
| Locale overrides | 10 code points (`$23 $40 $5B-$5D $60 $7B-$7E`) × 7 locales (UK/FR/DE/IT/SE/ES/DK) | replaces POM2's CP437 approximation |

**Work.**

1. `tools/import_printer_roms.py` — transcode the JS tables to a generated
   C++ header (`src/ImageWriterRom.h`, `constexpr uint16_t[]`). The
   data is a plain `{code: [12 numbers]}` map, so this is a 40-line script.
   Keep the script in-repo: it is the provenance record, and re-running it is
   how a future locale gets added.
2. Teach `ImageWriter` a **glyph source**: draft vs NLQ vs proportional,
   selected by the existing `Speed`/`ESC a`/`ESC p` state that is *already
   parsed and currently ignored for rendering*.
3. Per-glyph advance for proportional mode — the one structural change, since
   the current plotter assumes a fixed 7-dot cell.
4. NLQ as a genuine second pass at half-dot vertical offset, not just slower
   cps.

**Do not remove the CP437 fallback.** It is the graceful degradation for any
code point the ROM tables do not carry, and it keeps the printer working if a
future licensing decision drops the tables.

**Test.** `printer_glyph_test`: render a known string at draft, NLQ and
proportional, and compare the ink raster against a checked-in golden. web-a2e
ships snapshot tests (`tests/js/printer/__snapshots__/`) whose expectations can
seed POM2's goldens — the same trick the project used for the Klaus 6502 suite.

## 5. Phase B — screen dump ✅ SHIPPED

The cheapest big win, because **POM2 already owns both ends**: a framebuffer
(`Apple2Display::pixels()`) and a graphics parser (`ESC G` / `ESC C` / `ESC S`).

web-a2e's design is the one to copy, and its own header states the insight:
the dump must *synthesise the wire format the period software would have sent*
and push it through the printer's real parser, never paint pixels onto the
page directly. One framebuffer scanner (pack 8 vertical pixels into a column
byte) plus a small frozen per-printer protocol descriptor covering the only
four things that actually differ between heads:

| | C. Itoh / ImageWriter / DMP | Epson FX-80 |
|---|---|---|
| Graphics command | `ESC G` | `ESC *` |
| Column count | 4 ASCII digits | 2 binary bytes (nL nH) |
| Top-dot bit | bit 0 (LSB) | bit 7 (MSB) |
| Band line feed | `ESC T 16/144"` | `ESC 3 24/216"` |

**Work.** `src/PrinterScreenDump.h/.cpp` (~250 lines): framebuffer → column
bytes → `imageWriter->queueBytes()`. Threshold + invert options, auto-picking
invert by lit density as web-a2e does. UI: a button in the ImageWriter panel
and a `printer.dumpscreen` command-palette entry.

**Test.** `printer_screen_dump_test`: a synthetic 560×192 buffer with a known
pattern → assert the emitted stream is a well-formed `ESC G` sequence and that
running it back through `ImageWriter` reproduces the pattern. That round trip
is the real assertion — it pins scanner and parser against each other.

## 6. Phase C — more printer models ✅ SHIPPED

POM2's parser **is** the C. Itoh core already, so the family splits cheaply:

- **ImageWriter I** — the same core minus draft/NLQ tiers, colour ribbon and
  selectable form lengths, plus its own 7×8 charset. Mostly a feature-mask and
  a second ROM bank. *~0.5 d once Phase A lands.*
- **Apple DMP** (rebadged C. Itoh 8510) — same core, pica/elite/proportional,
  black only, its own ROM. *~0.5 d.*
- **Epson FX-80** — **a different lineage (ESC/P), and the real work.** Its own
  parser, Roman + Italic faces, international charsets, its own proportional
  handling. web-a2e's `epson-fx80.js` is 35 KB plus 23 KB of ROM. *~3 d.*

### What was actually built, and why not a class hierarchy

The plan called for splitting `ImageWriter` into a `DotMatrixPrinter` base plus
per-model subclasses, mirroring web-a2e's `CItohPrinter` → `ImageWriterII`.
Reading the two model classes settled it differently: `imagewriter-i.js` and
`apple-dmp.js` are almost entirely **overrides that return data** — which ROM
banks exist, whether there is a colour ribbon, which ESC codes have no hardware
behind them, the power-on pitch, the carriage rate. That is capability DATA,
not divergent behaviour.

So POM2 has `IwModelProfile`: one struct, one table of three. Adding a
difference is adding a member; it is not adding a code path. That kept a
2141-line, heavily-tested class intact — the whole 174-test suite stayed green
through the change — where a hierarchy refactor would have touched every
method to gain nothing the table does not give.

**The FX-80 is the case that WOULD justify a base class**, and that is exactly
why it is not in the table: it is a different lineage, not a capability mask.

### A finding worth recording

The ImageWriter I and Apple DMP correspondence banks are **byte-identical to
the ImageWriter II's** — verified against the source data, not inferred.
web-a2e seeds them from the II and falls back to it for anything not yet
transcribed (its own comments say so). POM2 still carries them as separate
banks so a future upstream divergence lands automatically rather than silently
keeping the II's face, and `printer_glyph` explicitly does NOT assert that the
three faces differ, with the reason written down next to it.

What genuinely differs today, and is pinned: power-on pitch (the DMP comes up
at Pica 10 cpi, so the same text is visibly wider), the ESC set each head has
no hardware for, the colour ribbon, and the carriage rate.

### C3 — the Epson FX-80 ✅

Shipped as a **second parser over the same mechanism**. The page, dot plotter,
ribbon, pacing and paper below the command layer are the ones the C. Itoh heads
use; only the byte grammar differs — and it differs enough that sharing a
dispatch is impossible, which the test pins directly: `ESC G` is *graphics* on
the C. Itoh family and *double-strike* on the Epson, so the same four bytes
produce 0 dots on one head and 86 on the other.

Implemented: `ESC @`, the style set (`E/F`, `G/H`, `4/5`, `S/T`, `- n`, `W n`,
`! n` master select), pitch (`M`, `P`, SI/DC2 condensed, SO/DC4 expanded), line
spacing (`0/1/2`, `3 n` at n/216, `A n` at n/72, `J n`, `j n`), form length
(`C n`, `N n`, `O`), `R n` international sets, and graphics — `ESC K/L/Y/Z` and
the generic `ESC * m n1 n2`, with **two binary count bytes and bit 7 as the top
dot**, both the opposite of the C. Itoh spelling.

Deliberately NOT implemented, and consumed with their parameters so a stray
byte never prints as text: user-defined characters (`ESC &`, `%`, `:`), the
vertical forms unit (`b`, `/`, `B`), nine-pin graphics (`^`), horizontal tab
lists (`D`), margins (`l`, `Q`) and graphics-letter reassignment (`?`). That
consumption is the difference between "a missing feature" and "gibberish on the
page".

One trap: `resetPrinter()` has to clear the ESC/P parameter collector as well,
or a reset arriving mid-command leaves the parser expecting parameters and it
eats the start of the next job.

Worth noting: the FX-80 unlocks the FujiNet path too — the FujiNet firmware's
own printer emulation is `epson80` (`lib/device/iwm/printer.cpp`), so a guest
printing through a FujiNet is speaking ESC/P.

## 7. Phase D — paper geometry, power and online ✅ SHIPPED

- **Custom paper in ¼-inch increments**, with *per-model* clamping: web-a2e's
  ImageWriter II bottoms out at a 3.5" tractor (≈4.0" outer) where the DMP and
  IW I stop at 4.5", and its `ESC H` reaches ~69" in 1/144" steps. Replace
  POM2's `PaperSize` enum with a range + presets.
- **Power switch** — off ignores incoming bytes *and preserves the paper*
  (distinct from POM2's existing power-cycle reset, which clears it).
- **Online / offline** — offline stops accepting; this is what the front-panel
  button does and what "select" means to the software.

Each is small, and each removes a class of "nothing is printing" confusion.
*~1 d together.*

## 8. Phase E — print history ✅ SHIPPED

web-a2e persists every completed page (IndexedDB) with metadata (model,
ribbon, form size, timestamp) and can re-preview a past job onto the paper.

Shipped as `printouts/history/` — one PNG per ejected sheet plus an index —
with a "Print history" section in the ImageWriter panel: the stored pages
listed newest first with their printer, ribbon and paper, click a row to put
that sheet back on the canvas, delete one or all.

**The index is NOT JSON**, which the plan asked for. POM2 has no JSON *parser*
— the AI control server only ever writes it — and pulling one in to read a few
dozen index lines would be the tail wagging the dog. It is tab-separated text:
trivial to write, trivial to parse, survives a truncated final line, and a user
can read it in a terminal when something looks wrong.

**Nor is there re-export**, and that is not an omission: the stored page IS a
PNG. Re-exporting it would mean decoding back into the printer's ink+band page
format and out again, which is lossy and pointless when the file is already the
artefact. The panel says where the folder is.

Four things the implementation had to get right, each pinned:

- **The eject counter must be the monotonic one.** `ImageWriter`'s page stack
  is capped at 32 and reused, so an archiver comparing `completedPageCount()`
  between frames silently misses pages that fell off it during a form-feed
  burst. `sheetsEjected()` was exposed for this, and when the archiver cannot
  reach everything it *says so* rather than pretending.
- **Archiving runs on every path** through `pumpImageWriter`, including the
  "no source this frame" early return — a job already inside the printer's
  buffer keeps ejecting sheets after its card is unplugged.
- **The filename counter resumes across sessions**, or a reload would clobber
  an existing page's PNG and the history would show two rows of one image.
- **The index is written to a temp file and renamed.** A crash mid-write then
  leaves the previous index intact instead of a truncated one, and a foreign
  or unrecognised index yields an EMPTY history rather than rows pointing at
  files whose meaning POM2 cannot vouch for.

## 9. Phase F — printer sound ✅ SHIPPED

**Two things in this section were wrong, and implementing it is what showed
that.**

*Wrong 1: `emuCycles`.* The floppy needs an emulated-cycle stamp because the
GUEST drives the stepper directly, so disk turbo collapses the wall-clock gaps.
The printer is not like that: `ImageWriter` consumes its queue on its OWN
wall-clock pacing (`tick(double dt)`, at the head's cps), so the guest can fire
a job in at any speed and the head still moves at 180 cps. The events are
already in real time when they are emitted, and stamping them would add
nothing. `PrinterSoundSink` says so where someone would otherwise wonder.

*Wrong 2: `FloppySoundDevice` as the model.* The floppy plays MAME's WAV set.
There is no equivalent free ImageWriter sample set — and, checked rather than
assumed, **web-a2e ships no audio assets at all**; its `printer-sound.js`
synthesises. So this is synthesis, ported from that model.

**The model.** A dot-matrix impact is a short broadband NOISE click, not a tone
(head energy near ~900-1000 Hz with a skirt to ~5 kHz, no clean fundamental) —
using an oscillator is what makes a printer emulation "sing" instead of clack.
One GRAIN per printed character (11 ms) or line feed (40 ms), bandpassed noise
with a wide Q, **spaced along the audio timeline** rather than all starting at
the instant the event arrived. That spacing is the whole trick: at print rate
the grains overlap into the continuous buzz, while a lone character is one
tick, so print density drives the texture for free.

**Adapting a Web Audio design to a pull mixer.** The reference schedules on an
`AudioContext` timeline; POM2's mixer pulls a mono float buffer, so the
timeline becomes an audio FRAME COUNTER the audio thread advances and the UI
thread reads when it stamps a grain — the arrangement `FloppySoundDevice`
already documents for its step cadence.

**The load-bearing detail** is the cap on how far ahead the scheduling cursor
may run (0.2 s). A full-black screen dump is tens of thousands of strikes in
one UI frame; scheduled naively at 5 ms apart that is *100 seconds* of buzz
still playing long after the page is done. With the cap the burst simply
THINS. The test fires 20 000 strikes and asserts the noise is over in under
100 buffers (~0.58 s) rather than 17 000.

Pinned by `printer_sound`, which has no ears and therefore asserts structure:
the cap, that dense print sustains where one character ticks, that pin count
scales level, that power-off is immediate and buffers nothing, and that a
sample-rate change drops grains built for the old rate.

## 10. What NOT to take from web-a2e

- **Its rendering of dots.** POM2's `fillDots` (dots painted as the page-pixel
  interval they cover) is deliberately *better* than the reference's scaling
  heuristic — adjacent dots abut exactly at any page DPI, so graphics dumps
  have no seams. Keep it.
- **Its pacing.** POM2's is `emuCycles`-driven and tied to the emulated clock;
  a browser emulator's is not. Keep POM2's. *(As built, 2026-10-06: the head
  paces itself on host time, `ImageWriter::tick(double dt)` at the model's
  cps — see § 9 "Wrong 1" and `ImageWriter.h:389`.)*
- **The agent/tool layer** (`printer-tools.js`) — POM2's equivalent is the AI
  control API, and the commands should be added there, in POM2's own idiom,
  not ported.
- **`printer-window.js`** (144 KB) — that is web-a2e's whole UI. POM2 has
  `ImageWriter_ImGui`; take the *feature list*, not the code.

## 11. Effort and ordering

| Phase | Deliverable | Estimate |
|---|---|---|
| A | Character ROMs: import script, draft + NLQ + proportional banks, locales, glyph test ✅ | 3.5 d |
| B | Screen dump + round-trip test ✅ | 1 d |
| D | Paper geometry, power, online ✅ | 1 d |
| C1 | ~~`DotMatrixPrinter` base extraction~~ → `IwModelProfile` table ✅ | 0.5 d (was 1.5) |
| C2 | ImageWriter I + Apple DMP ✅ | 1 d |
| C3 | Epson FX-80 (own ESC/P parser + ROM) ✅ | 3 d |
| E | Print history ✅ | 1.5 d |
| F | Printer sound ✅ | 1 d |
| | **Total** | **~13.5 d** |

**Ordering rationale.** A first: it improves *every* page POM2 already prints,
and it is independent of everything else. B next because it is a day's work
for a headline feature and it exercises the graphics parser hard. D is cheap
and removes user confusion. Only then C, whose first step is a refactor that
must land in one piece. E and F are polish and can slip.

**A good stopping point is after B+D** (~5.5 d): at that point POM2 prints
authentic ImageWriter text, can dump the screen, and behaves like a printer
with a power switch. C onwards is about *breadth of models*, which matters
much less if the user only ever prints from Apple software targeting the
ImageWriter.


---

## 12. What implementation changed

**The provenance question was NOT settled by me.** The ROM tables are in the
tree with their source stated in `src/ImageWriterRom.h`, in
`tools/import_printer_roms.py` and in `docs/lle_vs_hle.md`. If the answer is
"no", deleting the generated header and the `IwRomBank` selector restores the
old behaviour — the CP437 fallback was kept precisely so that stays a one-file
revert.

**Two generator bugs worth remembering**, both of which produced code that
compiled clean and printed wrong:

1. **A backslash at the end of a `//` comment continues it onto the next
   line.** The importer annotates each row with the character it draws, and
   `$5C` is `\`. That swallowed the following row, so every bank held 94
   initialisers for a 95-element array — no diagnostic, because the array is
   fixed-size and the tail simply zero-fills. The symptom was one blank glyph
   (`~`) per bank, three layers away from the cause.
2. **Comment text is not brace-free.** The first attempt walked braces to find
   the object body, and the row comments `// {` and `// }` were counted. Fixed
   by stripping comments first.

**`MAX_COLS` is 20, not 18.** The correspondence proportional bank reaches 19
columns (`$5B`). The plan's table said "16 columns" for NLQ, which is true of
the *fixed* bank only.

**One existing test asserted the old bug.** `imagewriter_smoke`'s `ESC 1..6`
case pinned a proportional advance of `0.1 + 3/120`, i.e. the fixed cell that
proportional mode was wrongly using. It now measures the baseline advance and
asserts `ESC 3` *added* 3/120" to it — which is what that test's own comment
always said it was about, and it no longer pins font data.

**Power off does not clear the sheet.** POM2 already had `powerCycle()`, which
resets everything and wipes the paper. The front-panel switch is a different
thing and had to be, or "power off" would lose the user's page — so it gates
the input path and touches nothing else.

**The screen dump is a palette command, not a menu item.** POM2 exposes
`saveScreenshot` the same way (F9 / palette / toolbar), so `printer.dumpscreen`
follows that rather than inventing a menu.

Not done from the phases marked shipped (rechecked 2026-10-06): per-model
paper ranges (eleven `IwModel` values exist now, `ImageWriter.h:99-149`, but
the paper range is still one set of class constants,
`ImageWriter::kMin/kMaxPaperWidthIn` / `…LengthIn` at `ImageWriter.h:352-355`,
shared by all of them), and the `printerSetup`-style AI control commands —
the AI control API has since grown the card-side `/printer-port`,
`/slot-log` and `/printer/spool` ([printer-detection § 12](printer-detection.md#12-pom2s-controls-cli-http-library)),
but still nothing that selects the printer model, ribbon or paper.
