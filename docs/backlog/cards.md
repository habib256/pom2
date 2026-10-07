# Backlog — Slot cards and peripherals

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
| [CARDS-001](#cards-001) | 🟡 | To verify | Reconcile four card abstraction levels with the catalog |
| [CARDS-003](#cards-003) | 🟡 | To verify | SmartPortCard leftovers, still open |
| [CARDS-005](#cards-005) | 🟡 | To verify | Nothing asserts the real ClockCard ROM path is taken |
| [CARDS-011](#cards-011) | 🟡 | To verify | Ghostscript unreachable on a stock Apple Silicon Homebrew |
| [CARDS-002](#cards-002) | 🧊 | Frozen | Apple II Workstation Card — it boots, it is identified, and there is no network on the other end. |
| [CARDS-004](#cards-004) | 🧊 | Frozen | EchoPlusTMS5220Card (real Echo+) |
| [CARDS-006](#cards-006) | 🧊 | Frozen | Apple II SCSI / High-Speed SCSI + CHD |
| [CARDS-007](#cards-007) | 🧊 | Frozen | Apple II VGA / Second Sight (VGA video card) |
| [CARDS-008](#cards-008) | 🧊 | Frozen | UDC (Apple 1991) |
| [CARDS-009](#cards-009) | 🧊 | Frozen | Slinky / RamFAST RAM disk |
| [CARDS-010](#cards-010) | 🧊 | Frozen | Apple 3.5" Controller IWM-level |
| [CARDS-012](#cards-012) | 🟡 | To verify | SSC fallback page and DOS 3.3 `PR#n` |
| [CARDS-013](#cards-013) | 🟢 | To verify | `$C800` owner across Ctrl-Reset |

<a id="cards-001"></a>

## CARDS-001 — Reconcile four card abstraction levels with the catalog

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Keep liron, workstation, 4play and transwarp levels consistent between lle_vs_hle and abstractionCatalog, or archive if already aligned.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-695). Original dates and estimates are retained below.

They have no row in the doc and no key in `abstractionCatalog()`, so the picker
shows no level for them. Land both in one commit — they are kept in step by hand.

| Card | Level | The seam to record |
|---|---|---|
| `liron` | **L2 firmware + L0 media path, H2 drive** — a composite | The UniDisk's drive-side 65C02 is not emulated; the protocol is the contract. Say explicitly that this is the strictly *lower* sibling of the `smartportcard` L2 veneer, since the picker now shows both |
| `workstation` | **L0 CPU + L1 SCC, with no transport** | A third failure category, the one the FujiNet's "no peer" row also names: **the network on the other end does not exist.** Level is high; reach is zero |
| `4play` | **L1 — and complete** | Nothing below the register to model. Needs an explicit *complete* marker, or "L1" reads as "something is missing" |
| `transwarp` | **L1 registers / host retiming** | The doc's first entry whose abstraction is in the **time domain**. Multiplier sampled once per frame: unbiased in aggregate, wrong about where in a frame the slow cycles land |

**Revalidated 2026-10-06 — half done.** `docs/lle_vs_hle.md` now has a row
for all four (Liron L2, TransWarp H1, Workstation L0/L1, 4play L1); the
levels it records differ from the table above for `liron` and `transwarp`.
`abstractionCatalog()` (`src/AbstractionLevels_ImGui.cpp:119-358`) still has
no `liron`, `workstation`, `4play` or `transwarp` key, so the picker still
shows no level for them.

<a id="cards-003"></a>

## CARDS-003 — SmartPortCard leftovers, still open

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Exercise failed boots and CONTROL data-list calls; document unsupported commands with the correct guest error.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1772). Original dates and estimates are retained below.

- 🟡 **SmartPortCard leftovers, still open** (2026-07-12 Liron audit
  follow-ups) — two of the original five remain:
  - boot failure is a silent `JMP $CnE0` loop; real firmware prints an error.
  - CONTROL calls that need the control-list DATA: only code 0 works, because
    the stub has no guest→device list copy. Everything else returns `$21`.

<a id="cards-005"></a>

## CARDS-005 — Nothing asserts the real ClockCard ROM path is taken

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Require the real ClockCard ROM path when its dump is present, and exercise driver loading from C800.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1797). Original dates and estimates are retained below.

- 🟡 **Nothing asserts the real ClockCard ROM path is taken** when the dump
  is present — `clock_card_smoke` tolerates its absence so CI stays
  ROM-free, so a regression that silently routed back to the synthetic ROM
  would fail nothing. Same silent-degradation hole as every other
  ROM-driven L path (Disk II P6, mouse MCU, Grappler EPROM); →
  [`docs/lle_vs_hle.md`](../lle_vs_hle.md) § Keeping a level once you
  have it. **Folded into [G5-15](release-1.0.md#g5)**, with
  Disk II P6, the mouse MCU and the Grappler EPROM. The DOS 3.3 / Applesoft
  tools that pull the driver from `$C800` are still untested.

**Revalidated 2026-10-06 — headline shipped.** `rom_path_taken` asserts that
the ThunderClock+ runs the Thunderware U9 dump, not the synthetic stub
(`tests/rom_path_taken_test.cpp:101-104`; CHANGELOG *2026-09-17 — G5-15:
the real ROM path, asserted*). `clock_card_smoke` pins the `$C800` claim
(`testC800ClaimFollowsTheEprom`) but no test runs the DOS 3.3 / Applesoft
driver loaded from `$C800`; only that residue remains.

<a id="cards-011"></a>

## CARDS-011 — Ghostscript unreachable on a stock Apple Silicon Homebrew

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Resolve and explain Ghostscript availability on Homebrew; verify the remaining printer margin, sizing and control-code cases independently.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1996). Original dates and estimates are retained below.

- 🟡 **Ghostscript unreachable on a stock Apple Silicon Homebrew**
  *(2026-09-09, bug hunt #15)*: `ChildProcess::findOnPath` refuses
  group-writable directories and Homebrew ships `/opt/homebrew/bin` as
  `drwxrwxr-x root:admin`, so `findPostScriptInterpreter()` returns nothing
  with `gs` installed and the panel says "install Ghostscript". The refusal
  is deliberate; at least say why, or accept a root-owned admin-group
  directory. Smaller printer residue: `status().headX` can read one dot past
  the right margin; the "Reset printer" tooltip says "discarded" where the
  sheet is ejected; `setPaperDimensions` re-snaps both axes and leaves the
  size combo stale; the carriage is the paper width (82 columns on Letter)
  where a real ImageWriter II has an 8.0 in / 80-column carriage;
  `kStyleDoubleStrike` is a silent no-op; unassigned control codes print
  CP437 art. *½ day.*

<a id="cards-002"></a>

## CARDS-002 — Apple II Workstation Card — it boots, it is identified, and there is no network on the other end.

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1661). Original dates and estimates are retained below.

- 🧊 **Apple II Workstation Card — it boots, it is identified, and there is no
  network on the other end.** *(Frozen by the [scope ruling](../decisions/project-scope.md#the-scope-ruling);
  steps 2 and 3 below are closed as won't-do. The title used to say the host
  handshake did not work — it does, see sub-item 1, which is why this item
  contradicted itself for a week.)*
  <a id="apple-ii-workstation-card"></a>The card that put a IIe on LocalTalk,
  so it could netboot from an AppleShare server and reach the LaserWriters on
  the same net. Emulated as `WorkstationCard` (catalog `workstation`), pinned
  by `workstation_card_smoke`.
  → [DEV](../../DEV.md#apple-ii-workstation-card-workstationcard),
  [plan 2 § 5](../printer_plan_2.md#5-the-apple-ii-workstation-card--it-boots)

  **What works.** Apple's real 341-0358-A firmware runs on the card's own
  65C02 — over `Memory::ForeignBus`, so the Apple II's hot path pays nothing
  (measured: PERFORMANCE § 9) — completes the power-on self-test including the
  255-byte SCC loopback, configures the chip for **LocalTalk, SDLC,
  230400 bit/s**, and then **acquires a node address and transmits real LLAP
  frames** (`0B 0B 81` lapENQ, then `FF 0B 84` and short DDP broadcasts). The
  `$Cn00` window, the `$C800-$CFFF` expansion ROM, the `$7C00` ROM banking,
  the interval timer and the snapshot (chip included) all work with the card
  in a real `SlotBus` — and **CardCat, booted on the emulated //e, names the
  card in slot 4**.

  **What remains, in order:**

  1. ✅ **The host handshake works.** AppleShare's `ATINIT` calls the card at
     `$Cn14` in ProDOS-MLI style — `JSR $Cn14 / .BYTE cmd / .WORD block`,
     command `$42` — and POM2 now services it end to end: the command byte
     reaches `$CnDB`, both rendezvous semaphores return to rest, and the call
     returns past its inline parameters. Pinned by `workstation_card_smoke`.

     **The bug was one number**: the `$C800-$CFFF` window was based at file
     `0xC800` instead of `0xC400`, so the page's `JMP $CC00` landed on a
     block-copy loop rather than the driver prologue
     (`CLD / PHP / SEI / LDA #$50 / STA $C080,X`). Nine bases were swept;
     `0xC400` is the only one at which the transaction completes.

     **Two things worth keeping from how long that took.** The card steers
     the host by *patching the host's code* — it writes `$CnBB`/`$CnBC` (the
     operands of the host's `JMP`) and `$CnC3`/`$C4`/`$C6`/`$C7` (the address
     operands of its block-move), and releases the host's spin loops by
     writing `$38` (`SEC`) over the `$18` (`CLC`) it is executing. And the
     "missing bit 6 of `$02EE`" was a **red herring**: wiring it moved the
     card one step further, which made it look right, and it was not. A
     change that unsticks a stuck system is not evidence that it is correct.

     Still open, and now cheap to look at: `$C0nX` reads answer `$FF` and
     writes are ignored, and the transaction completes anyway — so whatever
     the strobes are for, this path does not need them. `hostStrobeLog()`
     records them for whoever wants to find out.

     ✅ **Verified end to end**: `disks_3.5/AppleShare IIe Workstation.po`
     boots in POM2, its ATINIT passes the card's power-up diagnostics and
     the workstation software reaches its menu.

  2. 🧊 **Why lapACK does not move the node.** Answering the card's lapENQ
     with lapACK is accepted by the chip — the FIFO fills, the interrupt fires
     — and the driver enquires again anyway rather than picking another
     address. Timing window, a status bit that is not set, or simply what this
     firmware does on a dead network. Worth an hour before (3). *~0.5 d.*
  3. 🧊 **A host-side LocalTalk endpoint** — bridge the card's frames to a
     real or emulated AppleTalk network. `setFrameCallback` and
     `receiveFrame` are the seam and both work; note the card **disables its
     receiver while transmitting**, so an endpoint must wait for WR3 D0 to
     come back before answering. *~1-2 d.*

  ~~**SDLC framing.**~~ **Done** — datasheet-derived (MAME has no SDLC),
  marked `SDLC (datasheet, not MAME)` at every site, pinned by
  `scc8530_smoke`. ~~**The SCC's register file is not in the snapshot.**~~
  **Done** — `Scc8530Device::appendSnapshot`/`restoreSnapshot`, carried by the
  card's own blob.

  **Smaller gaps, worth knowing.** The interval timer's period is a **chosen**
  1 ms, not a derived one: the dump does not settle it and the firmware boots
  with it. And the card runs a second 6502 at the Apple II's own rate, so
  plugging it roughly doubles the emulation work.

  **Do not "optimise" `advanceCycles`.** Its 24-cycle interleave is
  correctness, not tuning: the POST's self-test has a fixed poll budget, and
  running the CPU for a whole 4096-cycle slot-bus chunk before the SCC moves
  fails it on a timeout no real card would see.

  **Worth knowing before finishing it:** given the LaserWriter's PostScript
  path already ships (plan 2 § 4), this card buys an *alternative transport*
  for PostScript that the Super Serial Card already carries — not a new
  capability. It is worth doing for netboot and AppleShare, and for being the
  way most sites actually wired a LaserWriter; it is not the only way to print
  to one.

<a id="cards-004"></a>

## CARDS-004 — EchoPlusTMS5220Card (real Echo+)

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1787). Original dates and estimates are retained below.

- 🧊 **EchoPlusTMS5220Card (real Echo+)** — catalog scaffold
  `echoplus_tms`: SlotPeripheral + stub register decode at
  $Cs00-$Cs0F, enough for detection.
  Remaining: TMS5220 LPC10 decoder (chirp ROM + K-parameter
  interpolation) and AY-3-8913 audio synth — the shared AY core it
  needs already exists (`src/AyPsgSynth.h`, extracted 2026-08-01,
  see [Audio]). *~3-5 d.*
  **Ruling [R2](../decisions/project-scope.md#standing-rulings) is answered by the scope ruling: hide.**
  Taking it out of the README table and the picker is a
  [G3](release-1.0.md#g3) item; the chip itself is closed as won't-do.
  *Revalidated 2026-10-06:* the picker half is done — `echoplus_tms` is
  hidden from the picker since 2026-09-08 and a saved key still plugs it
  (`src/SlotCardCatalog.h:158-161`). The README's Audio row still lists
  "Echo+ TMS5220 scaffold".

<a id="cards-006"></a>

## CARDS-006 — Apple II SCSI / High-Speed SCSI + CHD

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1806). Original dates and estimates are retained below.

- 🧊 **Apple II SCSI / High-Speed SCSI + CHD** — MAME
  `a2scsi.cpp` (NCR 5380) / `a2hsscsi.cpp` (53C80). Big lift for a
  niche need (CFFA suffices). *~30-50 h.*

<a id="cards-007"></a>

## CARDS-007 — Apple II VGA / Second Sight (VGA video card)

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1809). Original dates and estimates are retained below.

- 🧊 **Apple II VGA / Second Sight (VGA video card)** — slot card that
  shadows the Apple II framebuffer and outputs a clean VGA signal
  (scanline mode + text/HGR/DHGR/lo-res modes). Two incarnations: the
  open-hardware project **markadev/AppleII-VGA** (RP2040, free firmware +
  KiCad, so registers and timing are documented) and the commercial
  **Second Sight** (reactivemicro, Brutal Deluxe manual). POM2 already has
  all the video decode (`Apple2Display`); the value would be modelling the
  card's soft-switches/registers for software detection and an optional
  "VGA-clean" output. Code + doc refs:
  - <https://github.com/markadev/AppleII-VGA> (RP2040 firmware + KiCad)
  - <https://www.brutaldeluxe.fr/documentation/secondsight/secondsight_manual.pdf> (Second Sight manual)
  - <https://downloads.reactivemicro.com/Apple%20II%20Items/Hardware/SecondSite_VGA/> (ReactiveMicro dumps/ROMs)
  - <https://www.apple2history.org/history/ah13/#05> (historical context)
  *~5-10 d (register sourcing + integration mode to be decided).*

<a id="cards-008"></a>

## CARDS-008 — UDC (Apple 1991)

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1823). Original dates and estimates are retained below.

- 🧊 **UDC (Apple 1991)** — 4 heterogeneous bays (3.5"/5.25"/HDV).

<a id="cards-009"></a>

## CARDS-009 — Slinky / RamFAST RAM disk

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1824). Original dates and estimates are retained below.

- 🧊 **Slinky / RamFAST RAM disk** — limited utility vs RamWorks III.

<a id="cards-010"></a>

## CARDS-010 — Apple 3.5" Controller IWM-level

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1825). Original dates and estimates are retained below.

- 🧊 **Apple 3.5" Controller IWM-level** — refactor IWMDevice attached
  to a slot card (rare).

<a id="cards-012"></a>

## CARDS-012 — SSC fallback page and DOS 3.3 `PR#n`

**Priority:** 🟡 · **State:** To verify (lead from bug hunt 2026-10-06, not traced to a defect).

**Acceptance criterion:** With `roms/ssc_341-0065-a.bin` absent, DOS 3.3 + `PR#2` + `CATALOG` prints to the SSC and returns to the prompt; pin it.

**Evidence:** the hand-assembled page's dispatch (`SuperSerialCard.cpp`, region `dispatch`) decides output vs input by `CSWH == $Cn && CSWL == 0`. Under DOS 3.3, `PR#n` stores `$Cn00` in DOS's own hook; if CSW still points at DOS when DOS calls `$Cn00`, the page would take the input path and spin. DOS's own hook swap may make this safe — unverified. Only matters without the EPROM dump, which ships.

<a id="cards-013"></a>

## CARDS-013 — `$C800` owner across Ctrl-Reset

**Priority:** 🟢 · **State:** To verify against a MAME checkout.

**Acceptance criterion:** Cite MAME's `machine_reset` for `m_cnxx_slot`; if it releases the window, `SlotBus::reset()` does too, pinned.

**Evidence:** `SlotBus::reset()` keeps `activeExpansionSlot` deliberately. Bug hunt 2026-10-06 recalled (from memory) `m_cnxx_slot = CNXX_UNCLAIMED` in MAME's reset. Low impact: every shipped ROM opens with `LDA $CFFF`.

