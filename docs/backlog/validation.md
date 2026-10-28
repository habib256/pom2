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
| [VALIDATION-006](#validation-006) | 🟡 | Ready | Headless compatibility corpus with a published pass rate |
| [VALIDATION-001](#validation-001) | 🟡 | Needs hardware oracle | Disk II opposing-magnet response on a real //c |
| [VALIDATION-003](#validation-003) | 🟡 | Partial: make repeatable | Spiradisc / RWTS18 |
| [VALIDATION-004](#validation-004) | 🟢 | Needs oracle: hardware or MAME romsets | DOS 3.1 / 3.1.1 `]` anomaly |
| [VALIDATION-005](#validation-005) | 🟢 | Unknown: GUI-only, retest on 1.0 | A2 File Cmd one-in-three GUI-only DHGR failure |

<a id="validation-006"></a>

## VALIDATION-006 — Headless compatibility corpus with a published pass rate

**Priority:** 🟡 · **State:** Ready ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)).

**Acceptance criterion:** One command boots every title of a declared
manifest headless on its profile, records a screen hash after a fixed number
of frames (plus optional scripted keys), compares with the stored expectation
and prints a pass percentage. It runs from CI or by hand on a release
candidate, and the result for each version is recorded in `docs/audits/`.
Titles whose media are not tracked are skipped with a reason, not failed.

**Evidence:** today compatibility is shown title by title (`docs/test_corpus.md`,
`docs/audits/protected-disks-2026-10-12.md`, the DIX probes); there is no
number to compare between versions, and VALIDATION-003's captures live in a
scratch directory.

- Start with **DIX** (Core), then `docs/test_corpus.md`, then the
  protected-disks set once VALIDATION-003 records its WOZ SHA-256s.
- Reuse what exists: `pom2_headless`, `hdv_boot_dump` and the DIX probes
  (`cmake --build build --target probes`), the AI control server's
  `/screen` hash.
- Copy any tracked image before booting it with writes enabled (CLAUDE.md,
  media write default).

<a id="validation-001"></a>

## VALIDATION-001 — Disk II opposing-magnet response on a real //c

**Priority:** 🟡 · **State:** Needs a hardware oracle ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): a real //c, PH1→PH3 gaps of 4…512 cycles while the motor coasts.

**Acceptance criterion (after revalidation):** Record whether a real //c head moves under the cited opposing-magnet sequence while the internal motor coasts.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-131). Original dates and estimates are retained below.

Closed the same day (CHANGELOG, "The hunt's leftovers"). One thing only real
hardware can settle: POM2 now cancels a Disk II step whose opposing magnet
comes on within 256 CPU cycles, which is what keeps the //c's SmartPort
addressing pattern (`$CA80`: PH1, then PH3 four cycles later) from moving the
internal head. A real //c with the internal motor still coasting would show
whether its head moves there; if it does, the cancel is the divergence to
remove (`DiskIICard::seekPhaseW`, `kStepperResponseCycles`).

**2026-10-27 research.** MAME moves the head at once on PH1 and leaves it
there when PH3 joins (`floppy.cpp` `seek_phase_w`: an opposing pair is
`default: return`), so in MAME the //c's SmartPort addressing does step the
coasting internal head — MAME has no mechanical inertia. AppleWin defers
every step 10 cycles and then evaluates the magnets as they stand
(`Disk.cpp` `ControlStepper` / `ControlStepperDeferred`, GH#1110 citing
Sather UTAIIe pp. 9-12/13), so PH1 then PH3 four cycles later moves
nothing, as POM2's cancel does. BMOW confirms the phase lines are shared by
every drive on the port and that SmartPort enable is PH1+PH3 with PH2 low.
No source gives a measured minimum on-time; the cancel stays, pending a
real //c.

<a id="validation-003"></a>

## VALIDATION-003 — Spiradisc / RWTS18

**Priority:** 🟡 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): milestones reached on 2026-10-27; record WOZ SHA-256s and probe commands so VALIDATION-006 can replay them.

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
Rerun on 2026-10-27 after the Disk II stock-time change: the same
milestones reproduced (audit, "Rerun 2026-10-27").

<a id="validation-004"></a>

## VALIDATION-004 — DOS 3.1 / 3.1.1 `]` anomaly

**Priority:** 🟢 · **State:** Needs an oracle ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): real ][+ hardware, or MAME once `apple2p` romsets are installed locally.

**Acceptance criterion:** Boot both masters on an oracle; record whether the `]` loop with the head swept to half-track 66 is the disk (an Integer-BASIC HELLO on an Applesoft ][+) or POM2.

**Evidence:** [original report](../archive/todo-2026-10-05.md), bug hunt #3 (2026-09-07). Every DOS 3.2 image boots. `DOS13SEC.DSK` is separately known to carry a hand-modified boot0 — a disk defect. Recovered 2026-10-07.

<a id="validation-005"></a>

## VALIDATION-005 — A2 File Cmd: one-in-three GUI-only DHGR failure

**Priority:** 🟢 · **State:** Unknown ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): six GUI runs on a copy of the A2 File Cmd 1.0 image; close as stale if it does not reproduce.

**Acceptance criterion:** Reproduce or refute `DHGR.RAW: not an image` followed by a corrupt HGR in the GUI with A2 File Cmd 0.9.5 (`hdv/A2FILECMD-PRODOS-XL-65C02-enhanced-0.9.5.2mg`).

**Evidence:** [original report](../archive/todo-2026-10-05.md), "Retour A2FC 2026-09-09": seen in the GUI only, once in three, on 0.6.8; never headless, card or not; same bytes in RAM. Points after `Apple2Display::render` or at GUI timing. Recovered 2026-10-07.

