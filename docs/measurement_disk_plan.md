# One measurement disk for real machines — inventory and plan

*(2026-11-27. Inventory by four review agents over the `[F]`, `[E]` and `[D]`
citations in `src/`, the open questions in `TODO.md` § 3, `docs/backlog/`,
`docs/audits/` and `DEV.md`, and prior art. Nothing here is built yet.)*

POM2's reference is the silicon, and grade A — measured on real hardware —
is the strongest evidence (CLAUDE.md § Conventions). Many constants are still
F (tuned, unknown), E (no source) or D (another emulator says so). The people
who own a real Apple II do us a favour: we ask them for **one boot of one
disk**, no question to answer, and a phone photo of the screen. This file
records what that disk must measure, what it must not try, and how the
results come back.

People who can run it: French Touch (Grouik, fenarinarsa — //e PAL, //c PAL),
owners of a ][+, a //c NTSC, a //c Plus. **Not Uncle Bernie**: he owns an
Apple-1, not an Apple II.

## 1. What the disk measures (software only, stock machine)

Sorted by value to POM2. "Source" names the code or document the answer
settles.

| Priority | Measurement | Machines | How (sketch) | Output | Source |
|---|---|---|---|---|---|
| **High** | **The real PAL CPU clock.** Candidates: 1 015 625 (POM2), 1 016 966 (MAME, carries a FIXME), 1 017 889 (crystal ÷ 14), ~1 015 300 (one hobbyist reading). Also the NTSC average | //c PAL, //e PAL (NTSC as control) | (a) the 6551's own 1.8432 MHz crystal: 19200 8N1, send ~4000 bytes polling TDRE, count CPU cycles → f = cycles × 19200 / (10 × bytes), ~100 ppm. Built in on the //c; an SSC elsewhere. A //c port with nothing plugged may hold the transmitter (see next row) — fall back to (b)/(c). (b) a No-Slot Clock or ThunderClock if present: frames across ~20 s of RTC ticks, ~30 ppm. (c) optional long run: a frame counter on screen, photographed with a phone stopwatch at start and ≥ 60 min later | `CLK=1015xxx` | `src/CpuClock.h:136-174` |
| **High** | //c built-in serial ports: are `$C09C-F`/`$C0AC-F` mirrors of the 6551; do `$C090-7`/`$C0A0-7` decode nothing (floating bus); with nothing plugged, do DCD/DSR read active or inactive (the schematic says active, MAME and AppleWin inactive) | //c, //c+, //c PAL; any SSC for the lines | reads only, never the data register; read twice synced to video to tell floating bus from a fixed value | 2 hex rows per port + `DCD DSR CTS` | `SuperSerialCard.cpp:930-942, 1015-1040` |
| **High** (PAL //e) | Does AN2 switch the font on a two-set European 8 KB character ROM, and from which line | //e PAL FR/DE/UK; US as control | fill text with the 96 glyphs twice; at a cycle-counted line from VBL write `$C05D`, back with `$C05C` at the next VBL | **a photo** (+ the keyboard's charset switch position) | `Memory.h:733-734` [E] |
| Med-High | Where mid-line switches land on the //c (PAL and NTSC) and the ][+, and ALTCHAR (MAME and AppleWin disagree) | ][+, //c, //c PAL; //e as control | vapor-lock (VBL / VBLINT; ][+ floating-bus lock), flip PAGE2, HIRES, TEXT, MIXED, ALTCHAR at a fixed cycle in bands over a column ruler | **a photo** | `docs/audits/parite-apple2video-2026-11-01.md:370-376, 430-440` |
| Medium | Power-on RAM pattern, per machine (main, aux, language card banks; Saturn / Slinky if present) | all | **stage 0**, before anything touches RAM or `$C08x`: classify each page `$09-$BF` (all 00 / all FF / 00FF alternating / FF FF 00 00 / other) + first 8 bytes of a few pages; needs a power-off ≥ 30-60 s | ~5 text lines | `Memory.cpp:798-808` [D][E][F], `SaturnCard.cpp:51`, `SlinkyCard.cpp:73`, DEV.md § Power-on RAM pattern |
| Low-Med | Language Card state at power-on: bank and write-enable (Sather says bank 2, read ROM, write enabled — grade B; MAME uses bank 1) | ][+ with a 16K card; //e, //c | stage 0: `$C011`/`$C012` first, then probe | `LC B2 WE` | MEMORY-003 (stale: already B, see § 4) |
| Medium | Disk II: revolution period and jitter; the read amplifier on a track with no flux (WOZ/AppleWin 30 % ones vs MAME 50 %); motor coast and spin-up; where the platter is after a stop | all with a Disk II, //c internal | time address fields over 64 turns (~13 s); **read** an unformatted quarter track (35.25+) — no write; motor off 1.5 / 3 s then restart | `REV=…±… SECT σ=…`, `NOISE ones=…% distinct=…`, `SPINUP=…ms COAST=+n sect` | `DiskImageWozFlux.cpp:565` [F], `DiskIICard.cpp:1225-1238` [F][D], `DiskImage.cpp:1992-1995`, `DiskIICard.cpp:1195`, `DiskIICard_Cycles.cpp:24,152` |
| Medium | //c opposing-magnet response while the motor coasts (VALIDATION-001) | //c (any Disk II as bonus) | motor on then off; PH1 on, wait N cycles, PH3 on, all off; read the track. N = 4 … 512 | track deltas per N | `DiskIICard::seekPhaseW`, `kStepperResponseCycles` |
| Medium | ][+ floating bus spot checks and the HBL "$1000 phantom" (published measurements are //e only) | ][+ | memory encodes its own address, lock to the beam, sample ~9 H/V spots (as FLOATBUS) | 9 hex bytes | `parite-apple2video:439` |
| Low, cheap | Machine identity: ROM ID bytes (`$FBB3 $FBC0 $FBBF $FB1E`), CRC-32 of `$D000-$FFFF` and `$C100-$CFFF`, the ][+ `$FFFE` byte (`$40 $FA` vs the bad dump's `$59 $FF`), character ROM | all | reads only, FIRST (labels every other result) | one header line | `RomErrata.h:32-36` |
| Low, cheap | CPU: NMOS / NCR / Rockwell / WDC (decimal-mode Z flag, RMB0); `$5C` = 8 cycles? dummy reads of `JMP (a)`, `$5C`, `$DC`/`$FC`; NMOS ANE/LXA magic constant (POM2 uses `$EE`) | all | never execute KIL/STP/WAI | `CPU=… 5C=8c ANE=EE` | `M6502.h:404-407, 471`, DEV.md:40-43, 190-200 |
| Low | //c+: `$C060` bit 7 with nothing connected; speed right after boot | //c+ | read 16×; cycles per frame | `C060=… SPD=…` | `Memory.cpp:1418` [F] |
| Optional (card found) | Mockingboard AY: what is stored when Port A changes while WRITE (or LATCH) is held — POM2 re-strobes, MAME keeps the first byte, AppleWin ignores it. The AY reads back, so software settles it | Mockingboard A/C, //c with a 4c | latch R2, WRITE with PA=`$55`, change PA to `$AA` while held, INACTIVE, read back | `AY WRITE-held:LEVEL/EDGE` | `Ay3_8910.h:178-190` [F][D] |
| Optional | SSC: `$CF00` read; `$CFFF` under INTCXROM — does the card keep its `$C800` claim; 6551 TX interrupt with nothing to send | SSC (//e family) | select via `$Cn00`, read `$C800`, `$CF00` ×8 | `KEEP=Y/N CF00=xx TXIRQ=n/s` | `SuperSerialCard.h:470` [F], `Memory.cpp:2273-2283`, `SuperSerialCard.cpp:792-800` |
| Optional | RamWorks: what an absent bank reads, and whether a write reads back | //e with RamWorks | FindBanks over `$00-$7F` via `$C073` | bank bitmap + `ABS=xx WR=xx` | `Memory.cpp:884-896` [F][D] |

Two facts are better settled by archived disk images than by a volunteer:
the DOS 3.3 VTOC byte `$00` (`DiskBuilder.cpp:348`) and the ProDOS directory
header access byte (`ProDOSDiskFs.cpp:71`).

## 2. Not on the disk

**Needs an instrument** (scope, logic analyser, audio capture, colorimeter,
ruler, frequency counter): IOU presetting IOUDIS at reset; PAL picture aspect
(`CpuClock.h:262-283`); P31 phosphor colour; //c drive motor pitch; 6850
transmit phase; TMS9918 CPU/beam phase; the speaker cone (AUDIO-012); SSI263
voice (AUDIO-011); VidHD (DISPLAY-001); Le Chat Mauve palette and pots
(DISPLAY-011); the PAL divider chain; POM2's own Disk II sequencer vs real
flux (needs Applesauce / Greaseweazle); Mockingboard levels; DISPLAY-012.
Keyboard auto-repeat is measurable but needs the volunteer to hold a key:
left out.

**Needs a rare card**: Mockingboard 4c across Ctrl-Reset, Slinky RESET,
`$C800` release for ThunderClock / Slinky / Workstation / Videoterm /
UltraTerm, Liron RESET, Passport MIDI (6840/6850), Videoterm / HD6845,
UltraTerm, E-Z Color, Uthernet II, SCC, FASTChip, TransWarp CPU, SuperDrive
in a ][+, Z80-card DRAM at power-on, Le Chat Mauve power-on registers, Porte
Parole, No-Slot Clock across a power loss, Diablo 630.

**Not hardware questions**: STORAGE-018's true LSS clock (waits for a title),
CPU-001's DIX differential (compares emulator cores), VALIDATION-003/004
(need the original disks / 13-sector PROMs).

## 3. Already settled — do not measure again

`$C019` / //c VBLINT edge vs the floating bus ([A] arekkusu42 BEAMPOS, //e
and //c, PAL and NTSC); //c VBLINT = //e edge + 192 lines; //e floating bus
(FLOATBUS, Champailler's PAL traces); //e mid-line placement of PAGE2,
HIRES, 80COL, AN3, TEXT, MIXED ([A] DIX, AppleWin GH#656, logic analyser);
frame geometry 65 × 312 = 20280 / 65 × 262 = 17030; NMOS `LSR abs,X` = 7
cycles; 6522 T1 read-back (mb-audit + TRIBU); `$C060` = 1 with no cassette;
LC write-enable on NMOS vs 65C02 RMW (AppleWin #700); //c IOUDIS after reset
([B] IIc TRM); ][/][+ RESET keeps the key strobe ([B] Sather Fig. 7.7); Disk
II read pulse = one LSS cycle.

## 4. Stale documentation found on the way

`docs/backlog/cpu-memory.md` MEMORY-003 and `src/Memory.h:1620` say "no
primary source" for the Language Card power-on bank; the audit
`docs/audits/parite-apple2-2026-11-01.md` quotes Sather, *Understanding the
Apple II* p. 5-29/30 (grade B): read ROM, write enabled, pre-write reset,
bank 2. A hardware run only confirms it.

## 5. How the disk runs

- **Boot**: our own loader (`pom2_disk boot`, `DiskBuilder.h` — no Apple
  code; works with Apple's boot PROM, POM2's own and the //c's), payload at
  `$0900-$1FFF`, so RAM above stays untouched for the power-on stage.
- **Order**: stage 0 power-on snapshot (RAM classes, LC state, before any
  `$C08x`) → identity (reads only) → CPU → timing → memory probes (the only
  writes, to RAM) → card probes (only cards identified by their ROM) → Disk
  II reads → the optional long clock run, last.
- **Warm vs cold**: after stage 0, a signature + run counter at `$BF00`;
  found at the next boot → "WARM — power-on results invalid".
- **Crash containment**: each result shown as soon as measured; the record
  and "current test" kept at a fixed address; the reset vector `$03F2` points
  at a resume routine, so Ctrl-Reset marks the test HUNG and goes on.
- **Never writes to a disk.** The no-flux test reads an unformatted quarter
  track (proposed; the alternative — erasing a scratch track — needs the
  notch open and was left out).
- **Target**: under a minute after power-on.
- **3.5" / block version** (//c+ internal drive, CFFA, Floppy Emu HD mode):
  later — a block-0 loader calling the `$CnFF` driver; `DiskBuilder` is
  5.25" only today.

## 6. How the results come back

- **Primary: a QR code** on HGR page 2, version 6-M (41 modules, ~106
  bytes), modules 5 × 4 px (Apple pixels are ~0.91 as wide as tall) =
  225 × 180 px with a 2-module white quiet zone, all bit 7 equal. Payload:
  version byte, ID bytes, result fields, CRC-32; Base45 (RFC 9285) in
  alphanumeric mode. The 6502 does only Reed-Solomon (GF(256) tables) and
  interleaving; the host precomputes the module order. Start from Jentzsch's
  6502 QR generator (git.applefritter.com/6502/6502-QR-code-generator,
  GPLv3 — check the LICENSE). More results → several QR pages, numbered.
- **Fallback: text page 1**, one check digit (CRC) per line; SPACE switches
  between the two.
- **Visual tests** (AN2, mid-line bands) are photos of their own screens,
  numbered "PHOTO 2/3".
- **Owner side**: one field-definition file generates the ca65 `.inc` and the
  decoder's names; a script (zxing-cpp / pyzbar) decodes the photo, checks
  the CRC and prints the table. The same disk runs in POM2 (`pom2_headless
  … --screenshot`) through the same decoder: POM2's answer in a column next
  to each real machine; a ctest checks POM2 decodes its own QR. Results are
  archived under `docs/audits/` as grade-A evidence, one file per machine and
  date.
- Rejected: writing results back to the disk (only Floppy Emu/SD users could
  send it), a typed code (too long), audio through the cassette port (the
  //c has none).

Volunteer instructions (to print on screen and in the e-mail):

1. Switch the Apple II off for a minute. Insert the disk in drive 1 (or
   choose `POM2-MESURE` on your Floppy Emu).
2. Switch it on and touch nothing for about a minute.
3. When the large square code appears with DONE, photograph the whole
   screen, facing it. If it says "1/2", press SPACE and photograph the next.
4. If nothing changes for a minute, press CTRL-RESET once: it goes on.
5. E-mail the photo(s). Thank you!

(French: Éteignez l'Apple II une minute · Mettez la disquette dans le
lecteur 1 · Allumez, ne touchez à rien pendant une minute · Quand le grand
code carré apparaît avec TERMINE, photographiez tout l'écran, bien de face —
« 1/2 » : ESPACE puis la suivante · Rien ne bouge pendant une minute : une
fois CTRL-RESET · Envoyez la ou les photos. Merci !)

## 7. Prior art

- a2audit (zellyn, MIT) — LC and aux test sequences; github.com/zellyn/a2audit
- Apple Misc. Tech Note #7 (ROM ID bytes); Apple IIc Tech Note #9 (VBL on the //c)
- French Touch `loader.a` (GPL-3, grade A sync; forces PAL, does not detect it)
- Total Replay `4cade.init.a` (MIT) — machine detection ideas
- dos33fsprogs `vapor_lock.s` — floating-bus sync on the ][+ (check licence)

## 8. Decisions still open (owner)

1. No write to the volunteer's disk at all — recommended.
2. The long PAL clock run (an hour, a second photo) offered as an option at
   the end.
3. First version: the High and Medium rows of § 1, optional cards when found.
