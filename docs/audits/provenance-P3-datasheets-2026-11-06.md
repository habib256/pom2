# Provenance audit P3: datasheet claims (2026-11-06)

Tranche P3 of `docs/audits/BRIEF-provenance.md`. Sites were listed with
`grep -rniE "datasheet|data sheet" src/ --include='*.cpp' --include='*.h'`,
which gives **152 lines in 31 files**.

The tranche ran under a **20-minute time box** (21:21 to 21:41, UTC+4).
Datasheets for the core-accuracy chips came first, and the network chips were
left for later (see Bilan). Every document cited below was **downloaded and
read for this audit**. Nothing below comes from memory.

## Documents opened

| Document | Where it came from | Used for |
|---|---|---|
| WDC W65C22 (W65C22N/S) data sheet, rev. history to 2010 | westerndesigncenter.com `/wdc/documentation/w65c22.pdf` | `Via6522.h` |
| WDC W65C51N data sheet, March 18, 2024 | westerndesigncenter.com `/wdc/documentation/w65c51n.pdf` | `SuperSerialCard.cpp` |
| NEC *Integrated Circuits for Consumer Use* (1983), µPD1990AC pp. 792-805 | bitsavers `components/nec/_dataBooks/` | `ClockCard.*` |
| Dallas *1992 Product Data Book*, DS1216E pp. 5-22..5-30 | bitsavers `components/dallasSemiconductor/_dataBooks/` | `NoSlotClock.*` |
| Hitachi *U70 8/16-bit Multi-Chip Microcomputer Data Book* (1985), HD6845S pp. 451-463 | bitsavers `components/hitachi/_dataBooks/` | `Hd6845Crtc.*` |
| Hitachi HLN014 *HD46505R CRT Controller User's Manual* | bitsavers `components/hitachi/_dataSheets/` | cross-check, `Hd6845Crtc.h` |
| TI *TMS9918A/TMS9928A/TMS9929A VDP Data Manual* (Nov 1982) | bitsavers `components/ti/TMS9900/` | `TMS9918.*` |
| GI *1982 Microelectronics Data Catalog*, AY-3-8910 pp. 5-18..5-23 | bitsavers `components/gi/_dataBooks/` | `Ay3_8910.h`, `AyPsgSynth.h`, `Mockingboard.cpp` |

Documents I looked for and did **not** obtain:

- **SSI 263A data sheet.** I ran two web searches and checked the Kassel lab
  sheet, which is not the data sheet.
- **Zilog SCC/ESCC User Manual UM010902.** zilog.com now redirects to a
  Littelfuse JavaScript page.
- **Rockwell R6551 / Synertek SY6551 data sheet.** I did not search for it
  within the time box.

## Registre

Pages are the page labels printed in each document, not PDF page indexes.

