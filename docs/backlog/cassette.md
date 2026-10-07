# Backlog — Cassette

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
| [CASSETTE-002](#cassette-002) | 🟡 | To verify | Bug hunt #17's cassette and Workstation residue |
| [CASSETTE-001](#cassette-001) | 🧊 | Frozen | Enriched WAV record/playback |

<a id="cassette-002"></a>

## CASSETTE-002 — Bug hunt #17's cassette and Workstation residue

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Revalidate 24-bit WAV, capture rewind and save-tape extension behavior separately; keep Workstation transport subject to its frozen scope.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1935). Original dates and estimates are retained below.

- 🟡 **Bug hunt #17's cassette and Workstation residue** *(2026-09-09;
  read, not fixed)*: 24-bit PCM WAV is refused (three lines in the sample
  decoder); a rewind across a live cassette capture keeps
  `recordedDurations` since cassette output never bumps the media epoch
  (a design call — the printer's output does); `--save-tape out.mp3`
  writes ACI bytes into a `.mp3` (`resolveSaveTapePath` appends only for
  no extension, `cli_runner` pins the resolver); nothing is wired to the
  Workstation Card's SCC (`frameCb_`/`receiveFrame` have no owner — two
  SCCs back to back is the end-to-end shape); the AppleShare disk never
  reaches the card at all (zero `$Cn00` accesses over 120 M instructions,
  byte-identical to a run with no card), so "boots, does not netboot" is
  the ProDOS/SmartPort boot of that image, upstream of the card. *1 d.*
  The Workstation half overlaps [CARDS-002](cards.md#cards-002) step 3
  (host-side LocalTalk endpoint), which is frozen; that entry is its home.

<a id="cassette-001"></a>

## CASSETTE-001 — Enriched WAV record/playback

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1830). Original dates and estimates are retained below.

- 🧊 **Enriched WAV record/playback** — POM2 supports .wav; missing
  analog tape filtering (hiss, drop-out), VU-meter, timecode.
  MAME refs `apple2.cpp` cassette. *2 d.*
