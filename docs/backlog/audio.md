# Backlog — Audio

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
| [AUDIO-011](#audio-011) | 🟠 | To verify | Bug hunt #16's speech residue |
| [AUDIO-001](#audio-001) | 🟡 | To verify | Echo+ still cuts mute in one buffer and allocates on the audio thread. |
| [AUDIO-002](#audio-002) | 🟡 | To verify | Sound II stops `fillAudioTimed` once the mute ramp reaches ~0. |
| [AUDIO-003](#audio-003) | 🟡 | To verify | Compare Mockingboard output level with MAME |
| [AUDIO-005](#audio-005) | 🟢 | To verify | Analog output stage. |
| [AUDIO-006](#audio-006) | 🟢 | To verify | Mutex contention. |
| [AUDIO-007](#audio-007) | 🟢 | To verify | Mute drops the queue. |
| [AUDIO-010](#audio-010) | 🟢 | To verify | AY Port A read mask by DDR |
| [AUDIO-004](#audio-004) | 🧊 | Frozen | ayumi-grade resampling |
| [AUDIO-008](#audio-008) | 🧊 | Frozen | 8-bit DAC (Marczewski) |
| [AUDIO-009](#audio-009) | 🧊 | Frozen | Passport MIDI Music Card |

<a id="audio-011"></a>

## AUDIO-011 — Bug hunt #16's speech residue

**Priority:** 🟠 · **State:** To verify.

**Acceptance criterion (after revalidation):** Verify SSI duration, reset/IRQ and Phasor speech claims independently against a stated oracle; do not schedule the frozen TMS card rewrite implicitly.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1952). Original dates and estimates are retained below.

- 🟠 **Bug hunt #16's speech residue** *(2026-09-09; read, not fixed —
  each a design change, not a patch)*: (1) the SSI263 duration formula
  `ms = ((16-rate)*4096/1023)*(4-dur)` is a mis-citation — it exists once in
  AppleWin inside `#if LOG_SSI263B`, a debug line; the real duration is the
  PCM length (40 / 120 / 153 ms) and `UpdateAccurateLength` scales by
  1/(DUR+1), the inverse of `(4−DUR)`; at rate 15 / dur 3 POM2 plays 88 of
  2 656 samples. (2) Power-on state: AppleWin observed `$C0` (mode 11) and
  FILFREQ silence; POM2 resets to mode 00 = IRQ disabled, so a detection
  routine that powers up with one CTL=0 and waits for the phoneme IRQ
  (mb-audit, Willy Byte) hangs — `ssi263_smoke::testResetState` encodes the
  current values, so this is a ruling; AppleWin also latches `enableInts`
  on the CTL edge and repeats a finished phoneme with an IRQ each time.
  (3) `EchoPlusTMS5220Card` contradicts MAME's `a2bus_echoplus_device`:
  the TMS5220 is at `$C0n0` (idle `$7F`) and the two AYs sit behind a VIA1
  with PB3/PB4 as chip selects — a card rewrite. (4) The Phasor carries no
  SSI263 where the real card has two (`$Cn40`/`$Cn20`). *2-3 d for the lot.*

<a id="audio-001"></a>

## AUDIO-001 — Echo+ still cuts mute in one buffer and allocates on the audio thread.

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Render Echo mute transitions through a 5 ms gain ramp with no callback allocation after preparation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1159). Original dates and estimates are retained below.

- 🟡 **Echo+ still cuts mute in one buffer and allocates on the audio
  thread.** `EchoPlusCard::fillAudioBuffer` (`EchoPlusCard.cpp:49-53`)
  returns immediately when muted — no 5 ms `gainRamp`, unlike MB/Phasor —
  and `scratch.resize` still lives in the callback (`:38-41`) where
  Mockingboard reserved `speechScratch` in `setSampleRate`. Hunt #19 closed
  those two on the cards that have a PSG; Echo+ has the same comments and
  not the reserve.
  *Revalidated 2026-10-06:* still true; the early return is now
  `src/EchoPlusCard.cpp:53` and the `scratch.resize` `:61-62`.

<a id="audio-002"></a>

## AUDIO-002 — Sound II stops `fillAudioTimed` once the mute ramp reaches ~0.

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Keep the SSI playback timeline advancing while muted; unmute must not replay a burst of queued phonemes.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1166). Original dates and estimates are retained below.

- 🟡 **Sound II stops `fillAudioTimed` once the mute ramp reaches ~0.**
  `Mockingboard.cpp:580-583` returns after the ramp, so `ctlEvents_` is no
  longer drained. The CPU keeps `queuePlaybackEvent`; the ring fills, then
  `clear` + `aPrimed_=false`. Unmute dumps a burst of phonemes at sample 0
  — the same class of dropout hunt #20 closed for the PCM cursor. The
  mute tests cover MB/Phasor amplitude, not the SSI timeline.
  *Revalidated 2026-10-06:* the muted-and-faded early return is still there,
  now at `src/Mockingboard.cpp:568-573`, ahead of the `fillAudioTimed` call
  (`:596`).

<a id="audio-003"></a>

## AUDIO-003 — Compare Mockingboard output level with MAME

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Measure the same AY register stream through both output chains and record gain and bass response relative to the speaker.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1376). Original dates and estimates are retained below.

Raised by "il reste des choses à améliorer au niveau des basses". The
2026-08-02 sweep measured the whole card render chain against MAME and found
it already correct down to 27.5 Hz — volume table within 0.0007 of Westcott's
data, linear channel sum matching `a2mockingboard.cpp`, box integrator with a
sinc gain of 1.0000 below 100 Hz, no stereo cancellation. The one real gap
was the DC blocker (1-pole where MAME uses a 2-pole Butterworth), now ported,
worth ≤0.8 dB below 80 Hz.

That is almost certainly **too small to be what is actually heard**, so the
remaining suspect is level rather than frequency response: POM2 normalises
the per-side chip sum by `/3` (Mockingboard) and `/6` (Phasor), while MAME
routes each AY channel through `add_route(ALL_OUTPUTS, "speaker", 0.5, ch)`.
Those are different scalings, and a card that sits low against the speaker
and disk channels reads as thin. Wants a numbers-first comparison of POM2's
end-to-end output level against MAME's for the same register stream, not more
tuning inside `AyPsgSynth.h`. → `CHANGELOG.md` 2026-08-02.

<a id="audio-005"></a>

## AUDIO-005 — Analog output stage.

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Require a hardware waveform reference before choosing an analog-stage model; compare the approved model with that reference.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1442). Original dates and estimates are retained below.

- 🟢 **Analog output stage.** The real Sweet Micro board's LM386 pair
  makes the output *triangular*, not square (deater's scope capture:
  `deater.net/weave/vmwprod/chiptune/mock_problem/`). No emulator models
  it and there is no MAME oracle, so it would have to be an off-by-default
  toggle labelled non-authoritative — and only after band-limiting, since
  a low-pass over an aliased signal muffles rather than removes.

<a id="audio-006"></a>

## AUDIO-006 — Mutex contention.

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Profile contention under independent CPU/audio clocks and show any approved handoff avoids underruns while preserving event order.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1448). Original dates and estimates are retained below.

- 🟢 **Mutex contention.** SPSC handoff *if* a profile still shows it after
  the GUI TSan half ([ARCH-003](architecture-tooling.md#arch-003)). `advanceCycles` takes the card mutex on every
  emulated instruction (~1 M/s) and the realtime audio callback needs the
  same one, holding it across the whole SSI263 render on Sound II.
  Classic priority inversion; wants an SPSC handoff.

<a id="audio-007"></a>

## AUDIO-007 — Mute drops the queue.

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Verify muted register updates survive unmute and volume changes remain ramped; archive the report if already covered.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1453). Original dates and estimates are retained below.

- 🟢 **Mute drops the queue.** The `isMuted` early-out returns after
  `pending` has been drained, so writes are lost while muted and `vol`
  changes are applied as a hard step at buffer boundaries (click).

<a id="audio-010"></a>

## AUDIO-010 — AY Port A read mask by DDR

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Validate AY port reads for input/output DDR combinations against the chosen chip reference.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1461). Original dates and estimates are retained below.

- 🟢 **AY Port A read mask by DDR** (R14/R15) — academic.

<a id="audio-004"></a>

## AUDIO-004 — ayumi-grade resampling

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1433). Original dates and estimates are retained below.

- 🧊 **ayumi-grade resampling** (native clock/8 → 8× quadratic interp →
  192-tap FIR decimation + moving-average DC filter,
  `true-grue/ayumi`, MIT). Strictly better than the box filter and what
  chiptune players use; ~8× the inner iterations plus ~96 MACs per sample
  per channel, ~192 doubles/channel of rewind state and ~2 ms group
  delay — a real cost on the **WASM** target. Only worth it if listening
  shows the box filter is insufficient. Note this would be a deliberate
  departure from "MAME = source of truth" for the audio path.
  Frozen by the scope ruling.

<a id="audio-008"></a>

## AUDIO-008 — 8-bit DAC (Marczewski)

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1457). Original dates and estimates are retained below.

- 🧊 **8-bit DAC (Marczewski)** — 8-bit slot latch → R-2R DAC. Niche
  demos (Music Studio, trackers). AppleWin refs `Card::CT_DX1`. *1 d.*

<a id="audio-009"></a>

## AUDIO-009 — Passport MIDI Music Card

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1459). Original dates and estimates are retained below.

- 🧊 **Passport MIDI Music Card** — 6840 + 6850, Master Tracks Pro /
  Performer. MAME refs `mc6840.cpp` + `acia6850.cpp`. *3 d.*