| `file:line` | Claim (abridged) | Grade | Source found | Verdict |
|---|---|---|---|---|
| `Via6522.h:138` | RES clears every register except the T1/T2 latches and counters and the SR | B | W65C22 § 3.9 "Reset (RESB)", p. 36: *"Reset clears all internal registers (except T1 and T2 counters and latches, and the SR"* | CONFIRMÉE (the comment named the R6522, which was not opened) |
| `Via6522.h:359` | T2 has "the identical load/decrement silicon" as T1 | B→ inference | W65C22 § 2.9, p. 19: T2 one-shot *"is similar to T1"*. § 2.6, p. 17: T1 *"will start counting down on the next PHI2 clock following the load"* | IMPRÉCISE: the sheet says "similar". The one-cycle load is stated for T1 only |
| `Via6522.h:393` | An ORB store moves no pin; the only write strobe is CB2 in handshake or pulse mode | B | W65C22 § 2.2: *"PB lines … (CB1, CB2) handshake data on Write operations only"*. § 2.4 covers write handshake | CONFIRMÉE |
| `Via6522.h:532-631` | One-shot: PB7 low after the T1CH load, high at the first zero count, and only once | B | W65C22 § 2.6, p. 17: *"PB7 will then go to a Logic 0 following the load … and will go to a Logic 1 again when the counter reaches a zero count"* | CONFIRMÉE. The code toggles only on the armed underflow |
| `Via6522.h:639` | VICE: the one-shot reload is "contrary to the datasheet" | — | W65C22 § 2.6 says nothing about a reload after a one-shot underflow | DÉCORATIVE: it quotes VICE. The behaviour rests on [A] mb-audit T6522_9 |
| `Via6522.h:66` | Register layout per the W65C22 | B | W65C22 Table 2-1 (register select) | DÉCORATIVE (a header) |
| `SuperSerialCard.cpp:730,741` | DCD/DSR changes raise an IRQ unless IRD = 1 | B | W65C51N (Mar 2024) **p. 10**, "Data Carrier Detect (Bit 5) and Data Set Ready (Bit 6)", quoted verbatim | IMPRÉCISE: the code says **p. 13**. Page 13 of that revision is the Command Register and PMC table |
| `SuperSerialCard.cpp:1066` | Only a status read releases the IRQ; an RDR read does not | B | W65C51N p. 10: Interrupt (Bit 7) *"is cleared when the Status Register is read"*. RDRF (bit 3) clears on an RDR read | CONFIRMÉE |
| `SuperSerialCard.cpp:1107` | DCD/DSR status bits are active-low | B | W65C51N p. 10: *"A 0 indicates a low level (true condition) and a 1 indicates a high level (false)"* | CONFIRMÉE (it quotes AppleWin, but the WDC sheet agrees) |
| `SuperSerialCard.cpp:672` | The PMC table reads "Mark parity bit transmitted, parity check disabled" | E (for this audit) | Not in the W65C51N, whose PMC rows (p. 13) all read *"Use but no parity"*. The R6551/SY6551 sheet was not opened | INTROUVABLE |
| `ClockCard.h:246` | Time Set presets the 40-bit register and holds the counter | B | NEC 1983, p. 797, quoted verbatim | CONFIRMÉE |
| `ClockCard.cpp:216` | Command table C2 C1 C0. "Groups '0' and '1' hold their functions independently" | B | NEC 1983, p. 796 | CONFIRMÉE |
| `ClockCard.cpp:224` | "leap year is not automatically performed" | B | NEC 1983, p. 795 | CONFIRMÉE (page added) |
| `ClockCard.cpp:232` | NEC documents no power-on TP rate | F | Not stated on pp. 792-805 as far as they were read. The absence was not checked exhaustively | Kept as F |
| `ClockCard.cpp:144` | Battery-backed "per uPD1990AC datasheet, V_BATT operation" | B | There is no V_BATT section. p. 802 shows a power-supply circuit with VDD held up by **3 × Ni-Cd** behind a power-down protection circuit. p. 793 gives VDD = 2.0-5.5 V | IMPRÉCISE (corrected) |
| `ClockCard.cpp:370` | A "40 Bit Shift Register", not 48 | B | NEC 1983, pp. 793 and 797 | CONFIRMÉE |
| `NoSlotClock.cpp:83` | Reserved bits read 0 | B | DS1216E p. 5-23, ZERO BITS: *"will always read logic 0"* | CONFIRMÉE |
| `NoSlotClock.cpp:89` | Data-book location, pp. 5-22..5-25 | B | The sheet spans pp. 5-22..5-30. The quoted material is on 5-22..5-24 | CONFIRMÉE |
| `NoSlotClock.cpp:104` | Bit 5 = oscillator, "logic 1 … turned off". Both bits ship as 1 | B | p. 5-23, OSCILLATOR AND RESET BITS | CONFIRMÉE |
| `NoSlotClock.cpp:114` | AM-PM/12-24 mode bits | B | p. 5-23 | CONFIRMÉE |
| `NoSlotClock.cpp:217`, `.h:51` | A read during pattern recognition aborts and resets the pointer | B | p. 5-23 | CONFIRMÉE |
| `NoSlotClock.cpp:266`, `.h:178` | After the match, A2 = 0 cycles write A0 into the register bit | B | p. 5-23: *"either receive data on Data In (A0) or transmit data on Data Out (DQ0), depending on the level of READ/WRITE (A2)"* | CONFIRMÉE (page added) |
| `NoSlotClock.h:142` | Comparison register C5 3A A3 5C ×2, LSB first, Figure 1 p. 5-24 | B | p. 5-24, "SMARTWATCH COMPARISON REGISTER DEFINITION Figure 1" | CONFIRMÉE |
| `NoSlotClock.h:30` | Protocol read off the Dallas sheet | B | As above | CONFIRMÉE |
| `Hd6845Crtc.h:36`, `.cpp:38-45` | RES keeps R0-R17 | B | U70, p. 451 *"Control registers … are not affected and remain unchanged"* and p. 463. HLN014 says the same | CONFIRMÉE |
| `Hd6845Crtc.h:87` | Interlace sync & video mode: a row is R9 + 2 rasters ("RN-2 shall be programmed") | **D→B** | U70, p. 454, R9 in Interlace Sync & Video mode: *"total number of rasters is RN, RN-2 shall be programmed"* | CONFIRMÉE, raised from a quotation in MAME to the book. **Note**: HLN014 (HD46505**R**) gives "(RN-1) … RN must be even" for the same mode. That is a different part, so it does not affect POM2's HD6845S |
| `Hd6845Crtc.cpp:46` | The address register's state after RES is unknown | F | Not looked for in U70 | Kept as F |
| `TMS9918.cpp:608` | "TI datasheet §2.2: The 5S bit is cleared to 0 after …" | B | Manual **§ 2.3.3**, p. 2-11. The quote was paraphrased | IMPRÉCISE (section and quote corrected) |
| `TMS9918.cpp:641` | F goes high at VBlank, "datasheet §3.5.1" | B | Manual **§ 2.3.1**, p. 2-11: *"set to 1 at the end of the raster scan of the last line of the active display"*. § 3.5 is "External VDP Operation" | IMPRÉCISE (section corrected) |
| `TMS9918.cpp:1771`, `:1909` | 5S latches only while F = 0 | B | § 2.3.3, p. 2-11: *"five or more sprites on a horizontal line … and the frame flag is equal to a 0"*. The quoted sentence ("5th sprite detection is only active…") is not the manual's wording | CONFIRMÉE (substance). The quote was replaced |
| `TMS9918.cpp:1396`, `:1894`, `:616` | F, 5S and C are sticky until a status read | B | §§ 2.3.1-2.3.3, p. 2-11 | CONFIRMÉE |
| `TMS9918.cpp:897`, `:1548` | Text mode: 6 px border left, 10 px right | B | Table 3-3 "Screen Display Parameters", p. 3-8: Text left border 19 vs 13, right 25 vs 15 | CONFIRMÉE (location added) |
| `TMS9918.h:594` | "TI datasheet ~8 µs rule". "openMSX is the source of truth" | B / framing | § 2.1.5, p. 2-4: *"approximately 8 microseconds to fetch the VRAM byte following the last data transfer"* and *"can vary from 2 to 8 microseconds"* | Claim CONFIRMÉE. The **framing contradicts the graded rule**, so it is reworded: the D slot table is followed because the B text gives a bound, not a schedule |
| `TMS9918.cpp:8` | "Reference: TMS9918A datasheet" | — | — | DÉCORATIVE |
| `Ay3_8910.h:91` | The mask-programmed high-nibble chip select is 0000 | B | GI 1982, p. 5-19: *"Unless otherwise specified, address bits DA7--DA4 are programmed to recognize only a 0000 code"* | CONFIRMÉE (page and quote added) |
| `AyPsgSynth.h:39`, `Mockingboard.cpp:36` | Tone = clock / (16·TP) | B | GI 1982, p. 5-21: *"first counting down the input clock by 16, then by further counting down the result by the programmed 12-bit Tone Period"* | CONFIRMÉE. The clock/8 plus T-flop split is MAME's structure, not the sheet's, and the comment now says so |
| `Ssi263.cpp:120`, `Ssi263.h:410` | Frame = 4096 × (16 − R) XCK, phoneme = (4 − D) frames | claimed B | Data sheet **not obtained** | INTROUVABLE |
| `Ssi263.cpp:195`, `:411`, `Ssi263.h:390`, `Ssi263Formant.h:18`, `Ssi263Formant.cpp:126` | Phoneme initiation, I11/I10-I3 layout, the ~20 kHz filter clock | claimed B | Data sheet **not obtained** | INTROUVABLE |

