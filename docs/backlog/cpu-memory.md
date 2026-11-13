# Backlog — CPU and memory

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
| [CPU-001](#cpu-001) | 🟡 | In progress: design, no code yet | Optional cycle-stepped 6502 core |
| [MEMORY-002](#memory-002) | 🟡 | Confirmed open; revalidated 2026-11-07 | `Memory::memRead` hot path |
| [MEMORY-003](#memory-003) | 🟢 | Needs hardware oracle (bank) | Language Card power-on bank; the Apple card's own F8 ROM |

<a id="cpu-001"></a>

## CPU-001 — Optional cycle-stepped 6502 core

**Priority:** 🟡 · **State:** In progress — design only, no second core in
`src/` yet ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md); listed
under "Core — in progress" in TODO.md).

**Acceptance criterion:** A second core, selected per profile or by a setting,
issues one bus access per cycle in silicon order and samples IRQ/NMI in the
penultimate cycle. It passes the Tom Harte 65x02 corpus on **per-cycle bus
ordering** (address, data, direction — not only final state and cycle count),
both Klaus suites, and `display_golden_hash`; the existing fast core keeps
`bench_identity` and `pom2_bench` unchanged; and at least one device sync shim
is retired under the new core with a mutation control (first candidate: the
Mockingboard's `syncToCpuCycle()+1`).

**Why this is the entry worth doing — the French Touch / DIX demos.**
`docs/test_corpus.md` makes DIX the priority gold-standard benchmark, and the
DIX demos are *beam-raced* and PAL-timed: they place a store or a soft-switch
write on a chosen **cycle within a scanline** and read the result off the
screen. The Apple II's video scanner presents one address per 1 MHz cycle
(Sather, *Understanding the Apple IIe*, ch. 3 and the floating-bus tables), so
"which cycle of the instruction did that write land on" is a question the
hardware answers and POM2 currently cannot: `step()` charges an instruction's
cycles in one lump, so every bus access inside an instruction is attributed to
the same instant. A cycle-stepped core is what makes "cycle exact" mean the
cycle rather than the instruction, and it is the only way to CALIBRATE a
raster effect against the real machine instead of tuning a per-device offset
until the picture looks right.

The present state is exactly that tuning, device by device, and each shim is
a place where a DIX effect could be a cycle out:
`getCurrentInstructionCycles()` for the Disk II, `syncToCpuCycle()+1` for the
Mockingboard T1 IRQ phase (bug hunt 2026-09-02 — OLDSKOOL's free-running
T1 handler read `$C404` one too high and wrapped the phase), and the 6522 T1
read-back bias of −1 chosen over MAME's because TRIBU's release-tuned re-arm
chain was the oracle. Three independent fudge factors, three different
oracles, no common cycle model underneath them.

**What is NOT known, and must not be asserted:** nobody has measured which —
if any — DIX effects POM2 currently mis-times. The 2026-09-02 "DIX raster
offset" turned out to be the 6522 read-back bias, not the CPU, and
`dix_menu_raster_probe` is a probe, not a pinned expectation. So this entry's
value is argued from the *mechanism* (an instruction-granular model cannot
express a per-cycle placement), not from a measured failure. What would settle
it: VALIDATION-006's headless corpus, which since 2026-11-06 gives DIX a
recorded per-frame screen hash (`pom2_corpus --tag dix`), run again under the
cycle-stepped core — a differential, which
is the only honest way to claim an improvement. Until then the correct
statement in any report is "POM2 is instruction-accurate with per-device
compensation", not "cycle exact".

**Evidence:** DEV.md § CPU: `step()` charges an instruction's cycles in one
lump, so the penultimate-cycle IRQ sample cannot be modelled and the Harte
gate checks final state only. The gap is papered over per device —
`getCurrentInstructionCycles()` for the Disk II, `syncToCpuCycle()+1` for the
Mockingboard T1 IRQ phase, the 6522 T1 read-back bias tuned on TRIBU.

- Derive the per-cycle microsequences from MAME's `om6502.lst` /
  `ow65c02.lst` (the cores already follow them instruction-wise).
- **A separate core, never a flag on the bus path**: `docs/PERFORMANCE.md`
  §§ 8.2/8.5 measured a branch there at 13-16 % and a flag test at 7.2 %.
  Select it at the frame or worker level.
- Default it on the PAL / DIX profiles only after VALIDATION-006 shows no
  corpus regression — and record the DIX differential (same corpus, both
  cores) in `docs/audits/`, since that differential is the only evidence that
  the new core actually buys demo accuracy rather than just cycle purity.
- Sequence note: **VALIDATION-006 came first** and is done (2026-11-06).
  Without a recorded per-frame expectation for DIX there was no way to tell
  an improvement from a regression, and a cycle-stepped core will move timings everywhere at once.

<a id="memory-002"></a>

## MEMORY-002 — `Memory::memRead` hot path

**Priority:** 🟡 · **State:** Confirmed open; revalidated 2026-11-07 ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md), [measured revalidation](../PERFORMANCE.md#memread-2026-11-07)).

**Acceptance criterion (after revalidation):** Show a measured memRead improvement with bus_fastpath differential checks and bench_identity unchanged.

**2026-11-07 outcome:** Current banner sampling puts the whole `memRead` at
23.0 % of main-thread samples, without isolating the ROM-guard share. Two
isolated reorder prototypes improved ][+ but each slowed //e in all five
alternating-order A/B pairs (paired medians +1.35 % and +1.64 %); both were
rejected and the bus code remains unchanged. All measured cycle/RAM/framebuffer
identities held; the second prototype also passed `bench_identity`. The ~15 %
below remains historical. Next: isolate the current guard share before a page
table; require ≥10 % attributable share, then a repeatable ≥3 % workload gain
without >1 % repeatable cross-workload regressions, plus the correctness gates.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1056). Original dates and estimates are retained below.

- 🟡 **`Memory::memRead` hot path** — the multi-level `if` cascade is
  `Memory::memReadSlow`; `memRead` itself is the inline fast path (RAM, ROM
  window, and since 2026-08-20 the //e internal `$C100-$CFFF` ROM;
  `memWrite` has the same split). What remains is the condition chain in
  front of the ROM-window hit (~15 % of a ][+ banner, `PERFORMANCE.md` § 7.4):
  a 256-entry dispatch table per high page would replace it with one indexed
  load, at the price of an invalidation at every paging-state writer. Any
  change here must keep `tests/bus_fastpath_test.cpp` green — it is the
  differential oracle for the fast paths. Prerequisite: `IIcClassProfile`
  extraction (done). Perf job, orthogonal to the `Keyboard`/`PaddleInputs`
  split that already shipped — do not merge the two.

<a id="memory-003"></a>

## MEMORY-003 — The Apple II Language Card

**Priority:** 🟢 · **State:** Needs a hardware oracle for the power-on bank ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)); the card's F8 ROM is frozen with the ][ Original profile.

