# Detecting a printer without touching it

What a program can read from an Apple II printer or serial card to decide
"there is a printer here" **before** it writes anything — card by card, with
what each read costs (side effects), what can block, and how POM2 emulates it.
Written for A2 File Cmd's printing and VDrive detection (2026-09-26), meant to
be checked on real hardware.

Every fact carries its source. **MAME** means `mamedev/mame` master as of
2026-09-26 (commit `f663f942`), `src/devices/bus/a2bus/…` unless stated.
Byte values were computed from the real dumps with MAME's address mapping
applied, not copied from a document. "Unverified" means exactly that.

- [1. The one-line answer](#1-the-one-line-answer)
- [2. The Pascal 1.1 ID bytes, and why they are not enough](#2-the-pascal-11-id-bytes-and-why-they-are-not-enough)
- [3. Signature table](#3-signature-table)
- [4. Super Serial Card](#4-super-serial-card)
- [5. The 6551's handshake lines](#5-the-6551s-handshake-lines)
- [6. The //c serial ports](#6-the-c-serial-ports)
- [7. Grappler+](#7-grappler)
- [8. Grappler (1981)](#8-grappler-1981)
- [9. Apple Parallel Interface Card](#9-apple-parallel-interface-card)
- [10. Cards POM2 does not emulate](#10-cards-pom2-does-not-emulate)
- [11. The IIgs](#11-the-iigs)
- [12. POM2's controls: CLI, HTTP, library](#12-pom2s-controls-cli-http-library)

## 1. The one-line answer

| Card | "Is it a printer?" from | Side-effect-free? |
|---|---|---|
| Super Serial Card | DSW1 at `$C0n1`, bits `$03` = `$02` (printer mode) | yes |
| //c port 1 / 2 | the port's config in aux screen holes (`$047A` bit 0 / `$047E` bit 0 = 0 → printer) | needs RAMRD / 80STORE flipped (reversible) |
| Grappler / Grappler+ / Serial Grappler | `$Cn0C` = `$14` (class 1 = printer) | yes, but see the `$C800` claim and the Grappler+ bank reset |
| Apple Parallel Interface, Epson APL, 4th Dimension | no Pascal bytes: `$Cn00-$Cn02 = 18 B0 38` and `$Cn05/$Cn07 = 48 48` | yes (it re-enables autostrobe, harmless before a write) |
| IIgs port 1 | `$C02D` bit 1 = 0 (internal port) and BRAM copy `$E1:02C0` = `$00` (printer) | yes |

"Is a printer **attached and ready**?" is a different question — §5 (serial,
DCD/DSR) and §7-§9 (parallel, SELECT/BUSY/PE) — and on serial ports the honest
answer is often "a program cannot know without sending".

## 2. The Pascal 1.1 ID bytes, and why they are not enough

Apple II Miscellaneous Technical Note #8, "Pascal 1.1 Firmware Protocol ID
Bytes" (Nov 1988): `$Cn05 = $38`, `$Cn07 = $18`, `$Cn0B = $01`, `$Cn0C = $ci`
where `c` is the device class and `i` a unique ID; `$Cn0D-$Cn10` are the low
bytes of the PINIT / PREAD / PWRITE / PSTATUS entries.
([TN.MISC.008](https://mirrors.apple2.org.za/ground.icaen.uiowa.edu/Technotes/Tn/misc/TN.MISC.008))

The class nibble — **Apple IIe Technical Reference Manual, Table 6-6, p. 144**:

| `c` | Class |
|---|---|
| 0 | reserved |
| **1** | **printer** |
| 2 | joystick / X-Y input |
| **3** | **serial or parallel I/O card** |
| **4** | **modem** |
| 5 | sound / speech |
| 6 | clock |
| 7 | mass storage |
| 8 | 80-column card |
| 9 | network or bus interface |
| A | special purpose |
| B-F | reserved |

**The trap:** the Super Serial Card, both //c ports and both IIgs ports all
answer `$31` — class 3, whatever the card is set up for. Printer vs. modem on
those lives in DIP switches (SSC), screen holes (//c) or battery RAM (IIgs),
never in ROM.

## 3. Signature table

`page off` = file offset of the byte the CPU sees at `$Cn00`. `*` = the byte
at `$CnC0-$CnFF` follows the card's ACK latch on the Grappler+ family (MAME
`grappler.cpp:578-583`), so do not compare there. `†` = PIC-type PROM
addressing (A6 swapped, `a2pic.cpp:286-293`).

| Card | Dump (CRC32) | page off | 00 | 01 | 03 | 05 | 07 | 0B | 0C | 0D-10 | POM2 key |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Super Serial Card | 341-0065-A (`b7539d4c`) | `$700` | 2C | 58 | 70 | 38 | 18 | 01 | **31** | 8E 94 97 9A | `ssc` |
| //c port 1, ROM 255 | apple2c-16K (`f0edaa1b`) | `$100` | 2C | 58 | 70 | 38 | 18 | 01 | **31** | E4 EE F6 FB | built in |
| //c port 2, ROM 255 | same | `$200` | 2C | 58 | 70 | 38 | 18 | 01 | **31** | 11 13 15 17 | built in |
| //c port 1, 32K ROM 0/3/4 | apple2c-32Kv0 (`323952d1`) | `$100` | 2C | **89** | 70 | 38 | 18 | 01 | 31 | 9E A8 B4 BB | built in |
| //c port 2, 32K ROM | same | `$200` | 2C | 89 | 70 | 38 | 18 | 01 | 31 | 11 13 15 17 | built in |
| //c+ port 1 / 2 | apple2cp (`0b996420`) | `$100`/`$200` | 2C | 89 | 70 | 38 | 18 | 01 | 31 | as 32K ROM | built in |
| IIgs port 1 / 2, ROM 3 | apple2gs.rom | `$3C100`/`$3C200` | | | | 38 | 18 | 01 | 31 | 47 48 49 4A | — |
| Grappler+ 3.0 | `17cf5e02` | `$000` | 18 | B0 | 90 | 38 | 18 | 01 | **14** | 92 51 54 83 | — |
| Grappler+ 3.1 (POM2's `grappler_plus.bin`) | `c4781f97` | `$000` | 18 | B0 | 90 | 38 | 18 | 01 | **14** | 92 51 54 83 | `grappler` |
| Grappler+ 3.2, Buffered Grappler+ | `6f88b70c` / `cd07c7ef` | `$000` | 18 | B0 | 90 | 38 | 18 | 01 | **14** | 92 52 55 83 | — |
| Grappler (1981), slot 1 | eps-1 (`862773cb`) | `slot×$100` | 18 | B0 | 90 | 38 | 18 | 01 | **14** | 24 27 2A 33 | `grappler1` |
| Grappler (1981), slots 2-7 | same | `slot×$100` | 18 | B0 | 8D | **04** | **48** | `$Cn` | AA | varies | `grappler1` |
| Apple Parallel Interface, SW6 on | 341-0057 (`0a6b084b`) | `$100` † | 18 | B0 | 48 | **48** | **48** | 58 | FF | BA 68 68 68 | `pic` |
| Apple Parallel Interface, SW6 off | same | `$000` † | 18 | B0 | 48 | 48 | 48 | 58 | FF | BA 68 68 68 | `pic` |
| Apple Parallel Printer Interface (1977) | prom.b4 (`00b742ca`, MAME BAD_DUMP) | `$000` † | 18 | B0 | 48 | 48 | 48 | 58 | FF | BA 68 68 68 | — |
| Videx Uniprint | `e8423ef6` | `slot×$100` | 2C | 58 | 70 | 38 | 18 | 01 | **1D** | 18 1B 11 1E | — |
| Orange Micro Serial Grappler SG1.3 | `31edf479` | `$000` (unverified) | 18 | B0 | 90 | 38 | 18 | 01 | **14** | 9C 55 6B 93 | — |
| Epson APL | `ff695bef` | = PIC SW6-on (unverified) | | | | | | | | | — |
| Apple Synch Printer Interface (Silentype) | `bfdcf54d` | `$000` (unverified) | 2C | 58 | 70 | 38 | 18 | **48** | **98** | — | — |
| Prometheus PRT-1 | `54b0bff1` | `$000` (unverified) | 2C | 58 | 70 | 38 | 18 | 01 | **1A** | 11 1A 1F 28 | — |

Telling models apart once the class says "printer": the Grappler+ versions
differ at `$Cn0E/$Cn0F` (51 54 vs 52 55); the //c ROM revision shows at
`$Cn01` (58 vs 89); the SSC has high-ASCII "APPLE" at `$CnF9-$CnFE`; the PIC,
the Epson APL and the 4th Dimension card are byte-identical and **cannot** be
told apart by ROM. The Grappler+, the 1981 Grappler and the PIC all start
`18 B0 38` — Apple's Pascal 1.0 printer entry — so that prefix alone says
"parallel printer card of that era", nothing more.

## 4. Super Serial Card

**Registers** (MAME `a2ssc.cpp:394-409`, `read_c0nx`): `$C0n8-$C0nB` are the
6551 (A0-A1 decoded, `$C0nC-F` mirror). `$C0n0-$C0n7` are the DIP switches,
and **reading them has no side effect**: the value is `$FF`, ANDed with DSW1
when A1 = 0 and with DSW2 when A0 = 0 — so `$C0n1` = DSW1, `$C0n2` = DSW2,
`$C0n0` = both ANDed, `$C0n3` = `$FF`. Values are raw, not inverted.

**DSW1** (`a2ssc.cpp:99-122`):

| Bits | Switches | Meaning |
|---|---|---|
| `$F0` | SW1:4-1 | baud: `$10` 50, `$20` 75, `$30` 110, `$40` 134.5, `$50` 150, `$60` 300, `$70` 600, `$80` 1200, `$90` 1800, `$A0` 2400, `$B0` 3600, `$C0` 4800, `$D0` 7200, `$E0` 9600, `$F0` 19200, `$00` undefined |
| `$0C` | — | unused, read high |
| `$03` | SW1:6,5 | **mode**: `$00` communications, `$01` SIC P8 emulation, **`$02` printer**, `$03` SIC P8A emulation |

**DSW2** (`a2ssc.cpp:124-148`) — three switches change meaning with the mode:

| Bits | Switch | Communications mode | Printer mode |
|---|---|---|---|
| `$80` | SW2:1 | stop bits (set = 2) | same |
| `$20` | SW2:2 | data bits (set = 7, clear = 8) | delay after CR (set = none, clear = 1/4 s) |
| `$0C` | SW2:4,3 | parity: `$00` none, `$04` odd, `$08` none, `$0C` even | line width: `$00` 40, `$04` 72, `$08` 80, `$0C` 132 |
| `$02` | SW2:5 | end of line: clear = add LF after CR | same |
| `$50` | — | unused, read high | |

SW2:6 (interrupts) and SW1:7 (DCD connected) are **not** readable: MAME puts
them in a third, non-mapped port (`DSWX`, `a2ssc.cpp:150-157`).

**ROM reads:** `$Cn00-$CnFF` is the EPROM's last page; any `$CnXX` access
claims the shared `$C800` window (`take_c800() = true`, `a2ssc.cpp:50`), first
card wins until someone touches `$CFFF`. Harmless for detection, but a
detection loop that walks every slot's ROM should end with `LDA $CFFF`.

**What blocks:** the Apple firmware (not the card) waits, before every byte,
for `status & $70 == $10` (`$CAF5`) — transmitter empty **and** DSR and DCD
active. A `PR#n` on a communications-mode SSC with nothing on the line, or a
printer-mode one whose printer is off, hangs there without a timeout.

**In POM2:** Apple's EPROM when `roms/ssc_341-0065-a.bin` is present (else a
hand-written page with the same ID bytes). DIP banks settable per slot
(`--printer-port N:mode=…,dsw1=…,dsw2=…`, `/printer-port`, the Super Serial
panel), persisted as `ssc_dsw1_slotN` / `ssc_dsw2_slotN`. Defaults: slot 1 in
printer mode (`$FE`/`$7A`), other slots communications at 19200 8N1
(`$FC`/`$52`). Pinned by `ssc_firmware`.

## 5. The 6551's handshake lines

The 6551 status register (`$C0n9` on the SSC, `$C099`/`$C0A9` on the //c):
bit 7 IRQ, **bit 6 DSR**, **bit 5 DCD**, bit 4 TDRE, bit 3 RDRF, bits 2-0
errors. DSR and DCD bits are **set when the line is INACTIVE** (MAME
`mos6551.cpp:37-39` resets both to inactive). CTS is not in the register: an
inactive CTS stops the transmitter and **masks TDRE to 0** (`mos6551.cpp:286-289`).

**Side effect: reading the status register acknowledges the ACIA's interrupt**
(`mos6551.cpp` `read_status`: `m_irq_state = 0`). Harmless on a polled port;
on a port some interrupt-driven driver owns (ProTERM, an IIgs driver) it eats
that driver's interrupt. Reading `$C0n8` (RDR) clears the error bits and RDRF.

**What a printer looks like on the wire:** a serial printer signals "ready" by
asserting its DTR (DB-25 pin 20). On the usual printer (null-modem) cable that
reaches the computer's DCD and DSR; the Apple firmware requires both on the
SSC and DCD on the //c. Off line, out of paper or buffer full, the printer
drops DTR, both lines go inactive, and the firmware waits.

| Plugged in | DCD | DSR | CTS | Status bits `$70` |
|---|---|---|---|---|
| nothing | inactive | inactive | inactive | `$60` (TDRE masked) |
| printer, ready | active | active | active | `$10` |
| printer, off line / out of paper | inactive | inactive | active | `$70` |
| modem, no carrier | inactive | active | active | `$30` |
| modem, connected | active | active | active | `$10` |
| null modem (another computer) | active | active | active | `$10` |

So "status & `$60` == 0" says *something ready is on the line*; it cannot tell
a ready printer from a connected modem, and "nothing plugged in" reads exactly
like "printer off". MAME models an unplugged input as inactive; a real RS-232
receiver (MC1489) with an open input is only *typically* inactive —
unverified on hardware.

**In POM2:** `cable=` on `--printer-port` / `/printer-port`, per port, persisted
as `ssc_cable_slotN`: `auto` (POM2's historical model — lines active while a
telnet peer is connected or the printer tap is armed, CTS always active),
`none`, `printer`, `printer-offline`, `modem` (carrier = the telnet
connection), `null-modem`. A line change raises the DCD/DSR interrupt when DTR
is asserted. On a //c port only DCD comes from the cable (§6). Pinned by
`ssc_firmware` (per-cable status bits; a printer going off line mid-job and
back, through the real firmware) and `iic_printer_port`.

## 6. The //c serial ports

Port 1 is a 6551 at `$C098-$C09B`, port 2 at `$C0A8-$C0AB` (MAME
`src/mame/apple/apple2e.cpp` `apple2c_map` L3595-3604). No DIP switches.

**Configuration** (//c Technical Reference, 2nd ed., Tables 7-5 and 7-9): at
power-up the firmware copies ROM defaults into **auxiliary-memory** screen
holes; `PR#n` then copies them to main memory.

| Port | Aux holes | Default |
|---|---|---|
| 1 | `$0478` control reg, `$0479` command reg, **`$047A` flags** (bit 0: 1 = communications, **0 = printer**; bit 6 auto-LF; bit 7 echo), `$047B` line width | control `$9E` (9600, 8 data + 2 stop), command `$0B`, width `$50` (80) |
| 2 | `$047C`, `$047D`, **`$047E`** (bit 0 = 1: communications), `$047F` | `$16 $0B $01 $00` |

Reading aux `$04xx` from 8-bit code needs RAMRD on (or 80STORE + PAGE2) —
a soft-switch side effect, reversible. The aux holes survive Ctrl-Reset, not
power-on.

**"Printer ready":** the firmware sends only when status bit 4 (TDRE) = 1 and
**bit 5 (DCD) = 0**. Connector pin 5 (labelled DSR) is wired to the 6551's
**DCD** input; the 6551's own DSR is the disk port's EXTINT on port 1 and the
keyboard strobe on port 2; CTS is grounded. (//c Technical Reference ch. 3
pp. 103-105 and Table 11-37.) So on a //c only DCD tells you about the cable.

**Pascal ID bytes** are the SSC's (`$31`, §3), in every //c ROM.

**In POM2:** both ports are Super Serial Cards marked built-in (no EPROM, no
switches, `cable=` sets DCD alone). The printer tap may be armed on either
port: a printer on **port 2** (the modem port) prints, waits off line and
resumes through the real //c firmware (`iic_printer_port`, "port 2" case).
The ImageWriter takes the lowest-slot tapped port, so untick port 1's tap to
hear port 2.

## 7. Grappler+

**Status register** (MAME `grappler.cpp:699-707`): any `$C0nX` read returns

| Bit | Signal | Polarity |
|---|---|---|
| 7 | IRQ pending | 1 = pending |
| 6-4 | S1 printer-type switches | `101` Apple Dot Matrix/ImageWriter, `000` Epson (MAME default) |
| 3 | BUSY (pin 21) | 1 = busy |
| 2 | PAPER EMPTY (pin 23) | 1 = out of paper |
| 1 | SELECT (pin 25) | 1 = on line |
| 0 | ACK latch | 1 = last byte acknowledged |

Raw connector levels, no inversion. With **no printer on the cable** every
input is pulled up — BUSY, PE and SELECT all read 1 — and /ACK never pulses
(MAME `ctronics.cpp:56-73`).

**Side effects:** any `$CnXX` read (or write) resets the ROM bank to the low
2 KB (`grappler.cpp:578-591`); the card claims `$C800`. Reading `$C0nX` has
none. Writing `$C0nX` with A0 set selects the high bank; A1/A2 disable/enable
the ACK IRQ; `!(offset & 3)` strobes data — **don't**.

**Firmware 3.1 per state** (POM2's dump, disassembled; the output routine
calls `$CD84`, status read at `$CDE1`):

| State | Where it stops | What the user sees |
|---|---|---|
| ready | — | nothing |
| busy (SELECT high, no ACK yet) | ACK spin `$CD89-$CD95`, no timeout | nothing |
| **off line** (SELECT low) | `$CDB5`, then waits for SELECT `$CDCD-$CDD2`, no timeout, no key check | **flashing "NOT SELECTED" on row 10, columns 14-25, three beeps**; resumes by itself when SELECT returns and restores the row |
| paper out, SELECT dropped | as off line | as off line |
| paper out, SELECT still high | ACK spin, forever | silent hang — firmware 3.1 never reads PE |
| no printer | first byte goes (latch set at reset), second hangs in the ACK spin | silent hang |

The Operator's Manual (p. 3) says the card checks SELECT *and* PAPER EMPTY and
continues on RETURN; firmware 3.1 does neither. Either the manual describes
another revision, or it is loose.

**In POM2:** `online=`, `paper=`, `printer=`, `busy=`, `type=`. A byte strobed
while the printer cannot take it is held and delivered — with its ACK — when
it can (POM2's recoverable reading; a real printer ignores that strobe).
Pinned by `grappler_printer_state` (every state's bits; off line mid-job
through firmware 3.1, NOT SELECTED on row 10, resume).

## 8. Grappler (1981)

MAME `a2bus_grappler_device` (`grappler.cpp:223-446`), 2 KB EPROM eps-1.

- **Each slot has its own ROM page** (`rom[offset | slot<<8]`) — only slot 1's
  carries the Pascal signature; slots 2-7 read `$Cn05 = $04`, `$Cn07 = $48`.
  A detection routine that requires `$38/$18` misses this card outside slot 1.
- **Status** at any `$C0nX` with A0 set: open bus `& $F0` | BUSY<<3 | PE<<2 |
  SELECT<<1 | ACK latch — and here the latch reads **1 while a byte waits**
  for its /ACK, 0 once acknowledged (the opposite sense to the Grappler+).
- **Side effects of reads:** a read with A1 set asserts /STROBE, with A2 set
  releases it (`grappler.cpp:283-289`). **`$C0n1` is safe; `$C0n2`, `$C0n3`,
  `$C0n6`, `$C0n7` strobe the printer.**
- **Firmware:** checks SELECT *before* each byte (`$CBD2`); low, it writes
  "PRINTER NOT READY" (row 10) and "PRESS <CR> TO CONTINUE" (row 11), beeps
  three times and **waits for RETURN** (`$CC3E-$CC4A`), then checks again.
  It never reads its ACK latch: it waits for BUSY to drop (`$CBD9`).

**In POM2:** catalog key `grappler1`, ROM `roms/grappler_eps-1.bin` (fetched
from RetroBIOS `a2grappler.zip`). Pinned by `parallel_cards`.

## 9. Apple Parallel Interface Card

MAME `a2pic.cpp`, 512-byte PROM 341-0057 holding two firmwares, picked by
SW1:6 ("Parallel Printer" = 341-0005, adds LF after CR — the default;
"Centronics" = 341-0019, no LF). **No Pascal signature.**

- **Registers** (`a2pic.cpp:210-283`, A3 ignored): read `n3` = `$97` | PE<<5 |
  SELECT<<6 | /FAULT<<3; read `n4` = ACK latch<<7 | open bus `& $7E` | /ACK;
  **read or write `n6` enables the ACK IRQ, `n7` resets the mode** — side
  effects; write `n0` latches data and strobes, `n2` strobes again.
  **`$C0n3` and `$C0n4` are safe to read.**
- **PROM addressing** "Standard (X2)": A6 swapped, and `$CnC0-$CnFF` follow
  the ACK latch (`a2pic.cpp:286-293`).
- **Side effect of ROM reads:** any `$CnXX` access re-enables autostrobe
  (`a2pic.cpp:294-299`) — harmless before a write. No `$C800` claim.
- **Firmware:** waits on its ACK latch; an off-line printer holds the byte and
  the job resumes by itself.

Status by state (`$C0n3`): ready `$DF`, off line `$97`, paper out `$F7`, no
printer `$FF`.

**In POM2:** catalog key `pic`, PROM `roms/341-0057.bin` (RetroBIOS
`a2pic.zip`). Not modelled: strobe length/polarity switches, the 500 ns strobe
(MAME doesn't either), the X3/X5 jumpers. Pinned by `parallel_cards`.

## 10. Cards POM2 does not emulate

Signatures in §3; no driver in MAME, or one POM2 has not ported:

- **Buffered Grappler+** — ROM page identical to the Grappler+ 3.2; the buffer
  is an 8048 MCU (MAME emulates it). Tell it from a plain 3.2 by ROM: you
  can't.
- **Apple Parallel Printer Interface (Woz, 1977)** — MAME `a2parprn.cpp`,
  BAD_DUMP PROM. Differs from the PIC at `$Cn19` (47 vs 38) and `$Cn1E`.
- **Epson APL, Fourth Dimension** — PIC firmware; see §9.
- **Orange Micro Serial Grappler** — class `$14`, `$Cn0D-10 = 9C 55 6B 93`.
- **Apple Synch Printer Interface (Silentype)** — Pascal 1.0 only: no `$01`
  at `$Cn0B`.
- **Videx Uniprint** — class `$1D`, per-slot page like the 1981 Grappler.

## 11. The IIgs

POM2 has no IIgs. What an 8-bit program can read (sources: MAME
`src/mame/apple/apple2gs.cpp`, `z80scc.cpp`; Apple IIgs Hardware and Firmware
References; Technical Notes IIgs #18 and #30):

- **Slot 1 setting:** `$C02D` (SLTROMSEL), readable, bit n = slot n: **0 =
  internal port**, 1 = "Your Card". Bit 1 is the printer port, bit 2 the modem
  port (Hardware Reference Table 8-2; MAME L1681-1682).
- **Port type:** battery-RAM parameter `$00` (port 1) / `$0C` (port 2): `$00`
  printer, `$01` modem, `$02` AppleTalk. The firmware keeps a **RAM copy at
  `$E1:02C0 + param`**, readable from emulation mode with `LDA $E102C0`
  (`$AF $C0 $02 $E1`), no side effect (verified on ROM 3 only: the serial
  firmware reads it at `FF:0CEF`; ROM 01 unverified). The documented route is
  the toolbox `ReadBParam` from a native-mode excursion (TN IIgs #18). Talking
  to the clock chip at `$C033/$C034` works too but changes RTC state — don't.
- **The SCC** (Z8530): `$C038` channel B (modem) control, `$C039` channel A
  (printer) control, `$C03A/$C03B` data. A control read returns the register
  the pointer names **and resets the pointer**; the pointer is shared by both
  channels and set by a write. Reading data registers pops the FIFO. RR0 bit 5
  = CTS = connector pin 2 (HSKi, where the printer's DTR arrives), bit 3 = DCD
  = pin 7 (GPi, often unconnected). Recipe, not a documented guarantee:
  `PHP / SEI / LDA $C039 / LDA $C039 / PLP` (the second read is RR0). **Never
  write to recover the pointer.** If AppleTalk owns the port, don't touch it.
  With nothing plugged in the receivers are undefined (TN IIgs #30), and MAME
  forces CTS/DCD asserted on port 1 at reset — so neither answer is reliable.
- **Pascal ID bytes** at `$C1xx`/`$C2xx` = the SSC's (`$31`).

## 12. POM2's controls: CLI, HTTP, library

**Command line**

```
POM2 --slot 3=ssc --printer-port 3:mode=printer,cable=printer-offline
POM2 --slot 1=pic --printer-port 1:online=off
```

`--slot N=KEY` is a Slot Config Apply (persisted, rebuild). `--printer-port
N:k=v,…` applies to the live card; repeatable. Options (validated whole — one
bad key and nothing changes):

| Card | Options |
|---|---|
| Super Serial | `mode=comm\|printer\|sicp8\|sicp8a`, `dsw1=0xNN`, `dsw2=0xNN` (applied after `mode`), `cable=auto\|none\|printer\|printer-offline\|modem\|null-modem`, `tap=on\|off` |
| Grappler+, Grappler (1981), PIC | `online=on\|off`, `paper=ok\|out`, `printer=connected\|none`, `busy=on\|off`; `type=0-7` (Grappler+) |

**HTTP** (`--ai-control`, see DEV § AI control server):

| Endpoint | |
|---|---|
| `GET /printer-port[?slot=N]` | the printer side of one slot, or of every printer-capable slot |
| `POST /printer-port` `{"slot":N,"set":"k=v,…"}` | same option language; returns the new state |
| `POST /slot-log` `{"slot":N,"enable":1[,"capacity":C]}` | start logging slot N's bus accesses |
| `GET /slot-log?slot=N` | drain the log: `cycle`, `addr`, `value`, `op` (r/w), plus `writes` and `dropped` |
| `GET /printer/spool?slot=N[&from=K]` | bytes the card sent its printer since K (hex) and the next cursor |

**Library** (`libpom2_core_test.a`):

- `PrinterPortControl.h` — `describePrinterPort`, `applyPrinterPortOptions`,
  `printerPortJson`; the functions behind both doors above.
- `SlotBus::enableAccessLog / takeAccessLog / accessLogDropped` — every access
  to `$C0nX`, `$CnXX` and to `$C800-$CFFF` while the slot owns it, with the
  cycle, the address and the value. "Zero writes before the decision" is
  `std::count_if(log, isWrite) == 0` (pinned in `printer_detection`).
- `SuperSerialCard::setDipSwitches / setMode / setCable / inputLines`,
  `GrapplerCard::setOnline / setPaperOut / setPrinterConnected`,
  `CentronicsPrinter` (the PIC's and the 1981 Grappler's printer).
- `PrinterRender.h` — `savePrinterBytes(path, bytes)` and
  `renderPrinterBytesToPng(bytes, IwModel::…, prefix)`: any ImageWriter model
  or the Epson FX-80, one PNG per page, with nothing else linked.