## Constats

**None.** No datasheet I opened contradicts what the code *does*. Every
discrepancy was in the comment: a wrong page or section, a paraphrase
presented as a quote, a section name that does not exist, or a framing that
contradicts the graded rule. No discrepancy has a failure scenario.

Notes that are not findings:

- **6551 parity.** A WDC W65C51N has **no parity at all**, while the Super
  Serial Card's Rockwell or Synertek 6551 does. If POM2 ever offers a
  W65C51N-based card, its parity path must be off. The SSC is not that card.
- **CRTC interlace rule.** HD46505R and HD6845S disagree on the R9
  programming rule for interlace sync & video mode. POM2 models the HD6845S
  (Videx), so it follows the U70 book.

## Ce que POM2 ne sait pas

| Subsystem | Claim | What was searched | What would settle it |
|---|---|---|---|
| SSI263 | Rate and duration timing 4096 × (16−R) × (4−D) XCK; the I11/I10-I3 inflection layout; the ~20 kHz filter clock | 2 web searches; the Kassel lab sheet (not the data sheet) | The Silicon Systems SSI 263A data sheet (1985). Ask ReactiveMicro, who sell the part and mention documentation, or look for a scan on archive.org or applefritter. A scope on A/R̅ at known R and D would make it grade A |
| 6551 (SSC) | MARK and SPACE parity are not checked on receive | Only the W65C51N, which has no parity | The Rockwell R6551 or Synertek SY6551 data sheet, PMC table. Probably in a Rockwell data book on bitsavers (`components/rockwell/`) |
| Z8530 SCC | The page references p. 35 (reset values) and p. 36 (interrupt priority) in UM010902, plus every `SDLC (datasheet, not MAME)` site (23 lines, none with a section) | zilog.com, which redirects to a Littelfuse JavaScript page | Get UM010902 from Littelfuse or an archived copy, then add § and page to each SDLC site. The 2026-11-01 parity tranche 3 covered the MAME side |
| 6522 | T2's load and decrement timing identical to T1's | W65C22 § 2.9 says only "similar" | An mb-audit-style measurement of T2 read-back right after a T2CH write on a real 65C22 |
| AY-3-8910 | The internal clock/8 counter plus divide-by-2 T-flop | GI 1982 gives only the overall /16 | A die analysis or a scope on a channel output at TP = 1 |
| µPD1990AC | TP rate at power-on | pp. 792-805 read, no statement found | A real chip at power-on with a scope on TP |
| HD6845S | State of the address register after RES | Not searched in U70 | U70 pp. 451-463 |

