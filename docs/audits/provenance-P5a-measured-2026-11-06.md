# Provenance audit P5a — "measured", files A–L (2026-11-06)

Tranche P5, first half, of `docs/audits/BRIEF-provenance.md`. The sites are
`grep -rniE "measur" src/ --include='*.cpp' --include='*.h'`, restricted to
files whose basename starts with A–L: **99 lines**.

The question for each one is **measured against what?** Possible answers:
- real hardware: grade A, if the measurement was opened;
- software that ran on real hardware, replayed or observed: grade C;
- another emulator: grade D;
- POM2 itself or the host (a benchmark, a test run, a bug reproduction). That is not a claim about the Apple II at all.

The sources were opened on 2026-11-06 with `gh api` (AppleWin PR #837 and
tomcw/mb-audit issue #22) and WebFetch (applefritter node/33654).

## Registre

### Hardware claims (25 lines)

| `file:line` | Claim (abridged) | Grade | Source found | Verdict |
|---|---|---|---|---|
| `CpuClock.h:150,154,158` | PAL //e CPU clock measured at ~1.0153 MHz | **A** | afrb2, applefritter "PAL IIe cassette interface" (node/33654), post #7 "Scope tests", 2024-02-06: *"The CPU clock measures 1.0153MHz (on a good 12 digit counter using a 100ms gating time)"* | CONFIRMÉE — the quote is verbatim; the poster and date were missing |
| `Apple2Display.cpp:2142` | Mixed-DHGR boundary rules "fenarinarsa measured on the //c adapter" | **A** (third party) | AppleWin PR #837 (fenarinarsa, 2020-09-24): *"This was once again validated on my IIc adapter"*, with //c photos | CONFIRMÉE — observed on hardware, not instrumented |
| `LeChatMauveCard.h:72` | No TEXT/MIXED/HIRES precondition, "measured on the //c adapter" | **B + A** | PR #837: *"The Video7 patent doesn't state any precondition"*; the Prince of Persia BW560 behaviour is *"the same on real hardware"* | IMPRÉCISE — true, but the word "measured" hid two sources |
| `Ay3_8910.h:28,43,48` | AY-3-8910/8912/8913 read-back values | **A** | tomcw/mb-audit #22, comments of 2026-06-06 and 2026-07-26 (8913 = `3F 0E 0F`; GI 8912 date code 8048 behaves like an 8910) | CONFIRMÉE — already graded `[A]` correctly |
| `AyPsgSynth.h:97,107` | Westcott's 2001 measured AY output voltages | **D** (relays an A) | Known only through MAME's `ay8910_param`; Westcott's data was not opened | IMPRÉCISE — re-graded D: we read MAME, not the measurement |
| `Apple2Display.cpp:392,399` | Text column offset 24, "measured, not assumed" | **C** | POM2 replay of French Touch's MAD EFFECT, sweeping 65 phases; the demo was written against real hardware | IMPRÉCISE — a replay in POM2, not a capture. CpuClock.h:247 adds [B] Sather p. 5-10 for the same rule |
| `CpuClock.h:247` | Same rule, "measured on MAD EFFECT" | **C** (+ B below it) | as above | IMPRÉCISE |
| `DiskIICard_Sequencer.cpp:30` | Own sequencer "measured on Apple's sequencer as a black box" | **C** | Apple's 341-0028-A P6 table run inside POM2's LSS (`lss_window_probe`, `lss_write_probe`) | IMPRÉCISE — it is the PROM's contract, executed by POM2, not a drive |
| `IntegerBasicListing.h:27` | Integer BASIC line-length rule | **C** | The bytes of two real programs on `saturn128k_software.dsk` | CONFIRMÉE |
| `FujiNetCard.cpp:557` | The peer aborts on RESET to the printer unit | **C** | FujiNet desktop firmware, observed 2026-08-21 (peer software) | CONFIRMÉE as a software observation, not hardware |
| `FujiNetCard.cpp:849` | Control-list layout of an `N:` open | **C** | Wire bytes from the FujiNet Contiki browser | CONFIRMÉE |
| `FujiNetNetDevice.h:23` | The desktop firmware never opens a socket for `N:` | **C** | Peer descriptors, observed 2026-08-21 | CONFIRMÉE |
| `DiskBuilder.cpp:387` | Read step 2 is the fastest | **D** | Casso `DirectBootBuilder.cpp` @ 2d899adf | already graded `[D]` |
| `DiskBuilder.cpp:390` | Not measured in POM2 or on a drive | **F** | — | already graded `[F]`, honest |
| `Hd6845Crtc.cpp:132` | No HD6845S has been measured | **F** | — | already graded `[F]`, honest |
| `CrtPresets.h:28` | No photograph or measurement behind the CRT numbers | **F** | — | honest in words |
| `AudioDeviceWatch.h:39` | Stall thresholds are judgement, not measurement | **F** | — | already graded `[F]` |
| `CpuClock.h:231,236` | PAL window aspect unverified; `measuredWindowAspect()` | **B** (NTSC) / **F** (PAL) | NTSC: Sather, printed; PAL: POM2's own derivation | IMPRÉCISE — the function name says "measured", but neither value is a measurement |

### POM2 or host measurements, not hardware claims (49 lines) — DÉCORATIVE

These are performance figures, bug reproductions, test outputs and analyses of POM2's own assets. They are legitimate engineering evidence about POM2, but they say nothing about the Apple II:

`AudioMix.h:91,234` · `AudioDevice.h:111,114` · `AudioDevice.cpp:140` ·
`AudioSource.h:71` · `AudioBufferPolicy.h:38,77` · `Block512Backing.h:77` ·
`Block512Backing.cpp:263` · `FujiNetNetDevice.cpp:170,406` (macOS `connect()`) ·
`ClockCard.h:282` (POM2's own 2045.45 Hz rounding error, arithmetic) ·
`CpuClock.h:50` (`sony35_iwm_read_path_test`) ·
`EmulationController.cpp:110,229,849` ·
`FloppySoundDevice.cpp:52,500,936,975` · `FloppySoundDevice.h:156,347` ·
`FloppySoundSink.h:36` · `DiskIICard.cpp:798,799,1334` ·
`DiskImage.cpp:1895,2150` · `LironCard.cpp:337,437` · `AyPsgSynth.h:55,80`
(`mockingboard_audio_quality`) · `Keyboard.h:32` · `Apple2Display.h:580` ·
`Debugger.h:46,49` · `AiControlServer.cpp:1428,1617` · `CliRunner.cpp:258` ·
`ImageWriter.h:764,819,827` · `ImageWriter.cpp:2071` · `HgrConvert.cpp:451` ·
`HgrPaintModel.cpp:184` · `CassetteDevice.cpp:136` · `CassetteDevice.h:130` ·
`IWMDevice.h:156`.

### "Measured" used in another sense (25 lines) — DÉCORATIVE

These are units ("measured in 125 ns units"), reference points ("measured from the left margin"), feature names (Debugger "Clear measurements") and the keyboard photo's hotspots:

`IWMDevice.h:50` · `DiskImage.h:559` ·
`ImageWriter.cpp:778,1767,1768,1773,1936,1937` · `ImageWriter_EscP.cpp:269` ·
`ImageWriter.h:225` · `FloppySoundDevice.h:42` · `HgrConvert.h:29` ·
`hgrpaint/HgrPaintEditor.cpp:1791` · `DiskFlux_ImGui.cpp:53` ·
`Debugger.cpp:243,330` · `Debugger.h:150` · `Debugger_ImGui.cpp:186,188,190,191` ·
`Keyboard_ImGui.h:20` · `Keyboard_ImGui.cpp:123` ·
`AppleIIeKeyboardLayout.cpp:18` · `AppleIIeKeyboardLayout.h:37`.

## Constats

None. No "measured" claim was contradicted by the source it rests on.

One wording drift was fixed along the way: `CpuClock.h:157` still called MAME "the MAME value this file cites as its oracle". It is now graded D.

## Ce que POM2 ne sait pas

- **POM2 has no first-party hardware measurement in these files.**
  - Every grade-A claim here is a third party's: afrb2's counter reading, fenarinarsa's //c adapter, Tom Charlesworth's mb-audit captures.
  - Every other "measured" is POM2 measuring itself.
  - The project's "the silicon is the reference" rests, in this half of the tree, on **four** real hardware observations.
- **PAL CPU clock.**
  - Known: one counter reading of one machine (A). No schematic of the PAL divider chain was found.
  - Would settle it: a frequency counter on phi0 of two European //e boards, or the PAL //e schematic.
- **Text and mixed-mode beam offsets.**
  - Known: a demo replay (C) plus Sather (B). No capture.
  - Would settle it: a logic-analyser trace of a soft-switch write against the video address on a real //e.
- **Own Disk II sequencer.**
  - Known: Apple's P6 contract (C). Never compared against a drive's flux.
  - Would settle it: a flux capture (Applesauce or Greaseweazle) of a POM2-written track read back on a real Disk II.
- **AY volume curve.**
  - Known: Westcott's 2001 data, seen only through MAME (D).
  - Would settle it: opening the original measurement, or a scope on one AY channel at the 16 levels.
- **PAL picture aspect** (`kPom2PalWindowAspect` = 1.507).
  - Known: POM2's own derivation (F).
  - Would settle it: a ruler on a Péritel-era monitor showing a known-square test pattern (as `CpuClock.h` already says).
- **HD6845S cursor blink phase** (F). Would settle it: a capture from a Videx Videoterm.

## Bilan

**Sites examined:** 99 of 99. **Hardware claims:** 25 lines in 13 claims.

| Category | Lines |
|---|---:|
| A, real and opened | 8 |
| A relayed through D, not opened | 2 |
| C | 8 |
| D | 1 |
| F | 6 |
| POM2 or host measurements | 49 |
| Another sense of the word | 25 |

**Graded brackets on the 25 hardware lines:** 7 before (`Ay3_8910.h` ×3,
`DiskBuilder.cpp` ×2, `Hd6845Crtc.cpp`, `AudioDeviceWatch.h`), 18 after the
12 comment edits (11 brackets and 1 de-oracling) proposed in
`scratchpad/prov_P5a.json`. No E was raised to B. The work here was mostly
**demoting**: three "measured" claims really rest on C, and one on D. Two
were raised: the PAL clock and fenarinarsa's observations, which now carry
an attributable A.

**Not verified:**
- Westcott's 2001 dataset itself.
- Whether fenarinarsa's observations were photographs only or also instrumented. PR #837 shows photos.
- The second half of P5 (files M–Z) belongs to another tranche.
