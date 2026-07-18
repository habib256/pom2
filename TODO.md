# POM2 — TODO

Status as of 2026-07-18. Resolved items → `CHANGELOG.md`. MAME refs → `DEV.md`.

**Format**: `🟠 high · 🟡 medium · 🟢 low` at the head of each item. Indicative
effort in *italics*. File/line in `backticks`. Quick read:
[Quick wins](#quick-wins) then [Backlog by subsystem](#backlog).

## MAME ↔ POM2 parity (dashboard)

Canonical reference for what is ported and at what level. The `Known gaps`
listed here point to detailed items in the [backlog](#backlog).

| #  | Subsystem                  | Parity           | MAME / AppleWin refs                                                     | Known gaps                                                                              |
| --- | ---------------------------- | ---------------- | ------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| 1  | M6502 / 65C02 / Rockwell / WDC | Verbatim         | `om6502.lst`, `ow65c02.lst`; Tom Harte `65x02`                          | 🟢 NMOS 100% Tom Harte (decimal included); 🟡 WDC SBC invalid-BCD decimal not modelled (`e9`, undefined); 🟢 $5C 8-cyc residual |
| 2  | Memory + IIe + RamWorks        | Partial-verbatim | `apple2e.cpp:1275-1299`, `a2eramworks3.cpp:108-115`                      | 🟠 god-object (Keyboard/PaddleInputs to extract)                                         |
| 3  | Display HGR/DHGR/80-col        | Partial-verbatim | `apple2video.cpp:124-201`, `460-471`, `:751-758`; AppleWin `RGBMonitor.cpp` | 🟢 mono DHGR 1-px (mid-scanline, PAL 50 Hz, floating bus `$C05x`, page-flip DROL, Chat Mauve RGB: done) |
| 4  | SpeakerDevice                  | Verbatim         | `spkrdev.cpp:74-327`                                                     | —                                                                                        |
| 5  | CassetteDevice                 | POM2-original    | `apple2.cpp:362`                                                         | —                                                                                        |
| 6  | Mockingboard A/C (6522 + AY)   | Partial-verbatim | `ay8910.cpp:998-1015`, `:1077-1104`, `1309`; `6522via.cpp:959`          | 🟢 Port A read mask by DDR; 6522 subset (SR/PCR; T2 one-shot done, IRQ N+3 MAME)      |
| 6b | Mockingboard "C" Sound II      | POM2 + AppleWin  | `Mockingboard.h/.cpp` + `Via6522::setCa1NegativeEdge`                    | — (SSI263 at `$Cs40-$Cs44`, A/!R → VIA1.CA1)                                              |
| 7  | FloppySoundDevice              | Verbatim         | `floppy.cpp:1532-1620`, `:2925-3020`                                     | —                                                                                        |
| 8  | SlotBus + IRQ wire-OR          | POM2-original    | MAME slot bus pattern                                                    | —                                                                                        |
| 9  | DiskImage                      | Partial-verbatim | `woz_dsk.cpp`, `flopimg.cpp:2017-2106`                                   | 🟡 WOZ1 splice TRK+6650; 🟢 .nib2/.app, half-tracked NIB (88)                           |
| 10 | DiskIICard                     | Partial-verbatim | `machine/wozfdc.cpp:264-291`, P6 PROM 341-0028-A                         | 🟢 sub-instruction RAII vs per-cycle; Disk II out of snapshot deliberate                    |
| 11 | IWMDevice                      | Verbatim         | `machine/iwm.cpp:1-543`                                                  | 🟢 Q3 fast clock (Mac/IIgs only); window-size rounding                                  |
| 12 | SmartPortCard (//e Liron)      | POM2-original    | SmartPort spec + Apple Tech Note                                         | 🟢 multi-partition ProDOS (CFFA3000)                                                     |
| 13 | SmartPortHub + Sony35Drive     | Verbatim         | `apple2e.cpp:638-679`, `mac_floppy.cpp`, `flopimg.cpp:512/967/2017-2106` | —                                                                                        |
| 14 | CFFA (MAME-faithful IDE)       | Verbatim         | `bus/a2bus/a2cffa.cpp`                                                   | 🟢 CHD = phase 2; no media preservation on profile switch                         |
| 15 | ClockCard / ThunderClock+      | Partial-verbatim | `upd1990a.cpp:248-267`, `:312-327`                                       | 🟡 MODE_SHIFT lax; 🟡 DATA_OUT live vs MAME latch; 🟢 slot ROM mostly NOPs             |
| 16 | SuperSerialCard                | Partial-verbatim | `mos6551.cpp:46`, `:542-543`, `a2ssc.cpp:373`                            | 🟢 IRQ gate SW2:6 DIP not gated                                                          |
| 17 | MouseCard (MAME)               | Verbatim         | `bus/a2bus/mouse.cpp`, M68705 + MC6821                                   | 🟢 PIA out_a/b without `scheduler.synchronize`                                          |
| 18 | MouseCard (AppleWin HLE)       | Verbatim         | AppleWin `source/MouseInterface.cpp`                                     | — (slot EPROM only, MCU synthesized)                                                      |
| 19 | Phasor (AE — 2×VIA, 4×AY)      | Verbatim         | MAME `a2bus/phasor.cpp` + AppleWin                                       | 🟢 EchoPlus mode (=7) routed as native Phasor; stereo deferred                         |
| 20 | SSI263 speech (chip model)     | AppleWin-faithful| AppleWin `source/SSI263.{h,cpp}` (MAME does not implement)                 | 🟢 formant synth → PCM blob, 62 phonemes (AppleWin LGPL → GPL3)                           |
| 21 | EchoPlusCard (Cricket/SSI263, key `echoplus`) | POM2-original | Cricket / Street Elec SSI263 spec (historically mislabelled "Echo+") | 🟢 markadev audit 2026-05-28: the real Echo+ = TMS5220 (see line 21bis)                |
| 21bis | EchoPlusTMS5220Card (key `echoplus_tms`) | Scaffold       | markadev/AppleII-RevEng/Street-Electronics-Corp-ECHO+                  | 🟡 stub register decode; TMS5220 LPC + AY-3-8913 synth cores deferred                  |
| 22 | PrinterCard (parallel synth)  | POM2-original    | Apple II slot 1 convention + Pascal 1.1 sig                              | 🟡 PDF export deferred (`.txt` OK)                                                       |
| 22bis | GrapplerCard (key `grappler`) | ROM-gated        | markadev/AppleII-RevEng/Orange-Micro-Grappler+ (4 KB EPROM)             | 🟡 4 KB EPROM now bundled (`roms/grappler_plus.bin`); upper-2 KB bank-switch modelled; remaining: MAME `a2grappler.cpp` pin + HGR→PDF raster                        |

## Quick wins

Suggested attack order — items with high impact/effort ratio.

| # | Item                                    | Effort  | Why                                |
| - | --------------------------------------- | ------- | --------------------------------------- |
| 1 | WASM IDBFS settings persistence         | 2-4 h   | web user has no state        |
| 2 | WOZ1 splice point TRK+6650              | 1 d     | Applesauce re-master parity             |
| 3 | Memory god-object split                 | 2 d     | prerequisite for IIgs + cuts recompiles  |
| 4 | Debugger runtime glue (BP / watch / step) | 3-5 d | 80% of the bricks are there (Disassembler + MemView) |
| 5 | ~~CI GitHub Actions (`ctest` headless)~~ ✅ DONE | — | the dormant ctest suite (~130 tests) now gated (see [Arch]) |
| 6 | ~~Desktop drag-drop disk (`glfwSetDropCallback`)~~ ✅ DONE | — | README promise kept (see [UI/UX]) |

## Backlog

Grouped by subsystem. Severity encoded by 🟠/🟡/🟢 at the head of each item.

### [Memory] paging & RAM expansion

- 🟠 **God-object split** — extract `Keyboard` (FIFO + strobe + paste)
  and `PaddleInputs` (RC + buttons + Open/Solid Apple) from `Memory.cpp`.
  `IIcPlusBank` already done (`MemoryProfile`/`IIcClassProfile`).
  *Prerequisite for IIgs. ~2 d.*
- 🟡 **Saturn 128K LC** (Saturn Systems) — 16 banks ×16 KB on LC
  `$D000-$FFFF`, switches `$C080-$C08F` slot-relative. MAME refs
  `bus/a2bus/a2memexp.cpp`. *2-3 d.*
- 🟡 **`Memory::memRead` hot path** — 7-level `if` cascade
  (`Memory.cpp:1309-1437`). 256-entry dispatch table per high page.
  Prerequisite: `IIcPlusBank` extraction.
- 🟢 **Dedicated Pascal LC** — 16 KB variant shipped with Apple Pascal,
  minor differences vs IIe LC (write-protect DIP). *1 d.*

### [Display] HGR / DHGR / 80-col

- 🟡 **OE-CPU demod runs under `stateMutex`** — *small*. `MainWindow.cpp`
  `drawScreenImage` holds the emulation mutex across `display->render()`;
  in `ColorCompositeOECpu` (and mixed OE-GPU frames) that includes the
  17-tap × 560×192 FP demod (~1-2 ms/frame), blocking the CPU worker every
  UI frame — reads as emulation/audio jitter, amplified under disk-turbo.
  The demod consumes only `signalBuf` (filled under the lock); run it
  after release. (2026-07-12 graphics hunt, verified.)
- 🟢 **Frame-wrap video-event off-by-one** — *small, rare*. An instruction
  straddling the exact 17030/20280-cycle video-frame boundary publishes
  its soft-switch event into the *closing* frame (applied one frame
  early); ≤ ~7 cycles of exposure per frame. Fix: at publication, retain
  events stamped `>= frameBoundaryCycle` in the new recording log.
- 🟢 **`renderCompositeOeCpu` lacks PAL line-phase alternation** — with
  `ntsc_pal` on, mixed OE-GPU frames (CPU-demodded) treat hue differently
  from full-screen frames (GPU shader path).
- 🟢 **Golden coverage gaps** (from the 2026-07-12 audit): flash-on phase,
  ALTCHAR/mousetext + char-ROM glyphs, PAGE2/80STORE scanner-page scenes,
  rev-0 DHIRES+80COL-off HGR (would have caught the paintHgr bit7 bug),
  IIe 80COL+HIRES+MIXED without DHGR, Chat Mauve sub-modes hash-frozen,
  PAL beam-raced splits. Also: OE-GPU uploads the unused ~430 KB fallback
  framebuffer every frame (minor perf).
- ✅ **CRT post-process shader** — DONE. `CrtEffectStack` applies barrel →
  hue → BCS → phosphor curve → scanlines → shadow mask → vignette → luminance
  gain → edge-mask → persistence on any framebuffer, with a "CRT Settings
  (sliders)" panel and `crt_effects_enabled` toggle. Detail → `DEV.md` §
  CrtEffectStack.
- ✅ **Signal-level analog NTSC pipeline** — DONE. Signal-level demod
  (`ColorCompositeOE` GPU + `ColorCompositeOECpu`: 14.318 MHz waveform → FIR
  Y@2.0 MHz / chroma@0.6 MHz → YUV→RGB, PAL line-phase) plus the phosphor
  curve added 2026-05-31 (`phosphorGamma`, key `ntsc_phosphor_gamma`). Detail →
  `CHANGELOG.md` / `DEV.md`.
  - 🟢 Remaining *(deferred, academic)*: pure-analog signal-level pipeline (IIR
    on the signal itself before demod) vs the current 1-bit signal + FIR, *5-10 d*.
- ✅ **Beam-racing per-scanline composite** — DONE (2026-05-31).
  `fillCompositeSignal(mem, events)` replays the event log band by band so
  mid-scanline switches reach the composite modes (OE GPU/CPU, AppleWin), not
  just LUT modes. Pinned `beam_race_composite`. Detail → `DEV.md` § Beam-racing.
  - 🟢 Remaining: `signalPhaseOffset_` stays a per-frame constant (mid-frame
    HGR↔DHGR split approximated); lo-res clips at block-row (4 lines), like the
    RGBA path.
- ✅ **Horizontal mid-scanline split (per-byte granularity)** — DONE
  (2026-06-09, 280-wide + composite signal + 560-wide IIe/Chat Mauve).
  Beam-racing is no longer scanline-quantized: `renderBeamRacing`
  (`Apple2Display.cpp:~336`) reconstructs per-scanline column segments so a mode
  can change mid-scanline (TEXT/LORES/HGR on the same line — Codebreaker GEN2
  "color peg"); split is visible in ColorCompositeOE GPU/CPU + AppleWin and in
  80-col/DHGR/DLGR/Le Chat Mauve. Pinned `horizontal_split`,
  `horizontal_split_composite`, `horizontal_split_560`. Detail → `DEV.md` §
  Beam-racing / `CHANGELOG.md`.
  - 🟢 Remaining *(deferred)*: 40-col (280) + 80-col (560) mixed on the same line
    is undefined (separate `frame`/`frame80` buffers, scoped out); exact
    transition cycle at character-clock = later refinement. **Back-port to POM1**
    next (gated: LORES+TEXT rendering on GEN2 — HGR-only today — + HBLANK flag
    Phase 2 per Bernie's spec).
- ✅ **PAL 50 Hz machine timing** — DONE (2026-06-09, full machine). `enum
  VideoStandard{NTSC,PAL}` + `VideoTiming` table in `CpuClock.h` (NTSC
  262/60/1.0227 MHz, PAL 312/~50/1.0156 MHz, 20313 cyc/frame), threaded through
  `Memory::pushVideoEventLocked`, `Apple2Display::frameCycleToPos` and
  `EmulationController::setVideoStandard`. PAL profiles `Apple //e PAL` and
  `Apple //c PAL (Le Chat Mauve)`, wired in `applyProfile`, Presets menu, and CLI
  `--preset iie-pal|iic-pal|chatmauve`. Pinned `pal_timing`, `video_event_publish`.
  Detail → `CLAUDE.md` § System profiles + `docs/test_corpus.md` § DIX /
  `CHANGELOG.md`.
  - 🟢 Remaining: device clocks (AY/IWM/SSI263) stay at NTSC nominal (0.7 % delta
    = inaudible audio pitch, not retimed — speaker + cassette realtime audio ARE
    retimed since 2026-07-11/12, their queues starve audibly otherwise); WASM
    pacing (RAF 60 Hz) not yet
    switched to 50 Hz; manual NTSC/PAL toggle + auto-PAL when a Chat Mauve card
    is plugged (the PAL profiles already cover the use case).
- ✅ **DROL — page-flip flicker + cut-scene hang** — DONE (2026-06-10). Three
  bugs found booting the real `Drol.woz`/`.dsk` (probe `tests/drol_probe.cpp`):
  (1) page-flip flicker — `forEachBeamSegment` now detects unidirectional PAGE2
  events in a frame as a buffer flip → final full-frame page (bidirectional DIX
  MODPAGE keeps exact replay); (2) cut-scene hang — `$C05x`/`$C030-3F` reads now
  toggle the mode/click AND return `floatingBus()` instead of hard 0; (3) the 6
  560-wide painters now read band state, not live state. Pinned
  `drol_pageflip_render`, `vapor_lock` §(d). Detail → `DEV.md` § Beam-racing +
  `CHANGELOG.md`.
  - 🟢 Assumed limit: an intentional unidirectional mid-frame page split renders
    full-page (true remedy = incremental per-scanline rendering MAME-style).
- ✅ **Le Chat Mauve HGR resolution (AppleWin RGB decode)** — DONE
  (2026-06-10). `renderHiResChatMauve80` ported AppleWin's `RGBMonitor.cpp
  UpdateHiResRGBCell`: a pixel is COLOR only if it forms an isolated 010/101
  pattern with its neighbors, otherwise black/white at full 280 px resolution so
  white runs (text, outlines) regain their sharpness. Pinned `le_chat_mauve_smoke`
  + `display_persistence_smoke`. Detail → `CHANGELOG.md`.
- 🟡 **Eve Color text mode `$C0B9`** — Chat Mauve/Eve variant, FG/BG
  per character. Stub `LeChatMauve_ImGui.cpp:200`. *2 d.*
- 🟢 **"Smooth" interpolated sub-pixel mode** — bilinear/Lanczos on
  HGR/DHGR, UI toggle. Inspired by microM8. *2 d.*
- 🟢 **DHGR mono 1-px alignment + floating-TTL `empty_words` +
  per-scanline mode switch** — cosmetic / out-of-bounds.
- 🟢 **CRT parity refinements vs OpenEmulator** — low-priority residuals from
  the 2026-05-30 video audit (detailed implementation notes →
  `docs/archive/video_parity_revalidation_2026-05-30.md` §4):
  - **F4** POM2 CRT defaults 0.25/0.5/0.4 (scanlines/mask/persistence) vs OE
    ~0.05/0.05/0 — *biggest visual gain*, OR own the "punchy" choice and
    document it (`NtscPostProcessor.h`).
  - **F3** vignette center-lighting ~4× too strong (`cuv = 2×qc` in OE,
    `CrtEffectStack.cpp`).
  - **F2** cosine scanline → OE's **sin²** (keep the `scanAA` anti-moiré term).
  - **F7** HGR mono: 280 px average / 3 levels → **560 binary** (copy of the
    DHGR-mono loop already shipped).
  - **F6** row-dim mask ×0.7 ⚠ (make luminance-neutral, don't drop hard).
  - **DLGR not wired** to `fillCompositeSignal` (still emits lo-res 40-col
    main-only under OE/AppleWin).
  - *(Non-items, documented: F1 clamp double > AppleWin float; F8/F9 amber/green
    tints assumed — optional "AppleWin-faithful" preset.)*
- 🟢 **Le Chat Mauve EVE** (64 KB ext RAM + SPEC1/SPEC2/DASH/COL280),
  **Video-7 AppleColor RGB**, **Color killer Rev 1**,
  **Strapping RAM 4K→48K**.

### [Audio]

- 🟢 **8-bit DAC (Marczewski)** — 8-bit slot latch → R-2R DAC. Niche
  demos (Music Studio, trackers). AppleWin refs `Card::CT_DX1`. *1 d.*
- 🟢 **Passport MIDI Music Card** — 6840 + 6850, Master Tracks Pro /
  Performer. MAME refs `mc6840.cpp` + `acia6850.cpp`. *3 d.*
- 🟢 **Phasor stereo** — POM2 mixer is mono-only; when stereo, pan L/R
  per AY-pair on Phasor (and SSI263/Echo+).
- 🟢 **AY Port A read mask by DDR** (R14/R15) — academic.

### [Storage] disks & images

- 🟡 **WOZ1 splice point (TRK+6650)** — `DiskImage::writeFlux` splices
  bit-cells but the full `set_write_splice` handling (TRK +6650
  splice_point/nibble/bit_count fields, parsed at `DiskImage.cpp:720`)
  is ignored; IWM call site wired (`IWMDevice.cpp:235`, see the comment
  at `IWMDevice.cpp:48`). Applesauce re-master parity. *1 d.*
- 🟡 **SmartPort ProDOS multi-partition** — 1 image = 1 unit = 1
  volume today; multi-volume CFFA3000-style not supported.
- 🟢 **UI "Force DOS / Force ProDOS"** — backend ready
  (`DiskImage::loadFile(path, SectorOrder)` at `DiskImage.cpp:212`),
  button missing in `DiskLibrary_ImGui` / `DiskController_ImGui`.
  Auto-detect (extension + vol-dir content sniff `0x400`/`0xB00`)
  already covers 99 % of cases; manual override useful for ambiguous /
  non-standard / debug images. *~30 min.*
- 🟢 **Half-tracked NIB (88)** + **Applesauce `.nib2`/`.app`** +
  **Disk II in snapshot** — deliberately out of scope as long as
  WOZ covers it.
- 🟢 **Floppy Emu Dual-5.25" + Smartport-Unit-2 modes** — out of scope
  for v1 (4 main modes covered).

### [Cards] slot cards & peripherals

- 🟢 **Microsoft SoftCard (Z80) + CP/M — ✅ ALL 3 PHASES DONE 2026-07-12**
  (Z80 core zexdoc+zexall 100 % → `SoftCardZ80` card + generic DMA
  arbitration → CP/M 2.2 boots to `A>`: 44K v2.20 master on II+ 40-col
  and 60K v2.23 on //e 80-col, MAME-oracle-identical; pinned by
  `softcard_toggle` + media-gated `softcard_cpm_boot[_iie]`; see
  [DEV § SoftCard Z80](DEV.md#softcard-z80-softcardz80hcpp--cpm-phase-2)).
  Post-MVP ideas: Videx Videoterm 80-col for II+ CP/M, PCPI Applicard
  (reuses the Z80 core + DMA hook as-is), Turbo Pascal / WordStar /
  MBASIC corpus entries in `docs/test_corpus.md`.
- 🟢 **SmartPortCard leftovers** (2026-07-12 Liron audit follow-ups —
  STATUS pre-flight, the SmartPort `$Cn0D` dispatch and the real-ROM
  identity all landed same-day, see CHANGELOG): empty-bay WP error code
  is $2B where $28 "no device" is the honest one; boot failure is a
  silent `JMP $CnE0` loop (real firmware prints an error); 3.5-type units
  present WP-until-write-back while HDV bays are RAM-writable —
  inconsistent on the same card; CONTROL calls needing the control-list
  DATA (only code 0 works — the stub has no guest→device list copy);
  extended $4x calls return $01.
- 🟢 **SmartPort `$Cn0D` on armed //c-class hole is not fail-closed**
  (2026-07-18 hunt): the `$C500-$C5FF` hole doesn't cover `$C800-$CFFF`,
  so the `JMP $CE00` at `$C510` lands in //c internal firmware.
  Reachability is low (`$Cn07=$01` steers SmartPort-aware software to
  the ProDOS entry); a real fix needs the hole to also route the
  expansion window while armed. `SmartPortCard.cpp` (buildRom).
- 🟡 **`intC8Rom` + `SlotBus::activeExpansionSlot` not serialized**
  (2026-07-18 hunt): a snapshot/rewind taken while the PC executes
  inside `$C800-$CFFF` card code (now routine for SmartPort dispatch)
  restores with the expansion window unclaimed → the CPU fetches open
  bus. Add both to the Memory snapshot trailer. `Memory.cpp`,
  `MachineSnapshot.cpp`.
- 🟢 **`$C05E/F` ignores IOUDIS on //c-class** (MAME gates DHIRES on
  `m_ioudis`); II+ broadcasts `$C00C/D` on reads while IIe is write-only —
  both flagged for awareness by the 2026-07-12 Chat Mauve review.
- 🟠 **Z-80 SoftCard + CP/M** — Microsoft SoftCard, Z-80B clipped onto
  the 6502 bus, shares RAM via mode-switch. Unlocks the CP/M library
  (BASIC-80, dBase II, Turbo Pascal, WordStar). MAME refs
  `a2softcard.cpp` + Z-80 core. *10-15 d.*
- 🟡 **Grappler+ printer (`GrapplerCard`)** — ROM-gated shell in
  place (catalog `grappler`); the Orange Micro 4 KB EPROM dump is now
  **bundled** (`roms/grappler_plus.bin`, committed `ffdac5d`) so the card
  arms without a user-supplied ROM. The upper-2 KB bank-switch ($C0(8+s)X)
  is modelled (`romBankHigh_`, round-tripped through snapshot). Remaining:
  pin against MAME `a2grappler.cpp`, and host-side raster rendering of
  HGR dumps to PDF. *1-2 d.*
- 🟡 **EchoPlusTMS5220Card (real Echo+)** — catalog scaffold
  `echoplus_tms`: SlotPeripheral + stub register decode at
  $Cs00-$Cs0F, enough for detection. Remaining: TMS5220 LPC10
  decoder (chirp ROM + K-parameter interpolation) and AY-3-8913 audio
  synth (usable once the Mockingboard/Phasor core is extracted into a
  shared helper). *~3-5 d.*
- ✅ **No-Slot Clock (NSC, DS1216E)** — DONE. `src/NoSlotClock.{h,cpp}`
  is a full DS1216E SmartWatch state machine, hooked into `Memory`
  read paths (`interceptRead` under the $F800 ROM window) for machines
  with no free slot (//c). MAME refs `ds1216.cpp`. Pinned by
  `no_slot_clock_smoke` (`tests/no_slot_clock_test.cpp`).
- 🟢 **SSC IRQ gate SW2:6 DIP** not implemented (MAME `a2ssc.cpp:373`).
- 🟢 **Real ClockCard slot ROM** — load path in place
  (`roms/thunderclock_u9_v1.3.bin`, 256 B or 2 KB, source
  markadev/AppleII-RevEng). Remaining: ship the default dump + test
  against DOS 3.3 / Applesoft tools that load the driver from $C800.
- 🟡 **[P2] Real Liron / UniDisk 3.5 (IWM in a slot)** — stack already
  there (`IWMDevice` verbatim, `Sony35Drive`, zoned GCR, `SmartPortHub`).
  Remaining: `LironCard : SlotPeripheral` + ROM 343S0001.
  **Blocker**: no public ROM dump (MAME `a2iwm.cpp` *WANTED*).
  *~8-12 h excluding ROM sourcing.*
- 🟢 **[P3] Apple II SCSI / High-Speed SCSI + CHD** — MAME
  `a2scsi.cpp` (NCR 5380) / `a2hsscsi.cpp` (53C80). Big lift for a
  niche need (CFFA suffices). *~30-50 h.*
- 🟢 **Apple II VGA / Second Sight (VGA video card)** — slot card that
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
- 🟢 **UDC (Apple 1991)** — 4 heterogeneous bays (3.5"/5.25"/HDV).
- 🟢 **Slinky / RamFAST RAM disk** — limited utility vs RamWorks III.
- 🟢 **Apple 3.5" Controller IWM-level** — refactor IWMDevice attached
  to a slot card (rare).

### [Cassette]

- 🟢 **Enriched WAV record/playback** — POM2 supports .wav; missing
  analog tape filtering (hiss, drop-out), VU-meter, timecode.
  MAME refs `apple2.cpp` cassette. *2 d.*

### [Network]

- 🟠 **Uthernet I/II Ethernet TCP/IP** — unlocks modern
  IRC/HTTP/telnet/FTP. I = CS8900A NIC (`uthernet.cpp`); II = W5100
  hardware stack (`uthernetii.cpp`). Host backend = libslirp or TAP/TUN.
  *5-7 d.*

### [Printer]

- 🟡 **PDF export** — `PrinterCard` spool + `.txt` OK; remaining is a
  monospace renderer or libharu.

### [Input] joystick / paddles / mouse

- ✅ **Apple II square-gate stick** — DONE (2026-07-10).
  `JoystickInput::applySquareGate` expands the round modern-stick region to
  the full square so the corners (255/255) are reachable (Wings of Fury
  take-off); radial deadzone; toggle + persisted `joystick_square_gate`.
  Pinned `joystick_square_gate`. Detail → `DEV.md` § Joystick / `CHANGELOG.md`.
- ✅ **Kiosk gamepad disk selector** — DONE (2026-07-10). Start (or F10) opens
  a name-proximity-filtered picker of sibling disks; A mounts in-place, with
  Reset/Quit action rows. Detail → `DEV.md` § Host control (kiosk) /
  `CHANGELOG.md`.
- 🟡 **PADL(2)/PADL(3) host binding** — second stick centered at 127
  (`JoystickInput.cpp:65-75`).
- 🟡 **Mouse → paddles mapping** — paddle 0/1 on host mouse X/Y axes
  (alternative to pads).

### [Paint editor] HGR / GR / DHGR / DLGR (hgrpaint/ + hgrsprite/)

- ✅ **2026-07-12 batch — ALL 17 items DONE** (same day as planned): GR/DLGR
  screen-hole masking · HGR import scores LUT row 0 (pinned vs `renderHiRes`)
  · session persistence · canvas multi-pipeline (NTSC/Medium/4-bit/Chat
  Mauve) · 4:3 aspect · DHGR fringing overlay · 16-colour copy/paste +
  FlipH/V/Rot · MacPaint fill patterns · X/Y mirror symmetry · DHGR text ·
  onion skin · DHGR mono import · save-to-ProDOS (host-folder + `#TTAAAA`
  tags in `buildVolumeFromFolder`) · flipbook page 1↔2 + ghost · sprite
  editor port (`hgrsprite/`) · **DLGR mode** (aux nibble rotation pinned) ·
  **DHGR NTSC 8-px chroma import** (ii-pix palette, BSD-2). Detail →
  `DEV.md` § Paint editor, why → `CHANGELOG.md`.
- ✅ **Remaining niceties — DONE (2026-07-12, same evening)**: mono lo-res
  rendering (GR + DLGR nibbles display as their 14 MHz bit patterns through
  the phosphor, pinned in `dhgr_paint_model`); composite canvas pipelines
  (AppleWin NTSC + OE-CPU added to the editor's pipeline combo — the
  NTSC-8-px import previews faithfully); sprite editor DHGR target
  (stamp/grab/preview/ASM-export the shape as 16-colour fat pixels,
  aux+main pair tables).

### [UI/UX]

- ✅ **Desktop disk drag & drop** — DONE (2026-05-31). `glfwSetDropCallback`
  wired in `main.cpp`, routes the first recognized file via `insertAndBootImage`
  (auto-route Disk II / SmartPort 3.5" / ProDOS HDV) and reports the result in
  the status bar; unrecognized extensions are flagged. Detail → `CHANGELOG.md`.
- ✅ **Onboarding: Welcome / no-ROM panel** — DONE (2026-05-31).
  `renderWelcomePanelWindow`: no-ROM banner with probed dirs, expected ROM name
  for the active profile, "Reload ROM (re-probe)" button, plus quick-start;
  auto-opened on first launch without a ROM, also via `Help → Welcome / Quick
  Start`. Detail → `CHANGELOG.md`.
  - 🟢 Remaining: deeper guided tutorials.
- ✅ **UI density / discoverability** — DONE (2026-05-31). `Devices` menu
  grouped under `SeparatorText` headers via the `devItem` helper that adds a
  tooltip to every entry; `F6` shown on Rewind, ~25 tooltips added. Detail →
  `CHANGELOG.md`.
  - 🟢 Remaining: airier default layout (dedicated item below).
- 🟢 **MicroM8-style Rewind** — continuous state recording +
  scrub/step-back/rewind-live. **Phases 0→5 done** (2026-05-31,
  `CHANGELOG.md`): memory backend `SnapshotIO`, shared `MachineSnapshot`,
  `RewindBuffer` (keyframes + XOR deltas, memory budget), frame-boundary
  capture (`workerLoop` + `tickFrame` WASM), parked-worker transport +
  `Rewind_ImGui` UI (timeline / transport / `F6` rewind-live), `DiskIICard`
  drive state via `SlotPeripheral::*SnapshotState`, audio flush on restore.
  Pinned `snapshot_memory_roundtrip`, `rewind_roundtrip`, `rewind_delta`,
  `rewind_transport`, `rewind_slot_state`, `rewind_audio_state`
  (Mockingboard/Phasor VIA+AY+SSI263 → music **and** speech survive the rewind),
  `rewind_disk_write` (DiskIICard snapshot v2 = nibble track buffers → disk
  writes are undone on rewind). **Remaining**: writes to a writable WOZ not
  undone (`wozRaw` is a separate store; WOZ originals usually write-protected);
  "redo" (replay an undone future) not implemented. Detail → `DEV.md`
  § Rewind / time-travel.
- 🟡 **MicroM8-style 3D voxel view ("Voxel Cube")** — screen **stood up**
  (monitor, XY plane) as a 4:3 slab of **uniform-depth** cubes + per-color "pop"
  relief, orbital camera. NB: the initial luminance extrusion gave flat
  stalactites — fixed after scraping MicroM8 (cf. `CHANGELOG.md` + `DEV.md` § 3D
  voxel view). **Phases 0→3 done** (2026-05-31, `CHANGELOG.md`): `Mat4.h`
  (Vec3+Mat4+OrbitCamera, pinned `voxel3d_math`), `Voxel3DRenderer` (instanced
  cubes, FBO+depth, per-vertex color texture-fetch, derivative shading,
  **anti-moiré supersampling** + contiguous `cubeFill=1`), **native resolution**
  (1 voxel/pixel, 280|560×192), tap **before** `CrtEffectStack` (independent of
  CRT effects), *(P2)* left-drag orbit + middle-button **pan** + wheel zoom,
  *(P3)* View ▸ "3D voxel settings…" panel (depth/pop/fill/AA/ambient/mono/
  per-colour, persisted `voxel_*`), *(P4)* **WASM perf guard** (`ss≤2`+FBO≤2048²+
  `gridW≤280` under Emscripten), *(bonus)* **Mono mode** + **depth by color
  index** (snap lo-res palette `kVoxelPalette`). **WASM build OK** (+ browser
  wheel fix: `emscripten_set_wheel_callback` → `io.MouseWheel`, cf. `main.cpp`).
  **Remaining**: *(P5, deferred on request)* rewind tie-in "freeze + orbit a
  rewound frame" — already works for free (the view samples the live framebuffer
  that rewind restore updates), so doc + polish rather than plumbing; *(option)*
  alternative heightfield-mesh mode. Detail → `DEV.md` § 3D voxel view. *P5≈0.5 d.*
- 🟢 **Airier default layout** — ImGui Docking or
  `SetNextWindowPos` adaptive cascade.
- 🟢 **`isDuplicate` flags cffa/smartport35 duplicates** in the Slot
  Config assignment column — cosmetic.
- 🟢 **On-screen touchscreen / virtual joystick** — ImGui virtual
  joystick for mobile WASM builds (separate from raw touch routing). Two
  thumb-sticks + Open/Solid Apple buttons. Inspired by microM8 / A2TS.
  *2 d.*

### [WASM]

- 🟡 **IDBFS settings persistence** — `/persistent` mounted via IDBFS
  (`CMakeLists.txt:241`) but `Settings.cpp` writes to `$HOME`;
  `state.cfg` + `imgui.ini` do not survive a reload. Route via
  `ResourcePaths` under `__EMSCRIPTEN__`. *2-4 h.* ⭐ quick win
- 🟡 **File picker / drop-zone disks** — build-time bundling
  only. HTML5 drop-zone → `FS.writeFile('/uploads/…')` →
  `DiskIICard::insert`. *~1 d.*
- 🟢 **Mobile touch input** — GLFW3 under Emscripten does not map
  touch → mouse off-canvas. JS wrapper `touchstart/move/end` →
  `Module._inject_mouse_*`.
- 🟢 **Audio worklet tuning** — miniaudio Web Audio works but
  latency ~150 ms is audible on speaker click. Explore a custom
  `AudioWorkletNode` or shrink the buffer.
- 🟡 **"Zero-friction" web demo** *(commercial audit 2026-05-31)* — the WASM
  lever is throttled: user-provided ROMs + no bundled disk → the browser demo
  does not start turnkey like POM1, which kills instant conversion and viral
  sharing (3D voxel / rewind). Bundle **royalty-free demo disks** (without
  touching proprietary ROMs) playable on the WASM side. A marketing prerequisite
  before pushing to r/apple2 + Hacker News; aim in parallel for a **stable 1.0**
  (finish the //c+/IWM boot, cf. parity dashboard). *1-2 d excluding media
  sourcing.*

### [Arch] refactor & tooling

- 🟢 **Z80/SoftCard cleanup backlog** (2026-07-12 bug-hunt survivors — quality,
  not correctness): SoftCardZ80 SFZ2 blob → `pom2::byteio` putU16/Reader like
  every other card (3 hand-synced layout copies today);
  `softcard_cpm_boot_test` → `pom2::findResource` + `Apple2Display::
  textRowAddress` instead of private copies (6th in-repo transcription of the
  text-row interleave); Z80.cpp decoder dedup: rp-selector switch pasted 8×
  (readRP/writeRP helpers), JR cc's inline condition test vs `ccTest`,
  `memEA` body re-inlined twice for special timings (chargeless
  `indexedEA()` split); `xlate` 5-compare chain → 16-entry per-4K-page
  offset LUT; drop the per-instruction `mem_` null tests in the dmaRun hot
  loop (guard once at entry). Boot test could also pump a public
  controller slice hook instead of re-implementing arbitration.
- ✅ **CI GitHub Actions** — DONE (`.github/workflows/ci.yml`). Two jobs on
  push-to-`main` / PR / manual dispatch, with in-flight cancellation: **linux**
  builds the full tree (GUI + `pom2_headless` + tests, `POM2_ENABLE_TESTS=ON`)
  and runs the ~130-test ctest gate (Klaus 6502+65C02, Tom Harte curated,
  `cpu_cycle_count`, golden-hash display, boot traces); **wasm** is an Emscripten
  verification build (`build_wasm.sh`) asserting `wasm/POM2.{js,wasm}` +
  `index.html` are produced. Both jobs shallow-clone Dear ImGui (gitignored); no
  test depends on the user-supplied ROMs.
- 🟠 **`MainWindow.cpp` god-object (~6700 lines)** *(audit 2026-05-31)* — biggest
  single file in the repo, monolithic UI despite the `_Slots`/`_MemoryMaps`/
  `_ImGui` splits. Slows recompiles + hurts readability. Extract device-window
  groups into dedicated TUs (aim for < 3000 lines/file, like POM1's `MainWindow_*`
  discipline). *3-5 d.*
- 🟡 **Scattered config** — `POM2_*` env vars + CLI flags + `Settings`
  to centralize into a `Config` (env → CLI → Settings → defaults),
  list env vars in `--help`. *1 d.*
- 🟡 **`stateMutex` shared CPU+UI** (`EmulationController.h:118`) —
  `MainWindow_Slots` takes this lock during plug/unplug, audio jitter
  risk. Partition long-term.
- 🟡 **Inconsistent `pom2::` namespace** — 105/167 top-level files,
  `tests/` does not use it. Mechanical migration.
- 🟢 **Legacy M6502 style** — FR/EN comments, C-style casts,
  `void(void)`. Targeted `clang-format` + `clang-tidy modernize-*`.
- 🟢 **`*Card` raw pointers in MainWindow** (`MainWindow.h:97-103`) —
  no notification when SlotBus replugs. Observer pattern or
  `controller.slotBus().peripheral(N)`.

## Edge-case test corpus

Backlog of **manual / integration tests** with real software that tortures the
corners (cycle-exact CPU↔video sync, protected WOZ flux, VIA IRQ) — beyond the
unit `ctest`s. Curated list + POM2 status + cross-refs to the dashboard's
`Known gaps`: **[`docs/test_corpus.md`](docs/test_corpus.md)**.

- 🟠 **[DIX](https://github.com/Fr3nchT0uch/DIX/) — French Touch demo
  anthology**. **Priority reference** for emulation perfection: chains vapor
  lock, mid-scanline, Mockingboard, 128 KB aux, Unidisk/Liron. Validate DIX
  first before any other corpus title. Full description → `docs/test_corpus.md`.
- ✅ **Vapor lock** — DONE/proven (2026-06-09, extended 2026-06-10). Test
  `vapor_lock`: a real 6502 `LDA $C058 / CMP marker / BNE` loop locks on the
  marker in video RAM; `floatingBus()` tracks the beam per cycle. Scanner
  geometry is now PAL-aware (262/312 lines per `VideoStandard`); sub-instruction
  precision corrected (`$C0xx` read sampled at the access cycle); all
  non-driven `$C0xx` reads return the bus (`$C040`, `$C050-$C057`,
  `$C030-$C03F`). *(🟢 Remaining: non-last-cycle RMW-type accesses — outside
  vapor lock.)*
- ✅ **Mid-scanline video switch** (French Touch *Mad Effect*/*Plasmagical*,
  included in DIX) — DONE (intra-line per-byte-column rendering, RGBA + composite
  + 560-wide; cf. [Display]). Beam-racing vs double-buffer distinction: a
  unidirectional page flip = buffer (full-frame render, DROL anti-flicker),
  bidirectional = exact beam-racing. → `Gap #3` (residual: exact transition cycle
  at character-clock, 40/80-col mixed on same line).
- 🟡 **Spiradisc / RWTS18** (*Captain Goodnight*, *Prince of Persia*) — spiral
  tracking + weak bits to validate on real WOZ images. → `Gap #9/#10`.

## Deliberate skips (documented inline)

Conscious MAME divergences, justified in the code at the relevant spot.
Do not re-litigate without re-reading the original comment.

- 🟢 **`$C040` STRB not gated `!//c`** (MAME `apple2e.cpp:1927`) —
  no sink wired.
- 🟢 **ClockCard DATA_OUT live** vs MAME latch on CLK edge in
  MODE_SHIFT (`ClockCard.cpp:193-200`) — strict would break stock
  ProDOS.
- 🟢 **MouseCard PIA out_a/b without `scheduler.synchronize`** (MAME
  `mouse.cpp:280-294`) — no firmware-visible race.
- 🟢 **ClockCard offset model vs MAME `set_time`** — behaviorally
  equivalent as long as `timeFn()` is lock-step.
- 🔁 **MAME path drift refresher** — re-check ~every 6 months to
  track upstream renames (recent: `wozfdc.cpp`
  `bus/a2bus → machine`).

## Out of scope

Things we will not do unless explicitly requested + clear ROI.

- **Apple IIgs / ProDOS 16** — new project (Mega II + FPI + GLU +
  Ensoniq DOC, *30-100 d*).
- **Apple ///** + SOS — niche, *20-40 d*.
- **Clones** Franklin / Laser / Pravetz / Basis 108 — *2-5 d/clone*,
  low demand.
- **CFFA CompactFlash** — HDV + host folder suffices; MAME-faithful
  port already covered by P1 (CFFA done), P2/P3 above.

## Changelog

See [`CHANGELOG.md`](CHANGELOG.md).
