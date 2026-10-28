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
| [CPU-001](#cpu-001) | 🟡 | To design | Optional cycle-stepped 6502 core |
| [MEMORY-002](#memory-002) | 🟡 | Confirmed open | `Memory::memRead` hot path |
| [MEMORY-003](#memory-003) | 🟢 | Needs hardware oracle (bank) | Language Card power-on bank; the Apple card's own F8 ROM |
| [MEMORY-001](#memory-001) | 🧊 | Frozen | Saturn 128K LC |

<a id="cpu-001"></a>

## CPU-001 — Optional cycle-stepped 6502 core

**Priority:** 🟡 · **State:** To design ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)).

**Acceptance criterion:** A second core, selected per profile or by a setting,
issues one bus access per cycle in silicon order and samples IRQ/NMI in the
penultimate cycle. It passes the Tom Harte 65x02 corpus on **per-cycle bus
ordering** (address, data, direction — not only final state and cycle count),
both Klaus suites, and `display_golden_hash`; the existing fast core keeps
`bench_identity` and `pom2_bench` unchanged; and at least one device sync shim
is retired under the new core with a mutation control (first candidate: the
Mockingboard's `syncToCpuCycle()+1`).

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
  corpus regression.

<a id="memory-002"></a>

## MEMORY-002 — `Memory::memRead` hot path

**Priority:** 🟡 · **State:** Confirmed open ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)); re-measure the share before prototyping.

**Acceptance criterion (after revalidation):** Show a measured memRead improvement with bus_fastpath differential checks and bench_identity unchanged.

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

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1052). Original dates and estimates are retained below.

- 🧊 **Saturn 128K LC** (Saturn Systems) — 16 banks ×16 KB on LC
  `$D000-$FFFF`, switches `$C080-$C08F` slot-relative. MAME refs
  `bus/a2bus/a2memexp.cpp`. *2-3 d.*
  Frozen by the scope ruling: it leaves *Parked* only when named software needs it.
