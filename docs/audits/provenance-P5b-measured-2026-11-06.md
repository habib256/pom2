# Provenance P5b — "measured" in `src/` files M–Z (2026-11-06)

Tranche P5, second half, of `docs/audits/BRIEF-provenance.md`. Sites:

```
grep -rniE "measur" src/ --include='*.cpp' --include='*.h'
```

restricted to files whose basename starts with M–Z, `third_party/` excluded:
**117 sites**. The question for each: *measured against what?* Real hardware
(A), another emulator (D), a run of POM2 itself, the host machine, or nothing.
The four `[A]` brackets in the same files that do not contain the word
"measured" (`Memory.cpp:774`, `Memory.cpp:1194`, `PhasorCard.cpp:897`,
`Via6522.h:399`) were checked too, because the purpose of the tranche is to
count the A claims that are real.

Time-boxed to 20 minutes. Every source cited below was opened during the
tranche (GitHub issue threads through `gh api`, MAME and web-a2e files through
raw.githubusercontent.com) unless the row says otherwise.

## Registre

### Hardware claims (10 sites + 4 non-"measured" `[A]`)

| `file:line` | Claim (short) | Grade | Source found | Verdict |
|---|---|---|---|---|
| `MainWindow_SlotConfig.cpp:715`, `Mockingboard.h:202` | Mockingboard 4c carries an AY-3-8912; read-back like an 8910 | **A** | tomcw/mb-audit#22, comment 2026-07-26: two Microchip AY38912A/P (date code 9208) pulled from an MB4c, tested on an MB-K, read back `FF 55 69`; GI AY-3-8912 (8048) on 2026-08-19 | CONFIRMÉE (edit: name who, when, two parts) |
| `PaddleInputs.h:117` | bare ][+ reads `1 1 1 1` on `$C061-3`, //e enhanced `1 0 0 1`, Kraft leaves SW2 high | **A (by report)** | AppleWin#1234 (xotmatrix, 2023-06-02): Kraft results in the body; rikkles' ][+ `1 1 1 1` and the //e `1 0 0 1` relayed in a comment | CONFIRMÉE (edit: "by report", users' machines) |
| `Memory.cpp:1194` | `LDX $C050` in TEXT+HIRES returns a text byte, the next `LDY` an HGR byte | **A** | TomHarte/CLK#1204 (ryandesign, 2023-11-13): "On my real unenhanced Apple IIe … usually prints `A0 7F`" | CONFIRMÉE |
| `PhasorCard.cpp:897` | /RESET resets both AYs of a 6522 whatever the mode | **A (by report)** | AppleWin#1197 (tomcw): checklist item "6522's AY RESET always resets both attached AYs"; next to real-Phasor observations; no raw capture — as the comment already says | CONFIRMÉE |
| `Via6522.h:399` | a held ORB write does not re-strobe R13 | **A (by report)** | AppleWin#320 (2016): Grouik/French Touch's "inactive function" sequence, "the working method for real APPLE II" | CONFIRMÉE |
| `Memory.cpp:774` | real machines' power-on RAM disagree (Westgate's survey, AppleWin #206); depends on DRAM maker and board | **A (by report)** for "disagree"; **E** for the cause | AppleWin#206: sicklittlemonkey relaying Michael Mahon, "I don't think there is a standard pattern". No "Westgate" anywhere in the thread | IMPRÉCISE (edit) |
| `VidHDCard.cpp:108` | SHR fill-mode start colour "measured by John Brooks" | **D** (relaying an unpublished A) | MAME `apple2video.cpp:922-929` @ `mame0287`: "(hardware testing by John Brooks)" above `fillmode_init[32]` | IMPRÉCISE (edit: grade D, pin) |
| `PrinterSoundDevice.cpp:21` | grain values "came from spectral measurements of real dot-matrix printers" | **D**; the papers unopened; most values **F** | web-a2e `src/js/printer/printer-sound.js:86-88` @ `e12b5a88`: only the ~900-1000 Hz centre band is attributed to "Backes et al; JSPE noise studies". Durations, gains, jitter: no attribution | IMPRÉCISE (edit) |
| `MainWindow.h:615-618`, `MainWindow_Screen.cpp:581` | "the shape a ruler finds" — 1.225:1 NTSC, ~1.507 PAL | **B** NTSC (a *calculation* in Sather) / **E** PAL | Sather's text is quoted at `MainWindow_Screen.cpp:583-591`; **not reopened in this tranche**. `CpuClock.h:234` already labels 1.507 "POM2's derivation" | IMPRÉCISE — no ruler involved (edit) |
| `Via6522.h:324`, `:334` | T1 read-back bias −1 "measured, not assumed" on TRIBU | **C** (a program tuned on hardware) | Fr3nchT0uch/DIX `TRIBU/Sources/main.a`: `DELAY = 24 ; value FOR APPLEWIN / REAL APPLE II` (found by GitHub code search). The "−5 cycles/frame" is POM2's own run. The cited `disks_5.4/demo/unreeeal_superhero_3_tribute/` no longer exists | IMPRÉCISE + stale path (edit) |

### Runs of POM2 itself (20) — not hardware claims

"Measured" here means *observed in POM2* — a probe, a ctest, a demo or an OS
run under emulation. The hardware fact behind each, where there is one, is the
contract of the software that was run (C) or another citation nearby.

`SmartPortBusDevice.h:107` (ProDOS 8 2.4 + Liron firmware, `liron_chain`),
`ProDOSVolume.cpp:357`, `:394` (a survey of the ProDOS images then in the
tree — the tree no longer tracks them), `ZipChip.cpp:105` (bug symptom),
`Memory.cpp:503`, `:1041` (MAD EFFECT), `Mockingboard.h:413`,
`Mockingboard.cpp:383`, `:435`, `:446`, `:448`, `:451`, `:464`, `:516`,
`:628` (DD1 / AY runs), `SystemProfile.cpp:271` (`oldskool_raster_probe`),
`Sony35Drive.h:196`, `:260`, `M6502.cpp:2256` (`t1_irq_phase` fixture),
`MouseCoordinator.cpp:121` (A2DeskTop).

Verdict: DÉCORATIVE as provenance — no grade to assign. None of them claims
real hardware.

### Host measurements (33) — performance, host OS, libraries, peers

Wall-clock or profile figures on the developer's machine, or the behaviour of
the host's sockets / libslirp / a FujiNet process: `RewindBuffer.cpp:55`,
`:267`, `:292`; `MediaMount.h:26`, `:173`; `SerialPort.cpp:302`,
`SerialPort.h:119`; `SocketUtil.h:155`; `Memory.cpp:459`, `:549`;
`Memory.h:393`, `:417`, `:427`, `:487`, `:586`, `:1034`, `:1596`;
`TnfsClient.cpp:163`, `:345`, `:518`, `TnfsClient.h:44`;
`SlirpNetworkBackend.cpp:151`; `pom2_bench.cpp:146`, `:220`, `:296`;
`StorageCoordinator.cpp:2386`; `PrinterHistory.h:53`; `SlotBus.h:308`;
`M6502.cpp:30`, `:1899`, `:2268`, `:2306`; `SpOverSlipLink.cpp:926`.

Verdict: outside the grading scheme (it grades claims about the *Apple*).
Each is dated or names its tool; `docs/PERFORMANCE.md` holds the method for
the profile figures. Not re-run here.

### Honest "not measured" (6) — already F, correctly

`Memory.cpp:659`, `:772`, `MainWindow_AudioPanels.cpp:462`,
`NtscPostProcessor.h:110`, `SuperSerialCard.cpp:1018`, `:1035`. Verdict:
CONFIRMÉE as F; listed below under what POM2 does not know.

### Decorative (48) — "measure" in another sense

UI layout ("measuring the widest label"), units ("measured in CPU cycles"),
the `Measured` aspect mode's name and menu entries, a guest program measuring
something, a quoted sentence, an error string: `Memory.cpp:441`, `:1318`;
`SpeakerDevice.h:126`; `Ssi263Formant.cpp:94` (the example word "measure");
`PhasorCard.cpp:719`; `MainWindow.h:620`, `:1004`, `:1074`;
`MainWindow.cpp:718`; `MainWindow_Session.cpp:378`, `:410`;
`MainWindow_SlotConfig.cpp:1046`; `Sony35Drive.cpp:550`, `:870`;
`Sony35Drive.h:345`; `MainWindow_Input.cpp:488`; `Toolbar_ImGui.cpp:72`,
`:147`; `Scc8530Device.cpp:1451`; `MainWindow_Kiosk.cpp:1000`, `:1065`;
`SlotBusClock.h:30`; `MainWindow_Chrome.cpp:203`, `:204`, `:303`, `:890`,
`:894`, `:896`, `:897`, `:1111`, `:1293`; `MainWindow_Screen.cpp:405`,
`:408`, `:410`, `:594`, `:600`; `TranswarpCard.h:69`, `:102`;
`Mockingboard.cpp:1018`; `MouseCardAppleWin.cpp:178`;
`MainWindow_Slots.cpp:141`, `:142`, `:1733`; `MainWindow_CrtPanel.cpp:275`;
`VsDriveProtocol.cpp:73`; `RomFetch.cpp:559`; `SuperSerialCard.h:52`;
`main.cpp:229`.

## Constats

None. No source contradicts the code it backs. Two notes, below the bar of a
constat (no failure scenario):

- The `[A]` mb-audit claim is *stronger* than the comment says: both 8912s
  and a GI 8912 read back like an 8910, which `Ay3_8910.h:26-45` already
  models and records as AMONT FAUX against MAME. Consistent.
- `Via6522.h` pointed at a demo-source path that left the tree on 2026-10-29;
  the sources are public in `Fr3nchT0uch/DIX`. Fixed in the proposed edit.

## Ce que POM2 ne sait pas

**Display**
- PAL measured-window aspect (~1.507): POM2's derivation by Sather's method,
  not a document and not a ruler. Settled by: a photograph or a ruler on a PAL
  //e driving a calibrated PAL monitor, or a European technical reference that
  states the visible line fraction.
- NTSC 1.225: the quote was not reopened by this tranche. Settled by:
  opening Sather, *Understanding the Apple II*, ch. 8 and confirming the page.
- VidHD fill-mode start colour: known only as MAME's relay of John Brooks'
  unpublished IIgs VGC testing; nothing about the VidHD itself. Settled by: a
  VidHD capture of a fill-mode line whose first dot is index 0.

**Memory**
- Power-on RAM: "no standard pattern" rests on one relayed forum remark; the
  "Westgate's survey" the comment named could not be found. That the pattern
  depends on DRAM maker and board is unsourced (E). Settled by: RAM dumps taken
  before the cold start on two or more machines with different DRAM.

**Audio**
- Printer grain durations, gains and jitter: web-a2e's tuning (F). Only the
  ~900-1000 Hz band is attributed to papers, which nobody here has opened.
  Settled by: opening Backes et al. (USENIX Security 2010, "Acoustic
  Side-Channel Attacks on Printers") — or simply by the recorded ImageWriter
  II bank POM2 now ships, which is grade A for that printer.
- SSC / //c mirror decode (`SuperSerialCard.cpp:1018`, `:1035`): "not
  measured on a //c" — settled by a bus read of the mirrors on a //c.
- AY level and filtering vs the chip (`MainWindow_AudioPanels.cpp:462`): no
  measurement. Settled by: a scope capture of a Mockingboard output.

**Video signal**
- Tube halation constants (`NtscPostProcessor.h:110`): taste, no
  measurement. Settled by: a macro photograph of a lit pixel on the monitor
  being modelled.

**Timing**
- The Mockingboard T1 bias −1 rests on a demo tuned on hardware (C), not a
  capture. Settled by: a logic-analyser capture of `$C404` reads N cycles
  after a T1C-H write on a real 6522.

## Bilan

- **Sites examined:** 117 "measured" + 4 non-"measured" `[A]` = 121.
- **What "measured" meant:** real hardware 3 sites (2 facts) · relayed through
  another emulator or library 2 · a calculation from a primary document 3 · a
  program tuned on hardware 2 · runs of POM2 20 · host 33 · honest "not
  measured" 6 · decorative 48.
- **Real grade-A claims in M–Z files:** 7 sites carry `[A]`; **all 7 rest on
  something openable**, 2 of them first-hand (mb-audit#22, CLK#1204) and 5 by
  report through an issue thread. One (`Memory.cpp:774`) overstated its
  source. No A claim was invented — but none is POM2's own measurement.
- **Grades before → after (hardware sites):** before, 3 bracketed `[A]` + 7
  unbracketed "measured" that read as A. After: A 3 (2 marked "by report"),
  A-by-report 1 (Memory.cpp:774, narrowed) + E for its cause, B 1, C 1, D 2,
  E 1 (PAL aspect), F 1 (printer tuning).
- **Verdicts:** CONFIRMÉE 5 · IMPRÉCISE 5 · CONTREDITE 0 · INTROUVABLE 1
  ("Westgate's survey") · DÉCORATIVE 101 (runs, host, decorative).
- **Constats:** 0.
- **Proposed comment edits:** 9, in
  `scratchpad/prov_P5b.json` (comment text only).
- **Not verified:** Sather's 1.225 page (quoted, not reopened); the Backes /
  JSPE papers; the 33 host figures (not re-run); "Westgate's survey" (searched
  in AppleWin#206 only — it may exist elsewhere).
