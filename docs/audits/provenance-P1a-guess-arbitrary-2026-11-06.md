# Provenance audit P1a: `guess` / `arbitrar*` (2026-11-06)

Tranche P1a of `docs/audits/BRIEF-provenance.md`: the half of P1 ("assumed
unknowns") that the `guess` and `arbitrar*` patterns find. The other P1
patterns (`approximat*`, `empiric*`, `magic number`, `tuned`) are not covered
here. Time-boxed to 20 minutes.

```
grep -rniE "guess|arbitrar" src/ --include='*.cpp' --include='*.h'
```

**113 lines.** 26 are vendored third-party code (`stb_image.h` 12,
`stb_image_write.h` 1, `third_party/miniaudio.h` 13) and are out of scope.
That leaves **87 POM2 lines**.

## The main result: these two words do not find assumed unknowns

The brief files `guess` and `arbitrar*` under grade F ("the code says itself
that it does not know"). In POM2 that is almost never what they mean:

| What the word is doing | Lines | Example |
|---|---:|---|
| **Negation**: "rather than guessed", "not guessed at", "instead of guessing" | ~35 | `ProDOSDiskFs.cpp:28` "refused, not guessed at" |
| **Security**: guessing a token, an origin, a secret | ~16 | `AiControlServer.cpp:294` |
| **Software / format design**: an arbitrary offset, address, string, size | ~20 | `Settings.cpp:70`, `M6502.h:193` |
| **Image conversion heuristics**: `guessCtx3`, `guessByte` in the HGR converter | ~10 | `hgrpaint/HgrConvert.cpp:992` |
| **Hardware claims** | 6 | below |

None of the 87 lines is an admitted guess about the silicon. So the F
backlog the brief expected from this pattern is not here. The real F sites
are likely under `approximat*`, `tuned` and `unverified`, the other P1/P2
patterns.

## Register

Only the lines that make a claim about hardware or a persisted format. All
other lines are `DECORATIVE` (they justify no hardware behaviour) and are
not listed one by one.

| `file:line` | Claim (short) | Grade | Source found | Verdict |
|---|---|---|---|---|
| `src/Scc8530Device.cpp:1452-1456` | MAME passes the RX word length for both directions; "this port reproduces both, rather than second-guessing the oracle" | D (as written) | **[B] Zilog Z8030/Z8530 SCC Technical Manual, 00-2057-03, Sep 1986, § 7.1.6, Fig. 7-7 / Table 7-3, p. 7-8** (opened: bitsavers `components/zilog/_dataBooks/`). Tx bits/character are WR5 D6-D5; "In the Six or Seven Bits/Character modes, unused data bits are ignored." | **CONTRADICTED** → finding 1. The comment also called MAME "the oracle". |
| `src/PrinterSoundDevice.cpp:19-22` | The grain values "came from spectral measurements of real dot-matrix printers" | E (as written) | [D] web-a2e `src/js/printer/printer-sound.js:86-96` @ `345455a292` (opened). It attributes only the energy band (~900-1000 Hz, skirt to ~5 kHz) to "Backes et al; JSPE noise studies". Durations, Q, peaks and spacing carry no source there. | **IMPRECISE**: the comment claims more than the source. Band = D relaying an unopened citation; constants = F. |
| `src/Memory.h:1006-1009` | `MF_*` bit positions "only have to be stable for the life of the process" | — (not hardware) | The code: `MemorySnapshot.cpp:48` writes `iieMemMode` raw (`putU16`), and `:303` reads it back. | **CONTRADICTED by the code**: the positions are a snapshot/session file format. A renumbering done on the comment's word would silently remap the paging of every saved snapshot. |
| `src/NoSlotClock.cpp:102-112` | DAY bits 4/5 (RESET enable, oscillator off) ship as 1; POM2 reads 0 | B (as cited) | Already cited: 1992 Dallas Semiconductor Product Data Book, DS1216E, pp. 5-22..5-25. | Not re-opened in this tranche. "Guessed" here is a negation. |
| `src/ImageWriter.cpp:2171-2176` | Diablo 630 escape parameter counts | B (as cited) | Already cited: Diablo 90440-00A, Rev A (12/82), § 3.5, p. 3-12/3-13. | Not re-opened. "No longer guessed" is a negation. |
| `src/SmartPortBusDevice.cpp:186` | A //c+'s external chain starts at 2 (its MIG drive is device 1) | E | The same fact is stated in `CLAUDE.md` § System profiles and pinned by `iic_external_smartport`. Contract-level (C) at best; not traced to a ROM address in this tranche. | **NOT FOUND** (no ROM address). |
| `src/SmartPortBusDevice.cpp:468-472` | DIB type $01 / $02, bit 5 = not removable; "DISK 3.5" name | B (as cited) | Already cites TN SmartPort #7. | Not re-opened. |
| `src/Disk35Mfm.h:37-39` | "A1 with a deliberately missing MFM clock is 0x4489" | E | No document opened in this tranche. | **NOT FOUND**. Standard MFM, but uncited. |

## Findings

### 1. The SCC transmits with the receive word length

```
File         : src/Scc8530Device.cpp:1470 (frameHalfBits, used for txHalfBits at :1135, :1315, :1596)
               src/Scc8530Device.cpp:1560 (the async byte handed to txCb_ unmasked)
Source       : Zilog Z8030/Z8530 SCC Technical Manual, 00-2057-03 (Sep 1986), § 7.1.6,
               Figure 7-7 and Table 7-3, p. 7-8.
               Also [D] MAME z80scc.cpp (the port's origin; the comment says MAME does the same).
Severity     : MINOR
Confidence   : CONFIRMED (manual opened; probe below)
Divergence   : The manual sets transmit bits/character with WR5 D6-D5, independent of
               WR3 D7-D6 (receive), and says unused data bits are ignored in 6 and 7 bit
               modes. POM2 sizes the transmit frame from WR3 (rxWordLength) and hands the
               whole data byte to the host.
Scenario     : Async, WR3 = $C1 (Rx 8 bits), WR5 = $28 (Tx enable, Tx 7 bits), 1 stop,
               no parity, 9600 baud; the guest writes $C1 to the data port.
               Silicon: 9 bit times on the line, data bits = $41.
               POM2: $C1 handed out, after 10 bit times (TBE / All Sent 11 % late).
Observation  : Probe (outside the repo, on the scc8530_smoke harness):
                 byte handed out: $C1 (silicon: 7 data bits -> $41)
                 frame length: 10.00 bit times (silicon: 1+7+1 = 9)
```

Impact on an Apple II today is small. The SCC serves the Workstation Card,
whose LocalTalk traffic is SDLC with 8 bits in both directions. Any async use
with differing Tx/Rx lengths, or with 5 to 7 Tx bits, is affected.

The same comment says MAME "keeps the start bit even in the sync modes".
This tranche did not open the manual's synchronous-mode pages, so that half
is **not** classified (see below).

## What POM2 does not know

- **MFM sync word** (`Disk35Mfm.h:37`): the 0x4489 "A1 with missing clock"
  is uncited. *Would settle it*: the WD177x/WD279x data sheet or the IBM
  System/34 MFM format description, page cited.
- **//c+ external SmartPort numbering from 2** (`SmartPortBusDevice.cpp:186`):
  pinned by a test, but not traced to the //c+ ROM. *Would settle it*: the
  ROM address where the //c+ firmware assigns unit numbers during its INIT
  scan (bank 1, near `$F223`, which `CLAUDE.md` cites for the probe).
- **Printer grain constants** (`PrinterSoundDevice.cpp`): durations, Q,
  peaks and spacing are F in the reference too. *Would settle it*: a
  recording of an ImageWriter II analysed for grain length and band. The
  recorded bank in `roms/printer_samples/` is such a recording and could
  serve.
- **SCC start bit in monosync / bisync** (`Scc8530Device.cpp:1453`): not
  checked against the manual. *Would settle it*: the Z8530 manual's
  synchronous-mode chapter (same PDF).

## Summary

- **Sites**: 113 lines; 26 vendored (out of scope); 87 POM2 lines examined.
- **Hardware or format claims**: 8. The other 79 are DECORATIVE: negations,
  security, software design, or image-conversion heuristics.
- **Grades before / after** (the 8 claim sites):
  - before: D 1, E 3, B 3 (as cited), not graded 1;
  - after: B 1 (the SCC claim, now backed by the Zilog manual), D + F 1
    (printer sound, split), E 2, B 3 (as cited, not re-opened),
    code-contradicted 1.
- **Verdicts**: CONTRADICTED 2 (SCC by the manual, `Memory.h` by the code),
  IMPRECISE 1, NOT FOUND 2, not re-opened 3.
- **Findings**: 1 MINOR, CONFIRMED (SCC word length).
- **Comment edits proposed**: 3 (SCC, printer sound, `Memory.h`), each
  stating its grade and source.
- **Not verified**:
  - the three already-cited B sources (Dallas 1992, Diablo 90440-00A,
    TN SmartPort #7) were not re-opened;
  - the SCC's synchronous-mode start bit;
  - every line classed DECORATIVE was classed by reading its comment and the
    code beside it, not by looking for a hardware source.
- **Recommendation for the brief**: `guess` and `arbitrar*` should not count
  as F in the inventory. The 78 + 31 occurrences the brief lists are almost
  all negations or non-hardware wording.
