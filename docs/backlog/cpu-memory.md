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
| [MEMORY-004](#memory-004) | 🟡 | To verify | Bug hunt #14's CPU residue |
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

## MEMORY-004 — Bug hunt #14's CPU residue

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Verify WAI/STP, DMA stop handling and SSC IRQ behavior against the applicable CPU/firmware model before changing the policy.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2016). Original dates and estimates are retained below.

- 🟡 **Bug hunt #14's CPU residue** *(2026-09-09; read, not fixed)*: `WAI`
  charges 3 cycles and falls through (a `waiting` latch parallel to
  `halted` would fix it: wake on the line regardless of I, vector only if
  I=0); `$CB`/`$DB` are WAI/STP on the CMOS table where the Rockwell/GTE
  parts in every shipped Apple execute a 1-cycle NOP — a ruling, not a
  bug; `runCpuSlice`'s DMA-release tail skips the stop check and the
  breakpoint reconciliation. *½ day.* **Bug hunt #14's I/O residue**: the
  SSC ships SW2-6 (interrupts) on where MAME's DIP default is off, and its
  RDR read clears `IRQ_RDRF` where MAME's `read_rdr` does not — the
  in-code comment attributing that to MAME is wrong.

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