## Bilan

**Sites.** 152 lines match the grep.

- **Examined against an opened document: 44.** That covers
  `Via6522.h` (6), `SuperSerialCard.cpp` (5), `ClockCard.*` (6),
  `NoSlotClock.*` (10), `Hd6845Crtc.*` (4), `TMS9918.*` (11) and the AY (3).
- **Attempted without obtaining the document: 7** (SSI263) and 23 (SCC, UM010902).

**Verdicts** on the 44:

- CONFIRMÉE: 33
- IMPRÉCISE, corrected: 6
- INTROUVABLE: 1 (R6551 PMC)
- DÉCORATIVE: 3
- Kept as F: 2

Plus 7 INTROUVABLE on the SSI263.

**Grades before → after**, on the 44 sites:

- **Before:** all 44 *claimed* a datasheet. 21 named a locatable page or
  section, and 3 of those were wrong: W65C51N p. 13, TMS9918 § 2.2 and
  § 3.5.1. One rested on MAME quoting the datasheet (HD6845 R9).
- **After:** 40 sites have a **verified [B] with page or section**. One is
  raised **D → B** (HD6845 R9, U70 p. 454). One drops from claimed-B to
  **INTROUVABLE** (R6551 PMC).

**Constats:** 0.

**Comment edits proposed:** 18, in
`/private/tmp/claude-501/-Users-gistair-src-pom2/5aefd4ce-6f18-48e9-ad76-d341fe6f98ab/scratchpad/prov_P3.json`.
Each one changes comment text only and is unique in its file.

**Not examined (time box):**

- `W5100Device.*`: 45 lines, 32 already carry a § or page.
- `Cs8900aDevice.*`: 25 lines, 20 with a § or page.
- `Tms5220.cpp:348`: MAME-imported, cites "p 10, table 4".
- `Opcode6502.h:30` and `M6502.h:141`: the NCR sheet in the //c Technical Reference.
- `M68705P3.cpp:158`.
- `DiskImage.cpp:1994`: MC3470.
- `Ccs7710Card.*`: MC6850.
- `TMS9918.cpp:531`: "no inter-access constraint" for register writes.
- `AbstractionLevels_ImGui.cpp:343`: a UI string.

The W5100 and CS8900A files were re-read against their datasheets by parity
tranches 4 and 7 (2026-11-01), so they were the lowest priority here.
