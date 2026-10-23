# Backlog — Integration validation

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
| [VALIDATION-001](#validation-001) | 🟡 | To verify | Disk II opposing-magnet response on a real //c |
| [VALIDATION-003](#validation-003) | 🟡 | Partial: gameplay observed; broader protection evidence remains | Spiradisc / RWTS18 |
| [VALIDATION-004](#validation-004) | 🟢 | Blocked (oracle) | DOS 3.1 / 3.1.1 `]` anomaly |
| [VALIDATION-005](#validation-005) | 🟢 | To verify | A2 File Cmd one-in-three GUI-only DHGR failure |

<a id="validation-001"></a>

## VALIDATION-001 — Disk II opposing-magnet response on a real //c

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Record whether a real //c head moves under the cited opposing-magnet sequence while the internal motor coasts.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-131). Original dates and estimates are retained below.

Closed the same day (CHANGELOG, "The hunt's leftovers"). One thing only real
hardware can settle: POM2 now cancels a Disk II step whose opposing magnet
comes on within 256 CPU cycles, which is what keeps the //c's SmartPort
addressing pattern (`$CA80`: PH1, then PH3 four cycles later) from moving the
internal head. A real //c with the internal motor still coasting would show
whether its head moves there; if it does, the cancel is the divergence to
remove (`DiskIICard::seekPhaseW`, `kStepperResponseCycles`).

<a id="validation-003"></a>

## VALIDATION-003 — Spiradisc / RWTS18

**Priority:** 🟡 · **State:** Partial integration evidence recorded 2026-10-12; candidate rerun and broader coverage remain.

**Acceptance criterion (after revalidation):** Run the named protected titles on real WOZ images and record weak-bit/spiral behavior against their expected boot result.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2299). Original dates and estimates are retained below.

- 🟡 **Spiradisc / RWTS18** (*Captain Goodnight*, *Prince of Persia*) — spiral
  tracking + weak bits to validate on real WOZ images. → `Gap #9/#10`. The
  **model** landed 2026-09-07: a flux gap past MAME's 16 µs
  amplifier-freakout time now produces one hash-drawn blip per zone per
  revolution, so a weak-bit protection is a coin toss instead of a constant,
  and a track with no flux at all answers read-amplifier noise instead of
  hanging the guest's `LDA $C08C,X / BPL`.

The [protected-disk report](../audits/protected-disks-2026-10-12.md) records
Captain Goodnight side A/B reaching gameplay and Prince of Persia side A
reaching its first dungeon room on WOZ captures. The Captain capture includes
half-track positions; the selected PoP capture has whole tracks only, so its
boot does not validate quarter-track behavior. Later levels/PoP side B,
completion, other captures and physical-hardware comparison remain unverified.
These are dated manual observations, not automated protection goldens.

<a id="validation-004"></a>

## VALIDATION-004 — DOS 3.1 / 3.1.1 `]` anomaly

**Priority:** 🟢 · **State:** Blocked — needs MAME or real hardware to arbitrate.

**Acceptance criterion:** Boot both masters on an oracle; record whether the `]` loop with the head swept to half-track 66 is the disk (an Integer-BASIC HELLO on an Applesoft ][+) or POM2.

**Evidence:** [original report](../archive/todo-2026-10-05.md), bug hunt #3 (2026-09-07). Every DOS 3.2 image boots. `DOS13SEC.DSK` is separately known to carry a hand-modified boot0 — a disk defect. Recovered 2026-10-07.

<a id="validation-005"></a>

## VALIDATION-005 — A2 File Cmd: one-in-three GUI-only DHGR failure

**Priority:** 🟢 · **State:** To verify on current main.

**Acceptance criterion:** Reproduce or refute `DHGR.RAW: not an image` followed by a corrupt HGR in the GUI with A2 File Cmd 0.9.5 (`hdv/A2FILECMD-PRODOS-XL-65C02-enhanced-0.9.5.2mg`).

**Evidence:** [original report](../archive/todo-2026-10-05.md), "Retour A2FC 2026-09-09": seen in the GUI only, once in three, on 0.6.8; never headless, card or not; same bytes in RAM. Points after `Apple2Display::render` or at GUI timing. Recovered 2026-10-07.

