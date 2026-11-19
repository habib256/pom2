# Backlog — Display

[Planning index](../../TODO.md) · [Scope decisions](../decisions/project-scope.md)

Reorganised 2026-10-05. Priorities are inherited from the original TODO;
older reports are **To verify**, not confirmed defects on current main.
Before implementation, reproduce the described behavior on current main and
record the result. If already resolved, remove the active entry and link its
resolution in the changelog. Frozen work requires a named software need or
an explicit request before scheduling.

<a id="historical-findings"></a>

## Task index

| Task | Priority | State | Subject |
|---|---|---|---|
| [DISPLAY-001](#display-001) | 🟡 | Partial; needs hardware oracle | VidHD on Apple IIe — SHR display support. |
| [DISPLAY-011](#display-011) | 🟡 | Partial: Eve Leonard golden; rest needs oracle | Le Chat Mauve — what is left after P0-P2 |
| [DISPLAY-012](#display-012) | 🧊 | Frozen: needs a colour oracle | 80-column text colour phase under the CPU demodulators |
| [DISPLAY-002](#display-002) | 🟢 | Partial: ROM and PAL split goldens closed | Fallback upload measurement |
| [DISPLAY-008](#display-008) | 🧊 | Frozen — triage 2026-10-28 | "Smooth" interpolated sub-pixel mode |
| [DISPLAY-010](#display-010) | 🟢 | Partial: F6 only | CRT parity refinements vs OpenEmulator |
| [DISPLAY-003](#display-003) | 🧊 | Frozen | Pure-analog signal-level composite pipeline |

<a id="display-001"></a>

## DISPLAY-001 — VidHD on Apple IIe — SHR display support.

**Priority:** 🟡 · **State:** Partial, needs a hardware oracle ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): IIgs text/border colours landed 2026-10-21; a real VidHD capture is the remaining evidence.

**Acceptance criterion (after revalidation):** Render VidHD SHR on the IIe with documented register behavior and a reference image or hardware-derived test.

**Status (2026-10-17).** Ported from AppleWin's partial model: the `$Cn00-02`
signature, `$C022/$C029/$C034/$C035` latched off motherboard writes, SHR
(NEWVIDEO `$C0`) painted 640×400 from the CPU's aux bank in 320, 320 + fill
and 640 modes through the display's `CardVideoSource` seam; //e-class slotted
profiles only. Register behaviour is documented in
[DEV § VidHD](../../DEV.md#vidhd-vidhdcard) and pinned by `vidhd_card`
against hand-computed pixels — not yet against a real VidHD capture. Still
open: the VidHD's extended text modes (the IIgs text/border colours landed
2026-10-21), the ][+ aux
mirror, mid-frame SCB/palette changes.

The 2026-10-17 card audit corrected the detection window from `$C0nX` to
the slot ROM `$CnXX`, following AppleWin's actual `InitializeIO` handler
registration. `vidhd_card` pins all seven slot signatures, floating-bus
fallback and the absence of a C800 claim, as well as the SHR painter.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1072). Original dates and estimates are retained below.

- [ ] **VidHD on Apple IIe — SHR display support.** Model the VidHD card's
  display registers and video-memory observation so IIe software can display
  Super Hi-Res images. This is a display extension: VidHD does not add a
  65816 processor or the IIgs Toolbox and does not turn a IIe into a IIgs.
  Displaying an SHR image alone does not enable execution of IIgs
  applications. Reference: [VidHD Manual 1.2](https://www.callapple.org/docs/vidhd/VidHD_Manual_1.2.pdf).

- [ ] **RamWorks bank under the SHR picture (left as is, bug hunt
  2026-11-01).** The VidHD paints from the aux bank the CPU has selected
  (`Memory.cpp` `setAuxRamRef(aux.data())`, pinned by `vidhd_card` § 4,
  as AppleWin's `MemGetAuxPtr`), while the motherboard scanner reads bank 0
  (`videoAuxData`, MAME `a2eramworks3`). A slot card cannot read the aux
  slot; a real VidHD builds its picture from the aux writes it snoops, so a
  bank switch with no write would not change it — in POM2 it does. Options:
  (a) hand the card `videoAuxData()` (re-pointed on every bank swap), cheap
  but blind to SHR written while another bank is selected; (b) a VidHD-owned
  32 KB shadow fed by every aux write, the hardware model, at the cost of a
  hook on the aux write slow path (`docs/PERFORMANCE.md` § 8 first). Needs a
  VidHD + RamWorks capture to choose.

<a id="display-011"></a>

## DISPLAY-011 — Le Chat Mauve — what is left after P0-P2

**Priority:** 🟡 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): one actionable golden left; the other residuals are frozen until an oracle exists.

**Acceptance criterion:** A third Eve golden from Eve Leonard joins `purplesoft_eve_screens` / `chatmauve_feline_screens`; the Féline and Eve suites stay green unchanged.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1126); [triage 2026-10-28](../audits/backlog-triage-2026-10-28.md).

**Féline and Eve are core** — dot-exact against AppleWin's hardware-validated
oracles, with golden suites (`purplesoft_eve_screens`, `chatmauve_dot_rules`,
`chatmauve_feline_screens`, `chatmauve_latch_split`). Done since the original
entry, confirmed on `aad2fee5`:

- **Video-7 AppleColor RGB** — 160×192 chunky mode, F/B HGR, colour text
  (`LeChatMauveCard.h`: `Variant::Video7`, `Chunky160`, `FgBg`).
- **RVB Graph** — modelled from its manual on 2026-10-20 (`3c091891`):
  control register at `$C0F0+S+U+K`, eight colour registers at
  `$C70F/$C71F/$C72F/$C73F − Bn`. Unverified on hardware.
- **P7** — the doc step is recorded done in `docs/chatmauve_plan.md`.

**Actionable:** fetch **Eve Leonard** and pin a third Eve golden. It is not
in `disks_5.4/chatmauve/` today.

**Frozen until an oracle** (a board, a scope trace or a schematic): the Eve's
`$C0Bx` switches as beam-time events (applied directly today, `eveDecode`,
`LeChatMauveCard.cpp:213-222`), the exact in-cell dot position, the TTL-RGBI
palette, the three Féline trim pots (R/G/B gain, manual p. 13), the
//c-adapter quirk (P5), Color killer Rev 1 and the 4K→48K strapping RAM.
**P3 stays closed as bounded**: the PLA is a dot-stream router, not the
colour decoder.

<a id="display-012"></a>

## DISPLAY-012 — 80-column text colour phase under the CPU demodulators

**Priority:** 🧊 · **State:** Frozen until a colour reference exists.

**Scheduling condition:** a capture of real 80-column text through a colour
monitor (or another emulator with a stated source) to compare against.

80-column TEXT under the OE/AppleWin CPU demodulators uses phase 0 where
MAME's `is_80_column` term would apply; there is no colour oracle, and the
term is per-frame on a beam-raced split.

**Closed 2026-10-24, no change:** the II/II+ FLASH phase. Bug hunt #15
noted it blinks in antiphase with MAME's model::II. On those machines the
blink comes from a free-running 555 (MAME `apple2video.cpp:1176-1178`,
"approximate 555 flash timer"), unsynchronised with video, so its phase at
power-on is not defined on the hardware or in MAME; POM2's rate (16-frame
half period, ~1.9 Hz) matches. The Videx marker documentation was corrected
on 2026-10-19 (`normaliseCharRom` handles the marked dump).

<a id="display-002"></a>

## DISPLAY-002 — Golden coverage gaps

**Priority:** ✅ · **State:** Done 2026-11-19 (measured; no change); removed from the active index.
2026-11-07; PAL split goldens closed 2026-11-17; only the fallback-upload
measurement remains.

**Acceptance criterion for the remainder:** Measure whether OE-GPU's
fallback-framebuffer upload is actually performed and unused before changing
it. A measurement, not a code change.

**Closed 2026-11-17:** `display_golden_hash` adds four PAL beam-raced split
frames — TEXT/HGR bands on line edges, a mid-line TEXT|HGR split over 32
lines, a PAGE2 flip, DHGR/HGR through AN3 with a mid-line return — under the
seven integer pipelines and the composite signal: 32 pins, 248 in all. The
script names raster positions (line, horizontal cycle) and derives each cycle
from the standard's frame length, in the third frame; every pin is also
rendered on NTSC and must be identical. Mutation control: reading the PAL
cycle count on a 262-line frame in `Memory`'s event recorder fails all 32.
**Limit:** these freeze POM2's current split placement, they do not grade it —
the column offsets themselves are argued in `Apple2Display_Beam.cpp` and
pinned by `raster_switch_kind_offset`.

**Closed 2026-11-07:** `display_golden_hash` adds four real Enhanced-ROM
scenes (40/80 columns × ALTCHAR off/on), freezing all seven deterministic
CPU pipelines and the composite signal: 32 additional pins, with the original
fallback pins unchanged. An independent pixel oracle reads the raw active-low
ROM bytes and checks every screen code, both FLASH phases, MouseText,
inverse lowercase and main/aux cell order. The ROM is **not tracked**:
`tools/test_media.sha256` pins `roms/apple2e_char.rom`, fetched by CI's
`fetch-test-media` action. Missing or corrupt ROMs fail explicitly with the
fixture-fetch command, including in record mode.

**Measured 2026-11-19 — confirmed, not worth a change.** Temporary counters
in `drawScreenImage` (not committed), POM2 under Composite (OpenEmulator)
GPU on an Apple M1, an isolated `HOME`: `uploadScreenTexture()` runs every
frame. On DIX (3000 frames) the upload went unused on 1492 — the
full-graphics frames the shader presents; on an Applesoft text screen (2400
frames) it was used on every one (sharp text, mixed bands and the
shader-unavailable fallback present `screenTexture`). Its CPU-side cost:
84-118 µs a frame for 215-400 KB, about 0.5 % of a 20 ms PAL frame (the
GPU-side transfer was not timed). Skipping it would need the OE decision
moved ahead of the upload and a re-upload on every fallback, for a tenth
of a millisecond: kept as is. Reopen only on a profile where it shows.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1079);
[triage 2026-10-28](../audits/backlog-triage-2026-10-28.md). Earlier waves
already pinned FLASH, PAGE2/80STORE, rev-0 HGR+AN3,
IIe 80COL+HIRES+MIXED and Chat Mauve sub-modes.

<a id="display-008"></a>

## DISPLAY-008 — "Smooth" interpolated sub-pixel mode

**Priority:** 🧊 · **State:** Frozen — [triage 2026-10-28](../audits/backlog-triage-2026-10-28.md). Cosmetic, no oracle, no named need. Reopen on a user request naming the scaling it should fix.

**Acceptance criterion (after revalidation):** Provide an interpolation toggle and compare HGR/DHGR output at representative scales.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1110). Original dates and estimates are retained below.

- 🟢 **"Smooth" interpolated sub-pixel mode** — bilinear/Lanczos on
  HGR/DHGR, UI toggle. Inspired by microM8. *2 d.*

<a id="display-010"></a>

## DISPLAY-010 — CRT parity refinements vs OpenEmulator

**Priority:** 🟢 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): F4 is deliberate and F2 a phase-only difference; F6 (luminance-neutral row dim) is fix-on-report.

**Acceptance criterion (after revalidation):** Compare CRT defaults and mono rendering with the documented OpenEmulator reference; document deliberate visual choices.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1114). Original dates and estimates are retained below.

