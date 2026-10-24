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
| [MEMORY-002](#memory-002) | 🟡 | To verify | `Memory::memRead` hot path |
| [MEMORY-004](#memory-004) | 🟢 | Known limit | WDC `WAI` falls through instead of waiting |
| [MEMORY-003](#memory-003) | 🟢 | To verify | Dedicated Pascal LC |
| [MEMORY-001](#memory-001) | 🧊 | Frozen | Saturn 128K LC |

<a id="memory-002"></a>

## MEMORY-002 — `Memory::memRead` hot path

**Priority:** 🟡 · **State:** To verify.

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

<a id="memory-004"></a>

## MEMORY-004 — WDC `WAI` falls through instead of waiting

**Priority:** 🟢 · **State:** Known limit, documented in `M6502::WAI`.

**Acceptance criterion:** WAI parks until IRQ or NMI (waking whatever I
says, vectoring only with I=0, MAME `ow65c02.lst:797-803`), with
`bench_identity` and the bus fast-path numbers unchanged.

`WAI` charges 3 cycles and falls through to PC+1. A `waiting` latch beside
`halted` would fix it, but that latch sits on `step()`'s path, which
`docs/PERFORMANCE.md` §§ 8.2/8.5 measures before anything is added there.

**Closed 2026-10-24 from the original residue:** `$CB`/`$DB` as WAI/STP on
the CMOS table follows MAME, whose enhanced //e is a `W65C02`
(`apple2e.cpp:5263`); the SSC's RDR read no longer releases IRQ_RDRF (MAME
`read_rdr` does not — only `read_status`), pinned in `ssc_acia_smoke`; SW2-6
(interrupts) ships ON where MAME's `DSWX` default is OFF — kept on purpose,
so interrupt-driven drivers (A2osX's among them) work out of the box, and
the panel switches it.

<a id="memory-003"></a>

## MEMORY-003 — Dedicated Pascal LC

**Priority:** 🟢 · **State:** To verify.

**Acceptance criterion (after revalidation):** Model the Pascal LC write-protect behavior and test its banking against the selected hardware reference.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1067). Original dates and estimates are retained below.

- 🟢 **Dedicated Pascal LC** — 16 KB variant shipped with Apple Pascal,
  minor differences vs IIe LC (write-protect DIP). *1 d.*

<a id="memory-001"></a>

## MEMORY-001 — Saturn 128K LC

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1052). Original dates and estimates are retained below.

- 🧊 **Saturn 128K LC** (Saturn Systems) — 16 banks ×16 KB on LC
  `$D000-$FFFF`, switches `$C080-$C08F` slot-relative. MAME refs
  `bus/a2bus/a2memexp.cpp`. *2-3 d.*
  Frozen by the scope ruling: it leaves *Parked* only when named software needs it.
