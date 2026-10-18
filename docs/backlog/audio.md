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
| [AUDIO-003](#audio-003) | 🟡 | Partial: nominal single-tone balance calibrated | Compare Mockingboard output level with MAME |
| [AUDIO-005](#audio-005) | 🟢 | To verify | Analog output stage. |
| [AUDIO-006](#audio-006) | 🟢 | To verify | Mutex contention. |
| [AUDIO-007](#audio-007) | 🟢 | To verify | Mute drops the queue. |
| [AUDIO-010](#audio-010) | 🟢 | To verify | AY Port A read mask by DDR |
| [AUDIO-004](#audio-004) | 🧊 | Frozen | ayumi-grade resampling |
| [AUDIO-008](#audio-008) | 🧊 | Frozen | 8-bit DAC (Marczewski) |
| [AUDIO-009](#audio-009) | 🧊 | Frozen | Passport MIDI Music Card |

<a id="audio-011"></a>

## AUDIO-011 — Bug hunt #16's speech residue

**Priority:** 🟠 · **State:** Partly resolved (2026-10-17); remaining features below.

**Acceptance criterion:** Verify the remaining SSI waveform/reset and
Phasor speech behavior against a stated oracle; do not schedule the frozen
TMS card rewrite implicitly.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1952),
[DEV SSI263](../../DEV.md#ssi263--echo-street-electronics), CHANGELOG 2026-10-17.

Resolved against AppleWin `6c579da5bd94`: power-on DURPHON=$C0/FILFREQ=$FF,
CTL-latched mode/IRQ selection, repeated requests, Sound II's SSI enable
gate on VIA2.CA1, silent playback continuity and PCM-derived durations.
Both timer and renderer now use the primary `Update()` read/average/skip
loop. The old debug-log formula and the differing debugger fallback are
not timing oracles. All 64 codes, four DUR values, two function classes and
three CPU clocks have independent waveform/duration checks; future CTL
mode events and mid-phoneme acceleration changes are pinned too.
Sound II now preserves its disconnected SSI263 across bus RESET while
clearing VIA/AY state, including pending audio events and CA1 request
behavior. Cold boots use a separate power-cycle hook and clear speech.
The separate Phasor PSG pass corrected native GAL selection/latch/read
routing, full-page VIA decode, Echo+ single-VIA/stereo operation and host
mute continuity, with AppleWin hardware notes and timeline regressions.

Remaining:

- SSI RATEINF/inflection, articulation and analog waveform synthesis are
  not modelled by the fixed PCM table. Phasor now uses the SSI263AP reset
  contract from AppleWin; the older SSI263P reset quirk and Cricket reset
  behavior still need a hardware/firmware comparison.
- Phasor now implements both SSI263APs, address aliases, native direct
  IRQ/compatibility CA1 routing, cycle-stamped audio and PHS v3 rewind.
  Its mono speech sum with headroom is a mixing approximation; physical
  output levels/routing still need independent hardware measurements.
- Hidden/frozen `echoplus_tms` remains a provisional, silent TMS5220/AY
  scaffold whose decode needs a separate MAME/schematic-based rewrite.

<a id="audio-001"></a>
<a id="audio-002"></a>

AUDIO-001 and AUDIO-002 were reproduced and resolved on 2026-10-17:
mute preserves speech playback time on Cricket/Echo and Sound II, and the
Cricket/Echo callback applies a 5 ms gain ramp without scratch allocation.
See [CHANGELOG](../../CHANGELOG.md#2026-10-17--speech-keeps-its-place-while-muted)
and `ssi263_speech_timeline` for the audible-reference regression.

<a id="audio-003"></a>

## AUDIO-003 — Compare Mockingboard output level with MAME

**Priority:** 🟡 · **State:** Partial (2026-10-18); reference recording still needed.

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

**2026-10-18 calibration:** The original "card sits low" diagnosis was not
confirmed for a single full-scale tone. At shipped defaults, POM2's AY and
speaker were almost equal in RMS. MAME's Apple II configuration routes AY
channels at 0.5 and the speaker at 0.4; its AY resistor model (1 kOhm load,
Westcott table) gives levels 0.0590383 and 0.2987533 at amplitudes 0 and
15. A 50%-duty tone therefore gives an AY/speaker RMS ratio of 0.2996. The
new `audio_stereo` check renders both POM2 chains at their defaults and
measures 0.2940. The speaker nominal amplitude is 0.4; new Mockingboard
instances default to 0.35, while saved volume values remain authoritative.
`speaker_audio_quality` and `mockingboard_audio_quality` remain green.

The comparison is against MAME's published signal model and routes, not a
captured MAME WAV: the local MAME installation lacks the Apple IIe ROM set.
Keep the broader item open for a same-program recording and a listening or
hardware check before changing bass response or speech balance.

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
