# Backlog — Slot cards and peripherals

[Planning index](../../TODO.md) · [Scope decisions](../decisions/project-scope.md)

Reorganised 2026-10-05. Priorities are inherited from the original TODO;
older reports are **To verify**, not confirmed defects on current main.
Before implementation, reproduce the described behavior on current main and
record the result. If already resolved, remove the active entry and link its
resolution in the changelog. Frozen work requires a named software need or
an explicit request before scheduling.

Current implementation review: [all extension cards, 2026-10-17](../audits/extension-cards-2026-10-17.md).
It separates reproduced defects, tested contracts and remaining hardware limits.

<a id="historical-findings"></a>

## Task index

| Task | Priority | State | Subject |
|---|---|---|---|
| [CARDS-001](#cards-001) | 🟡 | To verify | Reconcile four card abstraction levels with the catalog |
| [CARDS-002](#cards-002) | 🧊 | Frozen | Apple II Workstation Card — it boots, it is identified, and there is no network on the other end. |
| [CARDS-006](#cards-006) | 🧊 | Frozen | Apple II SCSI / High-Speed SCSI + CHD |
| [CARDS-007](#cards-007) | 🧊 | Frozen | Apple II VGA / Second Sight (VGA video card) |
| [CARDS-008](#cards-008) | 🧊 | Frozen | UDC (Apple 1991) |
| [CARDS-010](#cards-010) | 🧊 | Frozen | Apple 3.5" Controller IWM-level |
| [CARDS-011](#cards-011) | 🟡 | Reverse engineering | ALS "The CP/M Card" (6 MHz Z80B, own 2×64 KB, CP/M Plus) |

<a id="cards-001"></a>

## CARDS-001 — Reconcile four card abstraction levels with the catalog

**Priority:** 🟡 · **State:** To verify.

**Acceptance criterion (after revalidation):** Keep liron, workstation, 4play and transwarp levels consistent between lle_vs_hle and abstractionCatalog, or archive if already aligned.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-695). Original dates and estimates are retained below.

The original report found no row or catalog key for these cards. The current
documentation has all four; the remaining catalog gaps are recorded below.

| Card | Level | The seam to record |
|---|---|---|
| `liron` | **L2 firmware + L0 media path, H2 drive** — a composite | The UniDisk's drive-side 65C02 is not emulated; the protocol is the contract. Say explicitly that this is the strictly *lower* sibling of the `smartportcard` L2 veneer, since the picker now shows both |
| `workstation` | **L0 CPU + L1 SCC, with no transport** | A third failure category, the one the FujiNet's "no peer" row also names: **the network on the other end does not exist.** Level is high; reach is zero |
| `4play` | **L1 — and complete** | Nothing below the register to model. Needs an explicit *complete* marker, or "L1" reads as "something is missing" |
| `transwarp` | **L1 registers / host retiming** (original proposed label) | The documented catalog classification is H1. CPU multiplier is sampled per 4096-cycle chunk, not once per frame (`transwarp_chunk_sampling`) |

**Revalidated 2026-10-06 — half done.** `docs/lle_vs_hle.md` now has a row
for all four (Liron L2, TransWarp H1, Workstation L0/L1, 4play L1); the
levels it records differ from the table above for `liron` and `transwarp`.
`abstractionCatalog()` (`src/AbstractionLevels_ImGui.cpp:119-358`) still has
no `liron`, `workstation`, `4play` or `transwarp` key, so the picker still
shows no level for them.

**Code reconciliation 2026-10-19:** The older catalog claim is partly
superseded: `abstractionCatalog()` now contains `liron` at L2. `workstation`,
`4play` and `transwarp` still have no catalog row. The document already
describes all four, so do not schedule their documentation as missing.

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

**State:** Implemented 2026-10-18 after the explicit request to complete the
remaining features. The previous hide ruling applied to the silent scaffold.

The catalog now offers the actual VIA/two-AY/TMS5220C card, with FIFO-paced
LPC synthesis and stereo audio. ETS v2 preserves generator/filter state and
fractional clocks. See [DEV](../../DEV.md#echoplustms5220card) for the hardware
map, reference, tests and remaining pin-timing/analog validation limits.

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

**Priority:** ✅ · **State:** Done (2026-10-16, explicit user request).

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1824).

- ✅ **Slinky** — `SlinkyCard` (catalog `slinky`), MAME `a2memexp.cpp` registers
  and Apple's real 341-0344A firmware; ProDOS mounts it as a RAM disk. Pinned
  `slinky_card`. → [DEV § Slinky](../../DEV.md#memory-expansion-card--slinky-slinkycard).
  RamFactor shares the register model but is not modelled (no ROM shipped).
- 🧊 **RamFAST** (a SCSI card with a RAM cache, not a RAM disk) — still frozen.

<a id="cards-010"></a>

## CARDS-010 — Apple 3.5" Controller IWM-level

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1825). Original dates and estimates are retained below.

- 🧊 **Apple 3.5" Controller IWM-level** — refactor IWMDevice attached
  to a slot card (rare).

<a id="cards-011"></a>

## CARDS-011 — ALS "The CP/M Card"

**Priority:** 🟡 · **State:** Reverse engineering, started 2026-10-28.

**Asked for by:** a letter about a Z80 card run in "ALS CP/M Card" mode with
an SSC-compatible serial card. No emulator has this card: not MAME (its
`a2bus/cards.cpp` has `softcard` and `applicard` only), not AppleWin
(`CT_Z80` = SoftCard). Its register map is documented nowhere, so it has to
be read off ALS's own boot code. Manual tool:
`cmake --build build --target als_boot_probe`
(`tests/als_boot_probe.cpp`: boots a disk against a logging stub card,
prints every access to its slot with the 6502 PC, dumps RAM).

**Sources (opened, not in the tree):**
- *The CP/M CARD User's Guide*, ALS 1982 —
  <https://www.callapple.org/wp-content/uploads/2024/07/ALS-CPM-Card.pdf>
  (§ 5: hardware).
- CP/M 3.01C disks + notes, VintageWare via Asimov —
  `images/cpm/os/Advanced Logic Systems (ALS)-CPM Card Installation Software.zip`
  (`CPM.Disk1.do` … `Disk3.do`, `.shk` of /BOOT, /CPMBOOT, full install),
  and `images/cpm/os/ALS CPM Card.zip` (ProDOS `.po` installs).

**Established:**
- Hardware (manual § 5): 6 MHz Z80B, slowed to 2 MHz only while it touches
  the Apple; 2 × 64 KB on the card, bank chosen by `OUT (0F0H)` 0/1; a
  common area at the top (about $E000/$F900 up) seen from both banks; an
  on-card real-time clock interrupting the Z80 (RST 30); slot IRQs are
  passed to the 6502. Slot 4 or 7. Ships **CP/M Plus** (3.0, 3.01C in
  Nov. 1984): there is no IOBYTE; devices are assigned with `DEVICE`.
- **On/off = one toggle**, as on the SoftCard: any write by either CPU to
  the card's `$Cn00` page. Disk 1's boot (T0S0 → $0800, then $0900)
  writes `$00` to `$C700`, `$C600` … `$C100` (self-modified `STA $Cn00` at
  $092C) and calls the card found when `$08FE` is non-zero afterwards;
  otherwise `** NO CP/M CARD IN SYSTEM **`. POM2's SoftCard in slot 4 is
  *not* found by it.
- **Bank-0 window = the SoftCard map with Z80 A15 inverted** (inference,
  consistent with every address seen): Apple $0000-$0FFF at Z80 $7000,
  Apple $C000-$CFFF at Z80 $6000. Evidence: the dual 6502/Z80 jump page
  the boot copies to Apple $0300 (`C3 12 73` = `JP $7312` beside `4C 5C 03`
  = `JMP $035C`) reads `$6082` ($C082), `$6FFF` ($CFFF) and `$77F8`
  (MSLOT) and adds $A0 to $Cn to reach $6n00; and the boot carries two
  8-byte Z80 stubs side by side, one per map — at $0AB1
  `LD A,($F8FF) / ADD $20 / LD H,A / LD (HL),A` (SoftCard: $08FF via
  $F8FF, toggle via $En00) and at $0AA9
  `LD A,($78FF) / SUB $60 / LD H,A / LD (HL),A` (the same through $78FF and
  $6n00).

**Open:**
- What sets `$08FE`. Neither stub writes it, and only the SoftCard one is
  copied (to $1000 = Z80 $0000 on a SoftCard). The Z80's state at the
  first toggle after power-on (PC, which bank, which map) decides it.
- The rest of bank 0 (Apple $1000-$BFFF and the language card), the RTC's
  ports and rate, and whether the 6502 is halted while the Z80 runs (the
  manual says "DMA device", one CPU at a time).
- The CP/M 3 BIOS (files `APPLE`, `RWTS`, `CPMLDR` on /BOOT) is the next
  thing to disassemble; a real card, or the Z80 card the letter mentions
  in its ALS mode, would settle the power-on state in one test.

**Note for BTERM:** CP/M Plus has no IOBYTE, but has BDOS 7 (AUXIST) and 8
(AUXOST): an AUX:-input-status call is the poll BTERM looks for, and the
SoftCard's 2.20 BIOS cannot offer it (its CONST ignores the IOBYTE —
`softcard_ssc_bterm`).

