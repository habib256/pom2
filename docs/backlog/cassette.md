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
| [CASSETTE-002](#cassette-002) | 🟡 | Decision pending | A rewind across a live cassette recording |
| [CASSETTE-001](#cassette-001) | 🧊 | Frozen | Enriched WAV record/playback |

<a id="cassette-002"></a>

## CASSETTE-002 — A rewind across a live cassette recording

**Priority:** 🟡 · **State:** Decision pending.

**Acceptance criterion:** Decide whether cassette output is irreversible
like printed output; implement and pin whichever rule is chosen.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1935).
A rewind across a live cassette capture keeps `recordedDurations`, since
cassette output never bumps the media epoch — while the printer's output
does, and clears the rewind ring. Either the tape is like paper (bump the
epoch, the ring goes) or the capture is part of the machine (truncate it on
a rewind). v7 sessions already carry the in-progress recording.

Closed parts: 24- and 32-bit integer PCM WAV now load (2026-10-24, pinned in
`cassette_hostile`); the CLI `--save-tape` suffix was settled on 2026-10-20
(`resolveSaveTapePath`, covered by `cli_runner`); the Workstation Card half
lives in [CARDS-002](cards.md#cards-002), which is frozen.

<a id="cassette-001"></a>

## CASSETTE-001 — Enriched WAV record/playback

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1830). Original dates and estimates are retained below.

- 🧊 **Enriched WAV record/playback** — POM2 supports .wav; missing
  analog tape filtering (hiss, drop-out), VU-meter, timecode.
  MAME refs `apple2.cpp` cassette. *2 d.*
