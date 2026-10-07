# Printer emulation, round two — more heads, and the LaserWriter

Follow-on to [`printer_plan.md`](printer_plan.md), which shipped the
ImageWriter faces, the screen dump, the three C. Itoh heads and the Epson
FX-80. That plan closed with four printers emulated. This one asks the
question the Grappler+ was already asking: **the card in slot 1 models a
seven-position printer-type DIP switch, so what are the other positions?**

**Status: all five items shipped**, with the Workstation Card at "boots,
configures LocalTalk and answers AppleShare's driver" rather than "carries
traffic" — see [§ 5](#5-the-apple-ii-workstation-card--it-boots) for exactly
what that means and what is left (rechecked against the code 2026-10-06: one
§ 5.3 step remains, the host-side LocalTalk endpoint; § 6 is not started).
The living description is [DEV § ImageWriter](../DEV.md#imagewriter-ii-printer-host-side)
for the heads and [DEV § Workstation Card](../DEV.md#apple-ii-workstation-card-workstationcard)
for the card.

## Table of contents

- [1. What the hardware actually supported](#1-what-the-hardware-actually-supported)
- [2. Epson generations and C. Itoh cousins ✅](#2-epson-generations-and-c-itoh-cousins-)
- [3. The LaserWriter, palier 1 — Diablo 630 ✅](#3-the-laserwriter-palier-1--diablo-630-)
- [4. The LaserWriter, palier 2 — PostScript by delegation ✅](#4-the-laserwriter-palier-2--postscript-by-delegation-)
- [5. The Apple II Workstation Card — it boots](#5-the-apple-ii-workstation-card--it-boots) · [5.1 the dump](#51-the-341-0358-a-dump) · [5.2 the memory map](#52-the-memory-map-the-dump-implies) · [5.3 what is left](#53-what-is-left-in-order)
- [6. Still proposed: the HP LaserJet](#6-still-proposed-the-hp-laserjet)
- [7. What changed against the plan](#7-what-changed-against-the-plan)

## 1. What the hardware actually supported

Two distinct questions hide inside "what printers did the Apple II support",
and keeping them apart is what makes this plan tractable.

**Parallel, through the Grappler+.** `GrapplerCard::PrinterType` already models
the real S1 DIP: Epson, NEC 8023 / C. Itoh 8510 / DMP 85, Star Gemini, Anadex,
Okidata 82A/83A/92/93, Apple Dot Matrix, Okidata 84. Those seven positions are
the families the Grappler firmware could emit graphics dumps for. Before this
plan only two of them (Apple Dot Matrix, Epson) matched a head POM2 actually
had; the rest produced the wrong-DIP garbage a real desk would show — correct,
and frustrating.

**Serial, through the Super Serial Card.** A different population entirely, and
the one the LaserWriter belongs to. POM2 already carries it: the SSC's printer
tap spools bytes with the same `drainSpoolFrom` shape a parallel card uses, and
`PrinterCoordinator` consumes it. **The transport for a serial printer was
already wired before this plan started** — which is most of why the LaserWriter
turned out to be affordable.

| Printer | Lineage | State |
|---|---|---|
| ImageWriter II / I, Apple DMP | C. Itoh | shipped in plan 1 |
| Epson FX-80 | ESC/P | shipped in plan 1 |
| C. Itoh Prowriter 8510A, NEC PC-8023A | C. Itoh | **shipped here** |
| Epson MX-80, MX-80 Graftrax+, RX-80 | ESC/P | **shipped here** |
| Apple LaserWriter (Diablo 630 mode) | Diablo | **shipped here** |
| Apple LaserWriter (PostScript) | delegated | **shipped here** |
| Star Gemini 10X | ESC/P + Star extensions | proposed |
| Okidata 82A/83A/92/93, 84 | Okidata + "Step II" graphics | proposed |
| Anadex DP-9500 | own | not worth it |
| HP LaserJet | PCL | proposed, [§ 6](#6-still-proposed-the-hp-laserjet) |
| Apple Silentype / Scribe | own interface card | out of scope |

## 2. Epson generations and C. Itoh cousins ✅

**The cousins are a table row each.** The Prowriter 8510A is the mechanism
Apple rebadged as the DMP, and the NEC PC-8023A is the same mechanism under
NEC's badge — which is precisely why the Grappler groups them on one DIP
position. They share the DMP's banks because they *are* the DMP's banks; what
differs is documented and pinned (the DMP's Apple-imposed firmware gaps, the
quoted carriage rate). Carried as separate rows rather than aliases, for the
reason the IW-I/DMP banks already are: a future divergence should land in the
table, not silently inherit.

**The Epson generations needed a mechanism, not just rows.** ESC/P grew feature
by feature across MX → RX → FX, so `IwModelProfile` gained `escPFeatures` and
the parser gates on it:

| | MX-80 | MX-80 Graftrax+ | RX-80 | FX-80 |
|---|---|---|---|---|
| `ESC K` 60 dpi | ✅ | ✅ | ✅ | ✅ |
| `ESC L/Y/Z` | — | ✅ | ✅ | ✅ |
| `ESC *` generic density | — | — | ✅ | ✅ |
| italics `ESC 4/5` | — | ✅ | ✅ | ✅ |
| master select `ESC !` | — | — | ✅ | ✅ |
| scripts `ESC S/T` | — | ✅ | ✅ | ✅ |
| proportional | — | — | — | ✅ |

A command the fitted head lacks is treated exactly like an unknown ESC —
dropped with its ESC, following bytes printing as text. That is what the
firmware did, and POM2 already reproduces wrong-DIP garbage on purpose
(`GrapplerCard::PrinterType`), because a driver aimed at the wrong head
*looking* wrong is the diagnosis.

**Graphics was the exception, and needed real work.** Dropping `ESC *` while
its data bytes still streamed would print a screenful of them, so `BitGraph`
gained a `swallow` flag. A density of 0 could not express this: `dotW =
1/horizDens` goes infinite and `fillDots` clamps that to a filled row.

**The screen dump had to follow.** It emitted `ESC *` unconditionally for any
Epson head — which an MX-80 prints as text. `buildScreenDumpEpson` takes the
command now, and the panel picks it from the fitted head's mask. This is the
kind of consequence a capability model surfaces and a bag of enum values
hides.

## 3. The LaserWriter, palier 1 — Diablo 630 ✅

**The historical key.** The LaserWriter's back-panel switch did not only offer
PostScript: it carried a **Diablo 630 emulation**, the daisywheel command set
of the era, so unmodified software could print text. Most Apple II word
processors had a 630 driver. That is how an Apple II actually got pages out of
one without speaking PostScript, over the serial port — the transport POM2
already had.

**It is a third parser over the same mechanism, not a new subsystem.** This was
planned as a new `LaserWriter.h/.cpp` with its own 300 dpi canvas and revised on
contact with the code: a Diablo 630 is fixed-pitch text on a page with no
graphics at all, and `ImageWriter`'s mechanism — page, plotter, margins, paper,
pacing, PDF export, print history — is already the right one. It is the FX-80's
situation exactly, and the FX-80's answer generalises. So `escP` became an
`IwLineage` enum: two lineages fit in a bool, three do not, and the bool was
already lying about what it meant.

**What is genuinely different is the model of motion.** A daisywheel has no
pitch and no line spacing; it has an HMI and a VMI, indices in 1/120 and 1/48
inch that the driver sets explicitly. Those land straight in the mechanism's
existing `hmi_`, which `printCharInternal` already documents as "the guest
saying move exactly this far, which outranks the font". No parallel state was
needed.

**Backspace overstrike falls out for free.** The 630 had no bold wheel: a driver
printed the character, backed up one HMI and printed it again. The page model
ORs ink, so the overstrike simply emerges.

**Deliberately conservative on the grammar.** Only unambiguous encodings are
decoded; everything else falls through to "unknown ESC, dropped with its ESC",
which is what the firmware did. Guessing a parameter COUNT wrong does not lose
one command — it desynchronises the rest of the job. And the LaserWriter's
emulation was a subset of the real 630 anyway, so a conservative subset is
closer to the machine than an eager one. `ESC FF` (form length) and `ESC L`
(bottom margin) are named in the source as the two left out, with the reason.

**The face is a substitute and says so.** A real LaserWriter set Courier from
its own font ROM, which POM2 has no dump of. Same honesty as the CP437 fallback
the ImageWriter ROM banks replaced: the shape of the page — pitch, margins,
motion — is right, the letterforms are borrowed.

## 4. The LaserWriter, palier 2 — PostScript by delegation ✅

**Do not write an interpreter.** PostScript is not a command set. It is a
Turing-complete stack language with a full graphics model: paths and Bezier
flattening, even-odd and non-zero winding fills, clipping, halftones, and Type 1
fonts that arrive encrypted and hinted. An interpreter would be larger than the
whole rest of POM2's printer subsystem and would never be faithful — and the
failure mode of an almost-right one is a page that is *subtly* wrong, which is
worse than no page.

**So run somebody else's.** `ChildProcess` supervises Ghostscript exactly as it
supervises the FujiNet helper — the pattern was already in the tree.

**Licensing, stated where it can be found.** Ghostscript is AGPL and POM2 is
GPLv3. Running a **separate process is not linking**, so this is an optional
runtime dependency rather than a derived work: POM2 ships nothing of
Ghostscript, detects it at runtime, and degrades to a message naming the Diablo
mode as the alternative. `PostScriptRender.h` says not to turn this into a
library binding without revisiting that.

**PGM, not PNG.** `-sDEVICE=pgmraw` is a five-token ASCII header and raw bytes —
thirty lines to parse. Asking for PNG would mean carrying a PNG *decoder* to
read back what POM2 only ever writes.

**`-dSAFER` is not optional.** The job comes from emulated software, and
PostScript can open and delete host files.

**Off the UI thread.** `PostScriptSpooler` owns a guarded worker and a mailbox.
Rendering inline would freeze the window the way mounting an image under
`stateMutex` used to freeze the machine. It joins its worker outside its own
mutex — the same shape, and the same reason, as `SpOverSlipLink::stop()` — and
`reset()` waits for a render rather than abandoning an unreaped child.

**A serial line has no end-of-file**, so end-of-job is the Ctrl-D drivers send,
with a 1.5 s idle flush for the ones that simply stop. Bytes after the Ctrl-D
start the next job rather than being lost.

**`adoptRenderedPage` is the seam.** The page model was always an intensity ramp
(`indexToRgb`) and merely had no source of greys before, so anti-aliased
PostScript text keeps its edges instead of being thresholded to one bit.

## 5. The Apple II Workstation Card — it boots

The card is built: its 65C02 runs Apple's own firmware, passes the power-on
self-test and configures the 8530 for LocalTalk. What is left is the handshake
between the two CPUs and SDLC framing — § 5.3. This section keeps its history
because the blocker moved three times (a missing dump, then missing MAME
source, then the discovery that the board is a coprocessor), and each move
changed what "support for this card" meant.

**What it is.** The Apple II Workstation Card put a IIe on LocalTalk so it could
reach an AppleShare server and the LaserWriters hanging off the same net —
which is how a LaserWriter was normally connected, the serial port being the
fallback. On board: a **Zilog 8530 SCC** doing LocalTalk's SDLC framing, and a
ROM carrying the card's half of the AppleTalk stack.

**What it needs, in order:**

1. **The card's ROM dump.** ~~Not in `roms/`~~ — **a dump has since been
   identified** (see [§ 5.1](#51-the-341-0358-a-dump)), which lifts this one.
   It mattered because POM2 could not synthesise around it the way it does for
   SmartPort: there the firmware entry contract is *published* in the Apple II
   Reference Manual, so real ProDOS drivers call a documented dispatch.
   AppleTalk has no equivalent published slot-firmware calling convention —
   guest software reaches the card through the 8530's registers and through
   Apple's own ROM entry points, so a synthesised interface would be one no
   actual Apple II software knows how to call.
2. ~~**An 8530 SCC device.**~~ **Shipped** as `Scc8530Device`, a MAME port of
   `z80scc.{h,cpp}` pinned by `scc8530_smoke`. Independently useful, as
   predicted — the Mac and IIgs serial ports are the same chip.
3. **The protocol stack above it**, to whatever depth the goal needs. For
   reaching a LaserWriter that is LLAP → DDP → ATP → **PAP** (Printer Access
   Protocol); for AppleShare it is also NBP and ASP. Note that on real hardware
   much of this lives in software the guest loads from disk, not in the card —
   so the emulation seam is genuinely at the chip, not at the protocol.

**What it would buy, given palier 2 already shipped:** an *alternative
transport* for the same PostScript that the serial path already carries. That
is the honest accounting — the LaserWriter works today over the SSC, and this
would make it work the way most sites actually wired it.

**Recommended shape when the ROM is available:** LLE at the chip (2), real ROM
(1), and let the guest's own AppleTalk software drive it — matching what
`docs/lle_vs_hle.md` says about picking a level. An HLE at the protocol layer
is the tempting shortcut and is the wrong one here, for the reason in (1).
§ 5.2 shows why the seam is even lower than that paragraph assumed: the card
is a coprocessor, so "let the guest's software drive it" is really "let the
*card's* firmware drive it, and let the guest talk to the card".

### 5.1 The 341-0358-A dump

A 64 KiB dump (sha1 `59c8e8c8…`, crc32 `0x63819dcb`) has been identified as
this card's firmware. The identification is from the CONTENTS, not from a
part-number database — which is the stronger evidence, and worth writing down
so a future reader can re-check it rather than trust it:

| Evidence | Offset | What it establishes |
|---|---|---|
| `STA $C080,X` / `STA $C08F,X` | several | Slot-card firmware. `$C080,X` with X = slot×16 is *the* Apple II device-select idiom; the sixteen registers behind it are where the 8530 sits. |
| `THOMAS EAGER WROTE THE LAP DRIVERS` | `0x0DB7F` | **LAP = LocalTalk Link Access Protocol.** This ROM carries the link layer. |
| `Apple //e Boot` | `0x0D5A8` | The netboot path — a //e booting from an AppleShare server, which is what this card was for. |
| `%%IncludeProcSet IWEm 1 1` | `0x04A1D` | A LaserWriter print path. `IWEm` is the **ImageWriter Emulator** procset: the card sends PostScript that asks the LaserWriter to render an ImageWriter-shaped page. |
| 8 × 8 KiB banks, all distinct | — | 64 KiB of real content, banked — no mirroring, so the whole dump is live. |
| No Pascal 1.1 signature, no ProDOS block trio, at any 256-byte alignment | — | Correct, and it means less than it looked like — see § 5.2. An AppleTalk card is neither a Pascal device nor a ProDOS block device, so the absence is what a *correct* `$Cn00` page for this card looks like. |

The "8 × 8 KiB banks, all distinct" row is the one to distrust: bank 3 (file
`0x6000-0x7FFF`) is `$FF` fill apart from six bytes of vector table. § 5.2
replaces that partitioning with the map the code actually implies.

**What remains after the ROM.** Item (2), the 8530 SCC, **is now written** —
`src/Scc8530Device.{h,cpp}`, a MAME port pinned by `scc8530_smoke`
(→ [DEV § Z8530 SCC](../DEV.md#zilog-z8530-scc-scc8530device)). Item (3) does
*not* take care of itself the way this section used to claim: MAME does not
model SDLC either, so the framing LocalTalk runs on has no oracle. That is
now the honest remaining gap, and § 5.3 says what is left in order.

### 5.2 The memory map the dump implies

Read out of the dump with a 65C02 disassembler, and stated with the evidence
so it can be re-checked rather than trusted.

**This is an intelligent card: it has its own 65C02.** The image is 65C02
code (`STZ`, `BRA`, `TSB`, `TRB` throughout) with its own vector table, its
own RAM and its own I/O — not a slot ROM the Apple II executes. That single
fact reorganises everything below it, and it is why the card was never going
to be a `SlotPeripheral` with a ROM in it.

| Card-CPU address | What | Evidence |
|---|---|---|
| `$0000-$6FFF` | RAM | The reset routine sizes it by writing `$40 $41 $42 $43` to `$0000/$2000/$4000/$6000` and reading them back — the classic aliasing probe for an 8/16/32 KB fit. |
| `$7000-$7FFF` | I/O, decoded in 256-byte selects | Every absolute reference in the whole dump that falls in this page lands on `$7x00`: `$7000`, `$7500-$7503`, `$7800`, `$7900`, `$7A00`, `$7B00`, `$7C00`. |
| `$7500-$7503` | **Zilog 8530 SCC**, wired A1 = A//B, A0 = D//C | `LDA #$03 / STA $7502 / NOP / NOP / LDA $7502` at `$EE13` is a poll of RR3, which exists in channel A only; `$10` then `$30` to `$7500` are the WR0 Reset External/Status and Error Reset commands. Nothing else in an 8-bit design looks like that. |
| `$7A00` | a **five-bit** latch | The POST writes `$FF` into it with a `DEC $7A00` and then compares against `#$1F` (`$F131-$F139`). The other selects read back all eight bits. |
| `$7000` | status / progress code | Written with `$03`, `$0B`, `$0F`, `$07`, `$04` at each phase boundary, and `$04` immediately before the halt loop. |
| `$8000-$FFFF` | ROM, 32 KB window | Two 32 KB images share it. The firmware never writes above `$8000` — checked over 2.5 M instructions. |

**The banking is one bit, not three.** File `0x8000-0xFFFF` is a single
coherent image at `$8000-$FFFF`: its `JSR`/`JMP` targets stay inside that
window (142/235 in the top 8 KB alone, and the strays go to the other three
quarters of the same window), its vector table at `0xFFFA` gives
NMI `$ED53` / RESET `$C000` / IRQ `$EE0F`, and all three land on code that
disassembles cleanly — the IRQ handler at `$EE13` is the SCC poll above, and
RESET `$C000` is the RAM-sizing probe. File `0x0000-0x7FFF` is a *second*
32 KB image for the same window, self-consistent the same way, empty above
`$E000`. Both are live: the LocalTalk strings (`THOMAS EAGER WROTE THE LAP
DRIVERS`, `Apple //e Boot`) are in the upper image, the LaserWriter one
(`%%IncludeProcSet IWEm 1 1`) is in the lower. Which of the `$7x00` latches
selects the half is **not yet established**; `$7000` and `$7C00` are the
candidates, since the reset routine rewrites both between two attempts at the
RAM probe.

**The Apple II side of the firmware is in the image, at a fixed offset.**
This is the part the earlier note got backwards. File `0xC400-0xC4FF` is a
textbook slot `$Cn00` page and `0xC800-0xCFFF` its `$C800` expansion ROM:

```
C400: A0 04     LDY #$04        ; entry 1
C404: A0 02     LDY #$02        ; entry 2
C408: 48 98 48 A9 00 2A A8 68   ; entry 3, Y from carry
...
C41B: A9 60     LDA #$60        ; the "which slot am I in?" trick:
C41D: 85 FB     STA $FB         ;   put an RTS at $00FB,
C41F: 20 FB 00  JSR $00FB       ;   call it, and read the return
C422: BA        TSX             ;   address off the stack
C423: BD 00 01  LDA $0100,X     ; A = $Cn
C426: 2C 18 C0  BIT $C018       ; RD80STORE
C42A: 8D 00 C0  STA $C000       ; 80STORE off
C42D: 2C 14 C0  BIT $C014       ; RDRAMWRT
C431: 8D 04 C0  STA $C004       ; RAMWRT main
C434: 8D F8 07  STA $07F8       ; claim $C800 for slot n
C443: AE FF CF  LDX $CFFF       ; release everyone else's expansion ROM
C44C: 0A 0A 0A 0A / AA          ; X = slot × 16
C451: A9 71     LDA #$71
C453: 9D 80 C0  STA $C080,X     ; the host↔card mailbox
C466: 4C 00 CC  JMP $CC00       ; into the expansion ROM
```

Every idiom in that block is Apple II host code, and none of it means
anything in the card CPU's own address space (`$C004` is ROM there). So the
card serves these bytes onto the Apple II bus while its own CPU treats them
as data. The offsets are the image's own: the page was assembled at `$C400`
and the hardware ignores the slot bits, which is why it works in any slot —
the `$Cn` in the code is discovered at run time, never assumed.

That also disposes of the "no Pascal 1.1 signature" worry. There is no
signature because there should not be one: this is neither a Pascal character
device nor a ProDOS block device. Software finds it as an AppleTalk card —
note the `ATLK` string at `0xC518` — not by scanning `$Cn05`/`$Cn07`.

**What the host actually pokes.** The sixteen `$C08x,X` registers are the
mailbox between the two CPUs: the `$Cn00` page writes `$71` to `$C080,X` and
`$C081,X`, the expansion ROM writes `$50` to `$C080,X` and hits `$C08F,X`.
The *semantics* of those bytes are not established, and cannot be by reading
the host side alone — they are answered by firmware running on the card.

**The firmware runs, and it validates the SCC.** The map above is not a
reading — it was executed. Driving the upper 32 KB with POM2's `M6502` over a
flat test bus, with `Scc8530Device` at `$7500` and the other selects modelled
as latches, the firmware completes its **power-on self-test** and goes on to
program the chip. Two things fall out of that, and neither could be got from
reading:

* **The clocks are derived, not guessed.** The POST's SCC test is a 255-byte
  loopback ping-pong on *both* channels inside a fixed 8000-poll budget
  (`$F000-$F09E`: `$0105`/`$0106` count down, `$0107`/`$0109` are the send
  cursors, `$0108`/`$010A` the expected-receive cursors). The budget puts a
  **ceiling** on how fast the card CPU may be relative to the SCC: against a
  3.6864 MHz chip clock it passes at every rate tried up to 2.0 MHz and fails
  from 2.05 MHz up, because a faster CPU burns the 8000 polls before the 255th
  byte arrives. That bounds the ratio from one side only — it says the card
  CPU is at most ~2 MHz, not that it is 1 MHz. Independently, the firmware's
  final configuration is WR12/WR13 = 6 with WR4 selecting the x1 clock, and
  `3686400 / (6 + 2) / 2` is exactly **230400** — the LocalTalk bit rate. So
  **/RTxC and PCLK are a 3.6864 MHz crystal**, and that one is pinned from
  both sides.
* **Where it ends up.** WR4 = `$20` (SDLC, x1 clock), WR3 = `$DD` (receiver
  enabled, 8 bits, address search, hunt), WR11 = `$F2` (receive clock from the
  DPLL, transmit from the BRG), WR14 = `$41` (BRG enabled from /RTxC), 230400
  bit/s. That is a LocalTalk node, configured by Apple's own driver, against
  POM2's chip.

This is pinned as `scc8530_workstation_firmware`
(`tests/scc8530_workstation_firmware_test.cpp`), ROM-gated so it skips
cleanly. Read the header comment there before changing it: the harness is
deliberately **not** a card emulation — it borrows `Memory` in flat test mode
and shims the I/O page by decoding effective addresses around each step,
precisely so this evidence did not have to wait for step 2 below.

### 5.3 What is left, in order

1. ~~An 8530 SCC device.~~ **Done** — `Scc8530Device`, MAME-cited, pinned
   twice: against the datasheet and against this card's own firmware.
2. ~~Give `M6502` a bus that is not `Memory`.~~ **Done**, and it cost the
   Apple II nothing. `Memory::ForeignBus` folds into tests the bus paths were
   already making rather than adding any — `docs/PERFORMANCE.md` § 9 has the
   shape and the numbers, and § 8's measurements are why the two obvious
   designs were never attempted.
3. ~~Build the card.~~ **Done** — `WorkstationCard`, catalog `workstation`,
   pinned by `workstation_card_smoke`. The firmware boots on the card's own
   65C02, passes its POST and configures LocalTalk at 230400 bit/s with the
   card in a real slot.
4. ~~Map the host handshake.~~ **Done** (CHANGELOG 2026-08-31, "The
   handshake works, and it was one wrong number") — and not at `$C0nX`: the
   card patches the host's own code in the `$Cn00` page (the `JMP` operands
   at `$CnBB`/`$CnBC`, the block-move operands, `SEC` over `CLC` to release
   a spin), with data through `$CnEA` and `$02A9` as the handshake.
   `WorkstationCard.h` § THE HANDSHAKE has the map; the `$C0nX` strobes stay
   unmodelled on purpose (`WorkstationCard.h:82-92`). AppleShare IIe
   Workstation's ATINIT passes the card's diagnostics and reaches its menu;
   `workstation_card_smoke` services a driver call the way ATINIT makes one.
   Original notes, kept for the history: This is what remains between the
   card and a working AppleTalk guest: the driver writes `$71` to `$C080,X`
   and `$C081,X` and `$50` from the expansion ROM, and the card firmware has
   **no interrupt path** for any of it (`$EE07` counts anything that is
   neither SCC nor timer as spurious), so the handshake is presumably a poll
   of the shared page. `WorkstationCard::hostStrobeLog()` records what a guest
   does; that plus an AppleTalk disk is the way in.
5. ~~SDLC framing.~~ **Done** — from the Zilog manual rather than MAME,
   which does not model it; marked `SDLC (datasheet, not MAME)` at every site.
   With it, the card's firmware acquires a LocalTalk node address and
   transmits: `0B 0B 81` is lapENQ, and once it holds `$0B` it broadcasts
   short DDP datagrams.
6. ❌ **A host-side LocalTalk endpoint.** Not started (2026-10-06): the seam
   exists and works (`Scc8530Device::setFrameCallback` / `receiveFrame`,
   `Scc8530Device.h:193,202`) but nothing outside the chip and its tests
   installs a callback; note the card disables its receiver
   while transmitting, so an endpoint must wait for WR3 D0 before answering.

Step 1 is independently useful whatever happens to the card — the same chip is
the Mac and IIgs serial port — and it is the best-evidenced part of the whole
peripheral stack, because Apple's own driver signs off on it.

## 6. Still proposed: the HP LaserJet

Not started; recorded so the analysis is not lost.

The LaserJet was parallel, so it *did* hang off a Grappler+ — but the Grappler
had no DIP position for it, so its graphics dumps produced text garbage on one,
which POM2 would reproduce for free. In text it worked with no driver at all;
serious software (AppleWorks 3.0, Publish It!) drove it with real PCL.

PCL is a fourth lineage and, unlike the Diablo, genuinely does not fit the
matrix mechanism: a LaserJet composes a whole page at 300 dpi and ejects it on
`ESC E` or a form feed. Phases would be PCL text (`ESC E`, orientation,
margins, pitch) then PCL raster (`ESC *t#R`, `ESC *r#A`, `ESC *b#W` with
compressions 0/1/2), which is what the AppleWorks driver actually emitted.
Downloadable soft fonts are not worth implementing — consume `ESC )s#W` and
`ESC (s#W` cleanly instead.

## 7. What changed against the plan

**The LaserWriter did not need its own files.** Planned as `LaserWriter.h/.cpp`
with a separate 300 dpi canvas; delivered as a third parser over the existing
mechanism. Reading `printCharInternal` is what settled it — `hmi_` was already
exactly a Diablo HMI, and everything below the command layer was already right.
The separate-canvas plan would have duplicated the paper, the PDF export and the
print history to gain nothing.

**`escP` had to become an enum.** A bool for "which of two grammars" does not
survive a third, and it was already misnamed for what it decided.

**A capability mask is not free.** `escPFeatures` looks like pure data until a
gated command has a BODY: `ESC *`'s data bytes had to be swallowed rather than
dropped, and the screen-dump builder had to learn which command the fitted head
can answer. Both are consequences the table surfaced, and neither was in the
plan.

**Ghostscript is a runtime dependency, not a build one**, and the test says so
out loud: most of `postscript_render` runs with no interpreter installed, and
the branch that needs one renders for real and asserts a drawn box lands where
PostScript put it — because a blank page satisfies "ok" and would hide a wrong
device or a wrong geometry.
