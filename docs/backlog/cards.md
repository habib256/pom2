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
| [CARDS-001](#cards-001) | 🟡 | Partial: catalog entries missing | Reconcile four card abstraction levels with the catalog |
| [CARDS-002](#cards-002) | 🧊 | Frozen | Apple II Workstation Card — it boots, it is identified, and there is no network on the other end. |
| [CARDS-006](#cards-006) | 🧊 | Frozen | Apple II SCSI / High-Speed SCSI + CHD |
| [CARDS-007](#cards-007) | 🧊 | Frozen | Apple II VGA / Second Sight (VGA video card) |
| [CARDS-008](#cards-008) | 🧊 | Frozen | UDC (Apple 1991) |
| [CARDS-010](#cards-010) | 🧊 | Frozen | Apple 3.5" Controller IWM-level |
| [CARDS-011](#cards-011) | 🟢 | Done 2026-10-28 — move the register map to DEV.md, then archive (ID also reused at `CHANGELOG.md:945`) | ALS "The CP/M Card" (6 MHz Z80B, own 64 KB, CP/M Plus) |

<a id="cards-001"></a>

## CARDS-001 — Reconcile four card abstraction levels with the catalog

**Priority:** 🟡 · **State:** Partial ([triage 2026-10-28](../audits/backlog-triage-2026-10-28.md)): `abstractionCatalog()` lacks `workstation`, `4play`, `transwarp`, `slinky`, `zipchip`, `sam`, `videoterm`, `vidhd`, `grappler1`, `pic`.

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

**Priority:** 🟢 · **State:** Done 2026-10-28 — catalog key `alscpm`,
`AlsCpmCard.h/.cpp`, pinned by `als_cpm_card` (CP/M Plus 3.01C boots from
`disks_5.4/dsk/als_cpm3_disk1.dsk`; BTERM finds and drives an SSC through
the BIOS IOBYTE). Archive this entry with the next CHANGELOG sweep.

**Asked for by:** a letter about a Z80 card run in "ALS CP/M Card" mode with
an SSC-compatible serial card. No emulator had this card (MAME's
`a2bus/cards.cpp`: `softcard`, `applicard`; AppleWin: `CT_Z80` = SoftCard),
and its register map is in no manual: it was read off ALS's software.
`als_boot_probe` (probes target) boots a disk against a logging stub card.

**Sources (opened, not in the tree except Disk 1):**
- *The CP/M CARD User's Guide*, ALS 1982 —
  <https://www.callapple.org/wp-content/uploads/2024/07/ALS-CPM-Card.pdf>
  (§ 5: 6 MHz Z80B, 2 MHz while it touches the Apple; "an additional
  64K"; banks by an OUT of 0/1; a write to the slot toggles the Z80; an
  on-card clock interrupting with RST 30).
- CP/M 3.01C, VintageWare via Asimov `images/cpm/os/` — the 5.25" disks
  and the ProDOS `/BOOT` (`CPM3.SYSTEM`, `ZBOOT`, `CPMLDR`, `RWTS`, `APPLE`).

**How the card works, and where each fact comes from:**
- **Detection.** CPM3.SYSTEM ($237D) / Disk 1 boot ($0900) clear $08FE,
  copy an 8-byte SoftCard stub to Apple $1000 and `STA $Cn00` for n = 7…1.
  A SoftCard runs the stub at Z80 $0000, which toggles straight back. The
  ALS starts at Z80 $0000 = Apple **$9000**, where ZBOOT is loaded
  (CPM3.SYSTEM's load table: rwts $8800, apple $0300, zboot $9000, cpmldr
  $9100); ZBOOT finds no RAM at Z80 $5000, stores $FF to Z80 $78FE (Apple
  $08FE) and its toggle address $6n00 at $73C9.
- **Bank 0 = the Apple, SoftCard map with Z80 A15 inverted**, below $E000;
  Apple $7000-$8FFF unreachable. Every address above fits it: ZBOOT/CPMLDR
  at $0000/$0100, the call page at $7300 (Apple $0300), Apple $C000 at
  $6000, CPM3.SYS's banked BDOS/BIOS at $2B00-$5FFF = Apple $BB00-$BFFF and
  the language card.
- **Bank 1 = card RAM $0000-$DFFF; $E000-$FFFF common** (resident BDOS/BIOS
  at $F200). 56 + 8 KB = the card's 64 KB.
- **Bank select** = BIOS SELMEM $F944: `OUT ($00)` of $F0 (bank 0) or $FF
  (bank 1). Data bit 0; the port is not decoded in the model.
- **6502 ↔ Z80 calls** go through Apple page 3 (the `apple` file): the
  6502 loops at $035C saving A/X/Y/P/S in $45-$49, `STA $Cn00`, then
  `JSR ($03C7)`; the Z80 side ($7312/$7335) stores the target and toggles
  through $6n00. $7388/$7392 read and write any Apple byte via the 6502.
- **Clock.** 60 Hz: the BIOS handler at $FB94 counts $3C ticks to a second;
  vectors at $0030 and $0038 of bank 1; interrupts only enabled in bank 1;
  no port read to acknowledge.
- **Console.** The ALS BIOS keeps an IOBYTE: each character entry copies
  $0003 from the TPA bank and dispatches on the field (banked BIOS $5B6F):
  slot per value at Apple $08F5-$08F8, driver per card type. CONST follows
  CON:, AUXIST follows RDR:.

**Also pinned** (`als_cpm_card`): the speed — 6 MHz on card RAM, and an
Apple access waits for the next bus-cycle edge then takes the whole cycle
(NOPs fetched from Apple memory run at one per 2 bus cycles); and 3.01C's
ProDOS path — booted from ProDOS on a slot-5 hard disk
(`disks_3.5/als_cpm3_prodos_800k.po`, A: = /CPM/SYSTEM through MLI calls),
INITRAM builds the RAM disk in ProDOS's /RAM and it answers as D:. Two
media findings on the way: Disks 2 and 3 end their directory in a sector
the formatter never wrote (zeros, now read as free entries); and the 800K
volume's virtual disk has DIR/PIP/INITRAM… entries written by an archiving
tool with 8-bit block numbers on a 16-bit format — they load as garbage on
the real card too, so the test takes INITRAM from the 3.01C ProDOS disk
(`disks_5.4/dsk/als_cpm3_prodos_disk_301c.cpm`, CP/M sector order, now
read by `pom2_cpm`) and PIP from Disk 1.

**Left:** the RTC's rate on a PAL machine is assumed 60 Hz (a crystal on
the card; nothing in the software says otherwise).
