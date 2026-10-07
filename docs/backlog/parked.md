# Backlog — Parked projects

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
| [PARKED-001](#parked-001) | 🧊 | Frozen | The MAME `a2bus` backlog — what is worth porting, and why. |
| [PARKED-002](#parked-002) | 🧊 | Frozen | E-Z Color / TMS9918 card |

<a id="parked-001"></a>

## PARKED-001 — The MAME `a2bus` backlog — what is worth porting, and why.

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-1582). Original dates and estimates are retained below.

- 🔵 **The MAME `a2bus` backlog — what is worth porting, and why.** *(survey,
  not a task)* <a id="a2bus-backlog"></a>The Workstation Card cost what it did
  **because MAME does not have it**. That is the criterion for everything
  below: a card MAME already models is a port with an oracle; a card it does
  not is reverse engineering. `src/devices/bus/a2bus` crossed against POM2's
  actual gaps:

  **The four that earn their keep.**

  - ✅ **Videx VideoTerm** (`a2videoterm`) — **done 2026-09-29**, catalog
    key `videoterm` (][ / ][+, slot 3, ROM-gated on RetroBIOS's
    `a2vidtrm.zip`): `VidexVideotermCard` + `Hd6845Crtc`, its 720 × 216
    picture shown while TEXT + AN0, `PR#3` pinned on the real firmware by
    `videx_videoterm_boot` → [DEV § Videx Videoterm](../../DEV.md#videx-videoterm-videxvideotermcard).
    Left: the other MAME character sets (APL, Epson, French, German,
    Katakana…) as a setting, and the cousins `a2ultraterm`, `suprterminal`,
    which the `CardVideoSource` seam was shaped to take.
    *(Was: the clearest functional hole — a II/II+ had no 80 columns at
    all; the card that made word processors and CP/M usable on a II+.)*
  - **Mountain Computer Music System** (`a2mcms`) — the real blind spot for
    an emulator that already has Mockingboard A/C, Sound II, Phasor, SSI263
    and a stereo bus. 16 digital voices, the Apple II's first polyphonic
    synth, and an architecture with nothing in common with the AYs. Plays
    straight to the project's strength.
  - **E-Z Color Graphics Interface** (`ezcgi`) — a **TMS9918 in an Apple II
    slot**: hardware sprites on a machine that has none. Re-ranked after
    looking at POM1: the expensive part is already written there, and better
    than MAME's, with 31 k lines of original software behind it. Scoped,
    estimated and parked → [§ E-Z Color](parked.md#parked-002).
  - ~~**Applied Engineering TransWarp** (`transwarp`)~~ **done** —
    `TranswarpCard`, pinned by `transwarp_card`. The estimate held (the
    `cyclesPerFrame` plumbing did the work) but the shape was wrong twice:
    there are no "cache semantics" to model — the board has no cache, it has
    a bus watcher that drops to 1 MHz around slot and paddle accesses — and
    it is not a speed latch in a slot, because it decodes nothing
    slot-relative at all. `$C072`/`$C074` are global, so it needed a bus
    snoop hook rather than a device-select handler.

  **Quick wins, a few hours each.**

  - ~~**4play** (`4play`)~~ **done** — `FourPlayCard`, pinned by
    `fourplay_card`. It was not a shift register: `read_c0nx` returns one
    byte per player and `device_start()` is empty. **SNES MAX** (`snesmax`)
    is still open and is the larger of the two — its controller is serial, so
    the card clocks a latch/shift protocol rather than exposing four ports.
    Both are modern homebrew, so their value is the current Apple II scene
    (and `pom2adventure`), not a period catalogue.
  - **TimeMaster H.O.** (`timemasterho`) — the other common clock beside the
    ThunderClock+ POM2 already has.
  - ~~**Apple Parallel Interface Card** (`a2pic`)~~ **done 2026-09-26** —
    `AppleParallelCard`, catalog key `pic` (ROM-gated), shipped together with
    the 1981 Grappler (`grappler1`, `GrapplerClassicCard`), both driving a
    shared `CentronicsPrinter`; pinned by `parallel_cards`
    (CHANGELOG 2026-09-26, "Same day — printer detection, for A2 File Cmd").
    *(Was: the third printer lineage beside the Grappler+ and the synthetic
    card.)*
  - **Memory Expansion Card / RamFactor** (`a2memexp`) — a slot RAM disk,
    distinct from the AUX-slot RamWorks POM2 has. Gives a II+ a RAM disk.

  **Heavier, only if the appetite is there.**

  - **Apple II SCSI / High-Speed SCSI** (`a2scsi`, `a2hsscsi`) — fidelity
    rather than capability, since CFFA and the synthetic HDV already cover
    the need. The point would be running software that talks to the real
    card.
  - **The Mill** (`a2themill`) — a 6809 coprocessor, OS-9 on an Apple II.
    Same shape as the Workstation Card, so **`Memory::ForeignBus` is already
    there for it** (PERFORMANCE § 9).
  - **PC Transporter** (`pc_xporter`) — a whole 8086. MAME has it; it is a
    project in itself.

  **Deliberately skipped.** LANceGS (two Ethernet cards already), the Z80
  variants (`a2applicard`, `softcard3`, `titan3plus2` — the Microsoft SoftCard
  covers it), IEEE-488, ComputerEyes, and the modern storage cards (`a2sd`,
  `booti`, `sider`) that FujiNet + CFFA + HDV already cover.

  **What a hardware archive adds that MAME does not.** Not schematics —
  **DIP-switch and jumper positions with their meanings**. That is exactly
  what POM2 already models for the Grappler+ (its seven printer-type
  positions) and the SSC (its two blocks), and for a Videx or a TransWarp it
  is the source MAME lacks.

<a id="parked-002"></a>

## PARKED-002 — E-Z Color / TMS9918 card

**Priority:** 🧊 · **State:** Frozen.

**Scheduling condition:** a named software requirement or an explicit request; define acceptance tests before implementation.

**Evidence:** [original report](../archive/todo-2026-10-05.md#source-2307). Original dates and estimates are retained below.

Things with a clear shape and a reason that have no slot. Under the
[scope ruling](../decisions/project-scope.md#the-scope-ruling) everything here is **frozen**: it leaves this
section when a named piece of software needs it, and not before. The estimate
below is kept because it was done properly and re-costing it would be waste —
not because the work is queued.

### E-Z Color Graphics Interface — a TMS9918 in an Apple II slot

<a id="ez-color"></a>**Estimate: ~3.5-4 days** for a working card
(TMS9918A only). *Card only* — `tmspaint` and `tmssprite` stay in POM1.

MAME has it (`src/devices/bus/a2bus/ezcgi.cpp`, Steve Ciarcia, *BYTE* August
1982 — a construction article, not a product), so the usual "MAME is the
oracle" rule half-applies. Only half, and this is the unusual part: **POM1
already has a better TMS9918 than MAME's.** `pom1/src/TMS9918.{h,cpp}` is
2 586 lines with a `ChipType` dispatch across TMS9918A / 9929A / 9118 / 9128 /
9129 / T7937A / T6950, modelling the Toshiba clones' suppression of sprite
cloning ("Bug N°8"); MAME models plain `tms9918a`. So the oracle here is
POM1 + the datasheet + POM1's own silicon tests, and **that must be written
down at the porting sites** the way SDLC framing is marked
`SDLC (datasheet, not MAME)` in `Scc8530Device` — otherwise a future reader
goes looking in MAME and finds something *less* accurate, which is the worst
kind of trap.

**Why it is worth doing at all**: the Apple II has no sprites, no VRAM of its
own, and colour only as an NTSC artefact. This card brings 16 KB of dedicated
VRAM, 32 hardware sprites and 15 commanded colours. And the software problem
that sinks most curiosity cards does not apply — POM1 carries **31 067 lines
of original 6502 assembly** for this chip (Rogue 6 777, a logo/scroller 5 426,
Galaga 5 127, Maze3D 4 315, plus Sokoban, Snake, Chess, Mandelbrot, Life,
Plasma, Nyan Cat). Porting those to the Apple II is a separate, later job and
is **not** in the estimate below.

**What makes the port cheap, checked rather than assumed:**

- The VDP port addresses are canonical in one place —
  `pom1/dev/lib/tms9918/tms9918.inc`, `VDP_DATA = $CC00` / `VDP_CTRL = $CC01`
  — so POM1's 6 079-line asm library is address-agnostic above two symbols.
- `BeamClock.h` is 63 lines, a pure header depending only on `<cstdint>`, and
  its own comment already names POM2 as an intended consumer.
- Both projects clock at 1 022 727 Hz exactly. No retiming.
- `pom1::Peripheral`'s pure-virtual surface is `name()` alone; everything else
  has a default. Stripping it costs almost nothing.

**What is not free:**

- `SnapshotIO.h` differs between the projects (273 vs 183 lines), so
  `serialize`/`deserialize` must be rewritten against POM2's
  `appendSnapshotState` / `loadSnapshotState` pair. The *content* is already
  enumerated in `TMS9918::Snapshot`, so it is mechanical.
- The beam/CPU sync (`renderBeamCatchUp`, `syncSpriteScanToBeam`) is tied to
  how the host feeds cycles, and is the part to read carefully rather than
  transplant.

**Breakdown:**

| | |
|---|---|
| Import + decouple the VDP core (2 586 lines + diagnostics + BeamClock), snapshot rewrite, build wiring | ~1 d |
| `EzCgiCard : SlotPeripheral` — offset 0 ↔ VRAM, offset 1 ↔ register/status, `$FF` elsewhere, `advanceCycles` feeding the beam. Slot-agnostic by construction | ~3 h |
| Display path — a second source composited into POM2's framebuffer. **The risk item**: not the rasterising, but the interaction with `NtscPostProcessor` / `CrtEffectStack` / the display-mode menu. The VDP's output is RGB and must NOT go through the composite shaders | ~1-1.5 d |
| Tests — a subset of POM1's ten (sprite status, per-scanline, silicon-strict) plus a card smoke at `$C0nX` | ~0.5 d |
| Catalog, plug site, docs (CLAUDE map, DEV section, README, CHANGELOG) | ~0.5 d |

**Deliberately out of v1**: the `ezcgi_9938` / `ezcgi_9958` variants (V9938 /
V9958 with an IRQ line back to the Apple II). MAME itself annotates their
clocks "typical … not verified".

**The standing objection, which grows with every shared file.** `hgrpaint/` is
already duplicated between POM1 and POM2 with no shared build. Adding the VDP
puts ~2 700 more lines in both trees, and a silicon-behaviour fix will then
have to be applied twice with nothing to flag the omission. That is
survivable — `hgrpaint` proves it — but at this volume the question "shared
module or verbatim copy?" deserves deciding on its own, not during a port.
→ [`a2bus` survey](parked.md#parked-001)
