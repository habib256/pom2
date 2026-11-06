# Provenance audit — P1b: "approximate", "tuned", "empirical", "magic number" (2026-11-06)

Tranche P1 of `docs/audits/BRIEF-provenance.md`, restricted to the words
`approximat|tuned|tuning|empiric|magic number` (the `guess` / `arbitrary`
half of P1 is not covered here). Time-boxed to 20 minutes.

```
grep -rniE "approximat|tuned|tuning|empiric|magic number" src/ --include='*.cpp' --include='*.h'
```

**61 hits** outside `Ssi263PhonemeData.cpp`; three are in `third_party/miniaudio.h`
and one in `stb_image.h` (vendored, out of scope). Most of the remainder are
not hardware claims (UI tuning, host-side heuristics, history notes). The
proposed comment corrections are in the session's `prov_P1b.json` (13 edits,
comment text only).

## Registre

Hardware-relevant sites, then the decorative ones in one block.

| `file:line` | Claim (abridged) | Grade before → after | Source found | Verdict |
|---|---|---|---|---|
| `src/M6502.h:398-401` | "WAI consumes three cycles and falls through; no waiting latch" | E → D | `M6502.cpp` `WAI()` (waits since 2026-10-24); [D] MAME `ow65c02.lst:875-881` @ `mame0287` (`wai_imp`) | **CONTREDITE** by the code itself — stale comment, not a behaviour bug |
| `src/Apple2Display.cpp:1412-1418` | Féline palette "verbatim from AppleWin … commit ec3b03c, source-of-truth"; "white-balanced RGB video capture" | D (unpinned) → D (pinned) | AppleWin `source/RGBMonitor.cpp:167-203` @ `3e8054b462` — all 16 values identical; the "white-balanced RGB video capture" comment is AppleWin's own (:168). **Commit `ec3b03c` does not exist** in AppleWin (GitHub API 422). | IMPRÉCISE: right values, wrong pin, "source-of-truth" framing |
| `src/Apple2Display_ChatMauve.cpp:72` | HGR Chat Mauve colours from the same Feline table | D → D (pinned) | same as above | CONFIRMÉE |
| `src/Apple2Display.cpp:1375-1376` | lo-res palette "verbatim from MAME … calibrated against real Apple II hardware" | E (as worded) → D | MAME `apple2video.cpp:832-854` @ `mame0287` — values identical; MAME's source line is the **"Apple II Video Display Theory" spreadsheet**, a derivation, not a calibration | **CONTREDITE** (claim stronger than the source) |
| `src/Apple2Display.h:872` | "IIGS-corrected approximation" | E → D | same table — it is MAME's theory palette | **CONTREDITE** (contradicts its own definition) |
| `src/AyPsgSynth.h:97-99` | "Westcott's 2001 measured AY-3-8910 output voltages" | A (imprecise) → A, relayed by D | `parite-ay8910-2026-11-01.md` line 91: the measurement was on an **AY-3-8912** in a ZX Spectrum 128; MAME `ay8910.cpp` carries it (`ay8910_param`), "Based on Matthew Westcott's measurements from Dec 2001" (:681 @ `a2b6ba2d4be7`) | IMPRÉCISE (chip variant) |
| `src/AyPsgSynth.h:108` | analogue amplitudes "remain an approximation" | F | — | INTROUVABLE (stays F; the MAME load model is the known difference) |
| `src/ZipChip.h:20-21, 44, 69` | MAME line numbers, "master as fetched 2026-10-16"; MAME "approximates" cache execution / boot speed | D (pinned by date) → D (pinned by commit) | MAME master `b2a182f0b7` (2026-10-29): :1315, :1537-1538 ("config 'Bootup speed' is an approximation"), :2237-2244 ("Cache is not emulated here, but as an approximation"), :2562-2632 all fall. **None falls on `mame0287`.** | CONFIRMÉE at master; pin added |
| `src/TMS9918.cpp:171` | power-on backdrop white, "empirical observation on Claudio's Replica-1" | (ungraded) → A | the observation itself (POM1 import, 2026-05-12) | CONFIRMÉE as an A of scope one machine, one observation |
| `src/MainWindow.h:1689`, `src/MainWindow_Slots.cpp:1279`, `src/FloppySoundDevice.h:425`, `src/FloppySoundDevice.cpp:967` | //c internal 5.25" drive is a "Sony", spins up "≈ 40 % faster", higher pitch; ×1.4 | E → F | none found; no recording compared | INTROUVABLE — re-graded F with what would settle it |
| `src/PrinterSoundDevice.cpp:21` | constants "came from spectral measurements of real dot-matrix printers" | E → D (+F) | web-a2e `src/js/printer/printer-sound.js:86-96` @ `e12b5a8806`: cites "Backes et al; JSPE noise studies" for the spectral **shape** (~900-1000 Hz, skirt to ~5 kHz). The constants (11/40 ms grains, 950/1500 Hz, Q, spacing) are the author's choices — 1500 Hz is outside the cited band | **CONTREDITE** (overstated) |
| `src/Apple2Display.h:76` (+ `Apple2Display.cpp:1212`) | green mono "approximates Apple's standard P31 CRT"; P31 at CIE x=0.280 y=0.595 | E | searched for a JEDEC TEP116 table: nothing usable found | INTROUVABLE |
| `src/Apple2Display_Beam.cpp:207` | TEXT/MIXED latency "glitching, approximate" | already [A]/[B]/[D] | graded in place (arekkusu42 photos, Sather, MAME PR #15247) | CONFIRMÉE (already graded) |
| `src/Ssi263Formant.cpp:111`, `src/Ssi263Formant.h:35` | Casso's values "tuned by ear" | already [F] | — | CONFIRMÉE as F |
| `src/CffaCard.h:24` | "cited line ranges below are approximate — re-pin" | F (self-declared) | not re-pinned here (parity work) | INTROUVABLE — belongs to the parity queue |
| `src/Tms5220.cpp:291` | "According to testing the pitch zeroing lasts approximately 2 samples" | D (MAME's comment, imported) | ported text; covered by `parite-queue-audio-2026-11-01.md` | not re-examined |
| `src/Keyboard.h:132,136` | one-shot AKD "approximation" for stream-only hosts | F (contract) | design choice, documented in place | DÉCORATIVE for the silicon (host-input contract) |
| `src/Memory.h:1553` | floating bus "approximates the address" | — | superseded by the next sentence and DEV § Memory (Sather-checked scanner) | DÉCORATIVE |
| `src/M6502.h:225` | `cycleCounter + getCurrentInstructionCycles()` is an approximation, ≤ 7 cycles | — | internal bound, true by construction | DÉCORATIVE |
| `src/DiskIICard.cpp:238` | legacy 32-cycle gate "16s-tuned" mis-frames 5-and-3 | — | POM2-internal, `dos32_boot_trace` phase 0 | DÉCORATIVE |
| `src/Sony35Drive.cpp:30, 852, 963` | an earlier "boot-tuned" register map | — | history note | DÉCORATIVE |
| `src/Apple2Display.cpp:2156` | MAME's byte-level Chat Mauve rule was "the approximation this replaces" | — | history; the manual-based rule is graded in DEV § Le Chat Mauve | DÉCORATIVE |

Decorative, not hardware claims (no further action): `AbstractionLevels_ImGui.cpp:128`,
`AppleSingle.h:16` (format magic, already a spec citation), `PhasorCard.h:320`,
`SpeakerDevice.cpp:178`, `TnfsClient.h:97`, `ImageWriter.cpp:762`,
`W5100Device.cpp:1701` (a register's name), `DiskImage.h:95`, `MainWindow.h:795,1616`,
`MainWindow_Input.cpp:592,631`, `Debugger_ImGui.cpp:395`, `CrtPresets.h:9-10`,
`EmulationController.h:420`, `JoystickPanel_ImGui.cpp:356`, `ProDOSVolume.cpp:1204,2112`,
`Z80.cpp:31`, `Z80.h:31`, `AppleWinNtsc.cpp:25`, `Mockingboard.cpp:112`,
`MainWindow_CrtPanel.cpp:79`, `MainWindow_SettingsPanels.cpp:287`, `WorkstationCard.h:331`,
`DiskLibrary_ImGui.cpp:43`, `hgrpaint/DhgrNtsc8Palette.cpp:24`, `hgrpaint/ImportCommon.h:207`,
`hgrpaint/HgrPaintEditor.cpp:83` (paint-editor colour matching, not emulation).

## Constats

No behaviour bug. Every contradiction found is a comment saying more than its
source, or describing code that has since changed:

### The cited AppleWin commit for the Féline palette does not exist
Fichier      : src/Apple2Display.cpp:1413
Source       : AppleWin `source/RGBMonitor.cpp:167-203` @ 3e8054b462
Gravité      : MINEUR (documentation; values correct)
Confiance    : CONFIRMÉ — GitHub API answers 422 "No commit found for SHA: ec3b03c"; the table at 3e8054b462 matches POM2's 16 values byte for byte.
Divergence   : the comment pins a commit nobody can open.
Scénario     : a future audit trying to re-check the palette "at its pin" cannot.
Observation  : `gh api repos/AppleWin/AppleWin/commits/ec3b03c`.

### "Calibrated against real hardware" is a theory palette
Fichier      : src/Apple2Display.cpp:1375-1376, src/Apple2Display.h:872
Source       : MAME `apple2video.cpp:832-835` @ mame0287 — "Latest Apple II NTSC palette from 'Apple II Video Display Theory'" (a spreadsheet)
Gravité      : MINEUR (documentation)
Confiance    : CONFIRMÉ (values identical; MAME's attribution read at the tag)
Divergence   : POM2 claims a hardware calibration (and, in the header, an IIGS correction) MAME does not claim.
Scénario     : someone "corrects" a measured colour against this table believing it measured.
Observation  : diff the two tables; read MAME's comment above `apple2_palette[]`.

## Ce que POM2 ne sait pas

- **Display — composite lo-res/HGR colours**: derived from a theory spreadsheet
  via MAME (D). Settled by: a colorimeter on a real composite monitor, or the
  spreadsheet's method opened and checked.
- **Display — Le Chat Mauve Féline colours**: from a capture AppleWin made and
  POM2 has never seen (D). Settled by: a capture of a Féline board.
- **Display — green monochrome tint (P31)**: tint and CIE coordinates unsourced
  (E). Settled by: JEDEC TEP116 or a colorimeter on a Monitor II / //c monitor.
- **Audio — AY volume curve**: measured, but on an AY-3-8912, and without the
  Mockingboard's load (A for the 8912, not for the 8913 the card carries).
  Settled by: a measurement on a Mockingboard's AY-3-8913.
- **Audio — drive and printer sounds**: the //c drive's ×1.4 pitch and the
  printer grain constants are ear settings (F). Settled by: recordings.
- **Zip Chip** behaviour rests on MAME master `b2a182f0b7` only (D); MAME itself
  calls its cache and boot-speed handling approximations. Settled by: the Zip
  Chip manual, or a real chip.

## Bilan

- Sites: 61 hits; 3 vendored skipped; ~21 hardware-relevant examined in detail,
  the rest classified decorative.
- Grades moved: 2 E → D (lo-res palette ×2), 1 E → D (WAI comment), 1 ungraded → A
  (TMS9918), 1 imprecise A refined (AY, 8912), 3 unpinned D → pinned D (Féline ×2,
  Zip Chip), 5 E → F with a settle-by (motor pitch ×4, printer constants), 1 E kept
  (P31).
- Verdicts: CONFIRMÉE 6 · CONTREDITE 4 · IMPRÉCISE 2 · INTROUVABLE 4 · DÉCORATIVE ~37.
- Constats: 2 MINEUR (documentation), 0 behaviour bugs, 0 AMONT FAUX.
- **Not verified**: the "Apple II Video Display Theory" spreadsheet (not opened);
  the Backes / JSPE studies web-a2e cites (not opened); JEDEC TEP116 (not found);
  `Tms5220.cpp:291` (left to the parity report); `M6502.cpp`'s own WAI comment
  still cites `ow65c02.lst:797-803`, the blank-line-free numbering the
  `ow65c02` parity audit flagged — the real range at `mame0287` is 875-881.
- Out of scope, noted: the `guess` / `arbitrary` half of P1.