No write-protect DIP or jumper on the Apple Language Card was found in any
source (the old entry's premise). POM2's `$C080-$C08F` decode, prewrite
rule and RAM/ROM/bank selection match MAME's `ramcard16k.cpp` exactly; a
65C02 RMW write-enables and an NMOS one does not, through the bus cycles,
as AppleWin issue #700 measured on hardware. Two points remain:

- **Power-on bank:** MAME's card comes up in bank 1 (`device_reset`),
  AppleWin's and POM2's in bank 2. No schematic or manual text settles it.
- **The card's own Autostart F8 ROM**, which replaces the motherboard's
  `$F800-$FFFF` on a ][ (Apple's card asserts /INH). MAME does not model
  it, and POM2 always fits the card, so modelling it would make every
  ][ Original an Autostart machine. It needs an "LC fitted" option first.

<a id="memory-001"></a>

## MEMORY-001 — Saturn 128K LC

**Priority:** ✅ · **State:** Done 2026-10-31 (`f7bcc8fb`) — catalog key
`saturn`, `SaturnCard.h/.cpp`, pinned by `saturn_card`; see
[DEV § Saturn 128K](../../DEV.md#saturn-128k-saturncard). Removed from the
active task index on 2026-11-13; the original entry is kept below.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1052). Original dates and estimates are retained below.

- 🧊 **Saturn 128K LC** (Saturn Systems) — 16 banks ×16 KB on LC
  `$D000-$FFFF`, switches `$C080-$C08F` slot-relative. MAME refs
  `bus/a2bus/a2memexp.cpp`. *2-3 d.*
  Frozen by the scope ruling: it leaves *Parked* only when named software needs it.