- 🟢 **CRT parity refinements vs OpenEmulator** — low-priority residuals from
  the 2026-05-30 video audit (detailed implementation notes →
  `docs/archive/video_parity_revalidation_2026-05-30.md` §4):
  - **F4** POM2 CRT defaults 0.25/0.5/0.4 (scanlines/mask/persistence) vs OE
    ~0.05/0.05/0 — *biggest visual gain*, OR own the "punchy" choice and
    document it (`NtscPostProcessor.h`).
  - **F2** cosine scanline → OE's **sin²** (keep the `scanAA` anti-moiré term).
  - **F6** row-dim mask ×0.7 ⚠ (make luminance-neutral, don't drop hard).
  - *(Non-items, documented: F1 clamp double > AppleWin float; F8/F9 amber/green
    tints assumed — optional "AppleWin-faithful" preset.)*

F7 was reproduced and fixed on 2026-10-12: HGR and GR monochrome now
retain all 560 signal dots, preserving orange/blue stripes and phosphor
decay. See [CHANGELOG](../../CHANGELOG.md) and `hgr_render_smoke`.

<a id="display-003"></a>

## DISPLAY-003 — Pure-analog signal-level composite pipeline

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1087). Original dates and estimates are retained below.

- 🧊 **Pure-analog signal-level composite pipeline** *(deferred, academic)* —
  IIR on the signal itself before demod, against today's 1-bit signal + FIR.
  *5-10 d.* Frozen: an alternative to a pipeline that is already core and green.
