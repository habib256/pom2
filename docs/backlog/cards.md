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
| [CARDS-002](#cards-002) | 🧊 | Frozen | Apple II Workstation Card — it boots, it is identified, and there is no network on the other end. |
| [CARDS-006](#cards-006) | 🧊 | Frozen | Apple II SCSI / High-Speed SCSI + CHD |
| [CARDS-007](#cards-007) | 🧊 | Frozen | Apple II VGA / Second Sight (VGA video card) |
| [CARDS-008](#cards-008) | 🧊 | Frozen | UDC (Apple 1991) |
| [CARDS-010](#cards-010) | 🧊 | Frozen | Apple 3.5" Controller IWM-level |
| [CARDS-017](#cards-017) | 🟢 | Ready | Hayes AT modem layer on the Super Serial Card |
| [CARDS-018](#cards-018) | 🧊 | Frozen — media found, low value | Synetix SuperSprite |

<a id="cards-016"></a>

## CARDS-016 — E-Z Color: the VDP's /INT on /NMI, as published

**Priority:** ✅ · **State:** Done 2026-11-16 — the bus `/NMI` line
(`SlotPeripheral::assertNmi`, edge-triggered wire-OR) and the card's jumper
JP2 (`EzColorCard::setInterruptWiring`, setting `ezcolor_interrupt`, Card
Monitor, SDK `setEzColorInterrupt`). Pinned by `ezcolor_card` § 7. Removed
from the active index; see CHANGELOG 2026-11-16 and
[DEV § /NMI](../../DEV.md#nmi--pin-29-2026-11-16).

**Default:** `nmi`, the schematic's, from the first release that has the
jumper (1.2.1) — pom2games asked for it on 2026-11-16
(`dev/tools/a2shot/LETTRE_POM2_EZCOLOR_JP2.md` in their tree).

<a id="cards-015"></a>

## CARDS-015 — Integrate Sébastien Pourcel's PorteParole-MEA8000

**State:** Implemented 2026-11-08, following the explicit request to implement
his engine and the clarification that his practical validation is the basis.
Removed from the active task index on 2026-11-13; retained for incoming links.

The unmodified portable engine at `0984c9ef` is now compiled directly;
POM2's old synthesis is replaced. Its resonators, FM2 table, pitch, noise,
REQ turnaround, busy-write behaviour and analogue profile are used together.
POM2 supplies the slot bus, CPU-clock conversion, host audio resampling and
versioned snapshots. The upstream 187 checks and differential PCM tests
cover the port. See [DEV](../../DEV.md#porte-parole-porteparolecard-mea8000)
and [import provenance](../../third_party/porteparole-mea8000/POM2-IMPORT.md).

The practical validation belongs to the reference author; a new independent
hardware recording comparison is not claimed for POM2. The missing upstream
scripts have not been recreated.

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

**Priority:** ✅ · **State:** Done 2026-10-28 — catalog key `alscpm`,
`AlsCpmCard.h/.cpp`, pinned by `als_cpm_card`. Removed from the active task
index on 2026-11-13; the register map, its sources and what is left moved to
[DEV § ALS "The CP/M Card"](../../DEV.md#als-the-cpm-card-alscpmcard).

<a id="cards-017"></a>

## CARDS-017 — Hayes AT modem layer on the Super Serial Card

**Priority:** 🟢 · **State:** Ready — named software: ProTERM (research of 2026-11-18).

BBS use today goes through a WiModem232 / Zimodem-style modem: the
terminal sends AT commands and `ATDT host:port` opens a telnet session.
The SSC's telnet bridge exists; the AT command layer in front of it does not.

**Acceptance criterion:** ProTERM dials `ATDT` a local test telnet server,
exchanges text, and `+++` / `ATH` hangs up; loopback fence rules apply.

<a id="cards-018"></a>

## CARDS-018 — Synetix SuperSprite

**Priority:** 🧊 · **State:** Frozen — software and manual found
(research of 2026-11-19), not scheduled: the value is judged too low for
the cost of the display work.

TMS9918A + AY-3-8912 + TMS5220 (Echo ][ circuit) on one slot-7 card. POM2
already has all three kinds of chip: POM1's VDP (`TMS9918.*`, the E-Z
Color's), the AY (`Ay3_8910.h` + `AyPsgSynth.h`) and the TMS5220C
(`Tms5220.*`, the Echo+'s).

**Why it stays frozen (decision of 2026-11-19).** The media condition of
2026-11-18 is met, but the software is small: StarSprite I-III are authoring
tools (Ampersprite) and demos, and no commercial game is known to need the
card. Neither DIX nor `docs/test_corpus.md` uses it. The card itself is
cheap (about a day: register map, IRQ, AY + filters, speech). What makes it a
SuperSprite is the **overlay** of the VDP on the Apple's picture. That means
work in `Apple2Display` on the fresh-install display path (scope criterion 2):
a raster wider than 560, the VDP locked to the Apple's frame, and coexistence
with the GPU composite shader. Several days, with regression risk on core.

**Scheduling condition:** an explicit request with a use. For example, a
pom2games title that wants sprites + sound + speech on one card. A
"no-overlay" variant (VDP picture on the Card Monitor, as the E-Z Color) was
considered: it avoids the display risk but leaves every mixed-video demo
incomplete.

### Sources found (2026-11-19)

All in the Apple II Documentation Project,
`mirrors.apple2.org.za/Apple II Documentation Project/Interface Cards/Video/Synetix SuperSprite/`
(`Disk Images/`, `Manuals/`, `Photos/`). Third-party material: never tracked
and never packaged. A ctest that boots one goes through
`tools/test_media.sha256` + the archive.org test-media item (`pom2_media_20261119`), which
the user uploads.

| File | SHA-256 |
|---|---|
| `Manuals/Synetix SuperSprite Owners manual.pdf` (1983, 266 p., OCR text layer; also contains the StarSprite I and Echo ][ manuals; **no schematic**) | `f864160120b4671ab5939a5507225ad70751fd27402b3ddd7ec90c537460677d` |
| `Disk Images/StarSprite Demonstration.DSK` (DOS 3.3; DANCER, ALIENS, ARCADE, LOGO SPEECH, STATUS REGISTER USE…) | `60b84de2e59042283aed923374d1650ff9a9a3edaeec293448f34ab533c261e5` |
| `Disk Images/StarSprite I - Side A.DSK` | `f7616e7ece92f647c80f3fa022daee2e0601ace91acb0668424e7e80622a939b` |
| `Disk Images/StarSprite I - Side B.DSK` | `d71058d4cb81bcb40068c594ef07d9487c660decc22959dedfacb84fbd4b4171` |
| `Disk Images/Avant-garde creations - StarSprite III side A.dsk` | `e23d551f6dae2a0b6e949508c076f1898bcc3fbfa7340eac483ac6c5b20adb4f` |
| `Disk Images/Avant-garde creations - StarSprite III side B.dsk` | `03fc571257923e8429544056cf5095b1fa373f824b9d29d722acad30ae2454b7` |

All five disks catalog cleanly with `pom2_dos33 IMAGE ls`. **Not found:**
the Synetix "Natural Speech / Checkout Test" disk (its menu tests the
overlay, VRAM, the IRQ, the sound and the speech, so it would be the best
acceptance test) and StarSprite II.

Other implementations, both grade D:
- MAME `src/devices/bus/a2bus/ssprite.cpp` @ mame0289 (R. Belmont, Golden
  Child). Its register map is taken from the manual; it shows the VDP on a
  separate screen and models no video switch, no overlay and no filter.
- A2FPGA `hdl/supersprite/supersprite.sv` @ `71464b2c` (Ed Anuff). Its author
  says it was tested with all the original Synetix software. It implements
  the overlay but has no TMS5220 and no filters.

### What the manual settles

- **[B] Register map** (§ 7.2, Table 1, slot 7):

  | Address | Function |
  |---|---|
  | `$C0F0` | VDP VRAM read/write |
  | `$C0F1` | VDP register write (status read) |
  | `$C0F2` | TMS5220 data write / status read |
  | `$C0F3` | Video switch: APPLE VIDEO IN OFF |
  | `$C0F4` | Video switch: APPLE VIDEO IN ON |
  | `$C0F5` | Video switch: APPLE ONLY OUT |
  | `$C0F6` | Video switch: MIX VDP/EXTERNAL VIDEO |
  | `$C0F7` | VDP "write only / frame reset" |
  | `$C0F8-$C0FB` | Unused |
  | `$C0FC`, `$C0FD` | AY data write |
  | `$C0FE`, `$C0FF` | AY register write or data read |

  The OCR splits the `$C0FE`/`$C0FF` cell across two lines. MAME and A2FPGA
  both read it as "either address, write = register select, read = data".
  The manual's procedure (§ 7.5) writes the register number to `$C0FF` and
  the data to `$C0FD`.
- **[B] Slot 7 only** (§ 7.1). The card needs the Apple's composite sync
  (pin 19) and 3.579 MHz colour reference (pin 35), which only slot 7 carries.
  So the VDP is **locked to the Apple's video**: there is a "color lock"
  adjustment, a "horizontal alignment" pot, and a trouble-shooting entry
  "horizontal drift: SuperSprite is not locked to Apple video". The card
  cannot work on a PAL machine; this is POM2's inference from the NTSC colour
  reference, the manual does not say so.
- **[B] Line timing.** The VDP crystal is 3 × the colour subcarrier
  (10.738635 MHz, pixel clock /2) and the Apple's 14M clock is 4 ×. So one
  VDP line (342 pixel clocks) = 342 × 8/3 = **912 14M ticks** = one Apple
  line (65 CPU cycles including the long cycle; Sather). The lock is exact
  by construction. POM1's `TMS9918` counts a frame as 17067 nominal cycles;
  locked, it must take the Apple's 17030.
- **[B] IRQ** (§ 7.3). The VDP `/INT` goes to the bus IRQ (pin 30), 60 Hz, at
  the end of each frame; the handler restores A from `$45`. This is the
  wiring MAME has.
- **[B] Video switch use** (StarSprite I, "Initializing the SuperSprite", p. 48-49):
  - VDP only = `$C0F3` + `$C0F6`;
  - mixed = `$C0F4` + `$C0F6`;
  - Apple only = `$C0F4` + `$C0F5`, "without VDP blanking filter, which
    improves the display";
  - `POKE 49399,0` (`$C0F7`) "resets the VDP and centers Apple video".
- **[B] AY clock** 1.023 MHz (§ 7.5: "Tone Period = 1,022,727 / 16 ×
  pitch").
- **[B] 16 analogue filters on the AY output** (§ 7.5, Table 2). Neither
  MAME nor A2FPGA models them. Add 64 to R7 (port A as output), then write
  the filter number to R14 (port A). Bits 0-1 choose the type (0 high-pass,
  1 low-pass, 2 band-pass, 3 none); bits 2-3 choose the corner (400 / 800 /
  1600 / 3200 Hz). With R7 bit 6 clear the default is filter 15 (no filter),
  consistent with port-A pull-ups. **[F]** The filter order and Q are
  unknown; a scope or a recording of the card would settle them.

### Open: what is transparent

- **[B] Manual (StarSprite I, "Colors and Color Tables", p. 33-34):** "any color except
  transparent (0) … will prevent Apple video from showing through". It also
  says `&RED` disables the 9918's external video and "there is no provision
  on SuperSprite for external video feed".
- **[D] A2FPGA:** the card keys on the VDP's **black level**, so colour 1
  (black) lets the Apple through as well, and the 9918A's external-video mode
  is not used.
- **POM2's reading (to verify):** the 9918A has no transparency output pin,
  and a transparent pixel over a transparent backdrop leaves the chip as
  black. An off-chip key on the composite signal therefore cannot tell
  colour 0 from colour 1. This would make A2FPGA right and the manual's
  sentence a description of the intended use. **To check:** the luminance of
  colours 0 and 1 in the colour table of the TMS9918A/9928A/9929A data manual
  (the bibliography's), then a frame of a demo that draws colour 1 over
  Apple graphics.

### Geometry the overlay would need

- **[B] Pixel widths.** A VDP pixel is 8/3 of a 14M tick, so 256 VDP pixels
  = 682.7 half-dots, against the Apple's 560. The VDP picture is about 22 %
  wider than the Apple's.
- **[F] Alignment.** The Apple picture sits inside the VDP picture where the
  horizontal-alignment pot puts it; `$C0F7` "centers" it. The vertical
  alignment of the two 192-line rasters is also unstated. A photo of a mixed
  screen on a real card would settle both.
