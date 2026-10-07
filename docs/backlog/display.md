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
| [DISPLAY-001](#display-001) | 🟡 | To verify | VidHD on Apple IIe — SHR display support. |
| [DISPLAY-011](#display-011) | 🟡 | To verify | Le Chat Mauve — what is left after P0-P2 |
| [DISPLAY-012](#display-012) | 🟡 | To verify | Bug hunt #15's display residue |
| [DISPLAY-002](#display-002) | 🟢 | To verify | Golden coverage gaps |
| [DISPLAY-004](#display-004) | 🟢 | To verify | Beam-racing residuals |
| [DISPLAY-005](#display-005) | 🟢 | To verify | Mid-scanline split residuals |
| [DISPLAY-006](#display-006) | 🟢 | To verify | PAL residuals |
| [DISPLAY-007](#display-007) | 🟢 | To verify | Unidirectional mid-frame page split renders full-page |
| [DISPLAY-008](#display-008) | 🟢 | To verify | "Smooth" interpolated sub-pixel mode |
| [DISPLAY-009](#display-009) | 🟢 | To verify | DHGR mono 1-px alignment + floating-TTL `empty_words` + per-scanline mode switch |
| [DISPLAY-010](#display-010) | 🟢 | To verify | CRT parity refinements vs OpenEmulator |
| [DISPLAY-003](#display-003) | 🧊 | Frozen | Pure-analog signal-level composite pipeline |

<a id="display-001"></a>

## DISPLAY-001 — VidHD on Apple IIe — SHR display support.

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Render VidHD SHR on the IIe with documented register behavior and a reference image or hardware-derived test.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1072). Original dates and estimates are retained below.

- [ ] **VidHD on Apple IIe — SHR display support.** Model the VidHD card's
  display registers and video-memory observation so IIe software can display
  Super Hi-Res images. This is a display extension: VidHD does not add a
  65816 processor or the IIgs Toolbox and does not turn a IIe into a IIgs.
  Displaying an SHR image alone does not enable execution of IIgs
  applications. Reference: [VidHD Manual 1.2](https://www.callapple.org/docs/vidhd/VidHD_Manual_1.2.pdf).

<a id="display-011"></a>

## DISPLAY-011 — Le Chat Mauve — what is left after P0-P2

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Verify the remaining supported Feline/Eve behavior with their goldens; keep RVB and adapter work subject to the frozen scope.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1126). Original dates and estimates are retained below.

- 🟡 **Le Chat Mauve — what is left after P0-P2** (landed 2026-09-01; the
  model as built is `docs/chatmauve_plan.md` § 2.1). **Féline and Eve are
  core** — dot-exact against AppleWin's hardware-validated oracles, two golden
  suites (`purplesoft_eve_screens`, `chatmauve_dot_rules`), the Eve's sixteen
  switches + CPREG auto-write + table IX-1 corrected four times by Purplesoft's
  own code. `rvb` and the //c-adapter quirk are 🧊 **frozen**, both gated on
  sources that have never surfaced (P4's manual; P5's silicium.org thread
  behind a proof-of-work wall `WebFetch` cannot pass). **P3 is closed as
  bounded** (2026-09-02): the PLA is a dot-stream router / cell assembler, NOT
  the colour decoder — reopen only on a schematic or a board trace.
  Remaining and not frozen: **P6's residuals** (the Eve's `$C0Bx` as loggable
  events, the exact in-cell dot position, the TTL-RGBI palette option, DIX /
  PoP golden screens — the first rung, a beam-racing mode latch, landed as
  `chatmauve_latch_split`); the **Extasie and Arlequin goldens** (both boot
  headless now — pin the Arlequin demo-menu screen and an Extasie editor
  screen; Eve Leonard still to find); the **three Féline trim pots** (R/G/B
  gain, manual p. 13), which belong with the `NtscParams::rgbBandwidthMHz`
  pre-pass that gave the card its second connector on 2026-09-04; and **P7's
  last doc step** — fold § 3.4's corrected table back into § 3 prose, and
  rewrite the README's Chat Mauve paragraph.
  Still parked alongside: **Video-7
  AppleColor RGB** (its 160×192 chunky mode and F/B text are the only things
  the plan's Féline decoder does not cover), **Color killer Rev 1**,
  **Strapping RAM 4K→48K**.

<a id="display-012"></a>

## DISPLAY-012 — Bug hunt #15's display residue

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Compare FLASH phase, Videx glyph bits and 80-column color phase with the cited references and current dumps.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1984). Original dates and estimates are retained below.

- 🟡 **Bug hunt #15's display residue** *(2026-09-09; read, not fixed)*:
  the II/II+ FLASH range blinks in antiphase with MAME's model::II (and
  with POM2's own //e cursor) — cosmetic; `CharRomDump.h` and DEV say the
  Videx dump "never sets bit 7" while the shipped file marks all of
  $40-$7F, so that sentence and the offset branch are stale; 80-column
  TEXT under the OE/AppleWin CPU demods uses phase 0 where MAME's
  `is_80_column` term would apply (no colour oracle, and the term is
  per-frame on a beam-raced split). *2 h.*

<a id="display-002"></a>

## DISPLAY-002 — Golden coverage gaps

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Add the missing glyph and PAL split coverage; separately measure whether the fallback-framebuffer upload remains unnecessary.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1079). Original dates and estimates are retained below.

- 🟢 **Golden coverage gaps** (from the 2026-07-12 audit; mostly closed
  2026-07-12 wave 4, table 112 → 164 pins — flash-on phase, PAGE2/80STORE,
  rev-0 HGR+AN3, IIe 80COL+HIRES+MIXED without DHGR, Chat Mauve sub-modes
  all hash-frozen: `iie/text40flash`, `text40page2`, `hgrpage2`,
  `hgr80store2`, `hgran3`, `hgr80colmix`, `textcolorcm`). Remaining:
  ALTCHAR/mousetext + char-ROM glyphs (need a user ROM), PAL beam-raced
  splits (stay behavioural). Also: OE-GPU uploads the unused ~430 KB
  fallback framebuffer every frame (minor perf).

<a id="display-004"></a>

## DISPLAY-004 — Beam-racing residuals

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Validate HGR/DHGR phase changes and lo-res splits against a beam-timed reference.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1090). Original dates and estimates are retained below.

- 🟢 **Beam-racing residuals** — `signalPhaseOffset_` stays a per-frame
  constant, so a mid-frame HGR↔DHGR split is approximated; lo-res clips at
  block-row (4 lines), like the RGBA path.

<a id="display-005"></a>

## DISPLAY-005 — Mid-scanline split residuals

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Define the mixed-width scanline limit and record the separately gated POM1 back-port result.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1093). Original dates and estimates are retained below.

- 🟢 **Mid-scanline split residuals** — 40-col (280) + 80-col (560) mixed on
  the same line is undefined (separate `frame`/`frame80` buffers, scoped
  out). The exact transition cycle at character-clock is no longer a "later
  refinement": DIX showed it as a one-cell error each side. That symptom is
  closed — it was the 6522 T1 read-back bias plus a per-kind column offset,
  both fixed 2026-09-02 (`CHANGELOG.md` of that day; the offset was narrowed
  to AN3/DHGR the same day after it slid MAD EFFECT left). **Back-port to POM1** next (gated: LORES+TEXT rendering on
  GEN2 — HGR-only today — + HBLANK flag Phase 2 per Bernie's spec).

<a id="display-006"></a>

## DISPLAY-006 — PAL residuals

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Separate intentional device pitch limits from browser 50 Hz pacing; validate each approved remaining timing change.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1101). Original dates and estimates are retained below.

- 🟢 **PAL residuals** — device generator clocks (AY/IWM/SSI263) stay at the
  NTSC nominal (0.7 % delta = inaudible pitch, deliberately not retimed;
  speaker + cassette realtime audio ARE retimed, their queues starve
  audibly otherwise); WASM pacing (RAF 60 Hz) not yet switched to 50 Hz;
  manual NTSC/PAL toggle + auto-PAL when a Chat Mauve card is plugged (the
  two PAL profiles already cover the use case).

**Revalidated 2026-10-06 — premise partly false.** The AY and 6522 clocks
no longer stay at the NTSC nominal: the Mockingboard / Phasor derive them
from the live slot-bus clock, which follows PAL (`setStandardClock` →
`SlotBusClock.h`; `src/Mockingboard.cpp:540-558`, fed by
`src/EmulationController.cpp:82`), and SSI263 phoneme lengths follow the
live CPU clock (CHANGELOG *2026-09-29 — Bug hunt, round three*). The IWM
keeps its own 7.16 MHz crystal by design. The WASM pacing and the manual
NTSC/PAL toggle were not rechecked.

<a id="display-007"></a>

## DISPLAY-007 — Unidirectional mid-frame page split renders full-page

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Render the two sides of a mid-frame page switch correctly in a reproducing test.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1107). Original dates and estimates are retained below.

- 🟢 **Unidirectional mid-frame page split renders full-page** — assumed
  limit inherited from the DROL page-flip fix; the true remedy is
  incremental per-scanline rendering, MAME-style. → `CHANGELOG.md`.

<a id="display-008"></a>

## DISPLAY-008 — "Smooth" interpolated sub-pixel mode

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Provide an interpolation toggle and compare HGR/DHGR output at representative scales.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1110). Original dates and estimates are retained below.

- 🟢 **"Smooth" interpolated sub-pixel mode** — bilinear/Lanczos on
  HGR/DHGR, UI toggle. Inspired by microM8. *2 d.*

<a id="display-009"></a>

## DISPLAY-009 — DHGR mono 1-px alignment + floating-TTL `empty_words` + per-scanline mode switch

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Reproduce each cited mono-alignment, floating-TTL and scanline issue independently; retain only failures on current main.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1112). Original dates and estimates are retained below.

- 🟢 **DHGR mono 1-px alignment + floating-TTL `empty_words` +
  per-scanline mode switch** — cosmetic / out-of-bounds.

<a id="display-010"></a>

## DISPLAY-010 — CRT parity refinements vs OpenEmulator

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Compare CRT defaults and mono rendering with the documented OpenEmulator reference; document deliberate visual choices.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1114). Original dates and estimates are retained below.

- 🟢 **CRT parity refinements vs OpenEmulator** — low-priority residuals from
  the 2026-05-30 video audit (detailed implementation notes →
  `docs/archive/video_parity_revalidation_2026-05-30.md` §4):
  - **F4** POM2 CRT defaults 0.25/0.5/0.4 (scanlines/mask/persistence) vs OE
    ~0.05/0.05/0 — *biggest visual gain*, OR own the "punchy" choice and
    document it (`NtscPostProcessor.h`).
  - **F2** cosine scanline → OE's **sin²** (keep the `scanAA` anti-moiré term).
  - **F7** HGR mono: 280 px average / 3 levels → **560 binary** (copy of the
    DHGR-mono loop already shipped).
  - **F6** row-dim mask ×0.7 ⚠ (make luminance-neutral, don't drop hard).
  - *(Non-items, documented: F1 clamp double > AppleWin float; F8/F9 amber/green
    tints assumed — optional "AppleWin-faithful" preset.)*

<a id="display-003"></a>

## DISPLAY-003 — Pure-analog signal-level composite pipeline

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1087). Original dates and estimates are retained below.

- 🧊 **Pure-analog signal-level composite pipeline** *(deferred, academic)* —
  IIR on the signal itself before demod, against today's 1-bit signal + FIR.
  *5-10 d.* Frozen: an alternative to a pipeline that is already core and green.
