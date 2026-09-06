# Edge-case test corpus — real software for validating the emulation

> Curated list of programs (cycle-exact demos, copy-protected disks, CPU
> suites) that torture the corners of the Apple II: cycle-accurate CPU↔video
> sync, raw magnetic flux, 6502 timing, and IRQs. Serves as the **manual /
> integration test backlog** beyond the unit `ctest`s.
>
> Origin: curated research, re-verified and cross-checked against the actual
> POM2 subsystems. When a program targets a `known gap` from the
> [parity dashboard](../TODO.md#mame--pom2-parity-dashboard), the `#` number is
> cited. **Status** = what POM2 does *today*, not a promise.
>
> ⚠️ All commercial game images are **user-provided** (like the ROMs). This
> document references no binary; it describes *what to test and why it's hard*.

> **⭐ Priority reference — [DIX](https://github.com/Fr3nchT0uch/DIX/)**
> (French Touch anthology, 29+ min of Apple II demos 2014–2024, GPLv3 sources).
> It is the **most complete test bench** for chasing emulation perfection:
> vapor lock / floating bus, mid-scanline video toggles, Mockingboard + VIA IRQ,
> 128 KB aux, SmartPort/Liron + 800 KB Unidisk, PAL 50 Hz timing. If DIX runs
> glitch-free, the emulator is at the "cycle-exact demo" level; if DIX breaks,
> the corpus below tells you *which* subsystem to dig into.

## Contents

- [1. CPU↔video accuracy (cycle-exact sync)](#1-cpuvideo-accuracy-cycle-exact-sync)
- [2. Disk II controller hell (flux / WOZ)](#2-disk-ii-controller-hell-flux--woz)
- [3. CPU & hardware quirks](#3-cpu--hardware-quirks)
- [4. Audio / Mockingboard (VIA IRQ)](#4-audio--mockingboard-via-irq)
- [5. Le Chat Mauve — RGB cards](#5-le-chat-mauve--rgb-cards)
- [Appendix — Vapor Lock in detail](#appendix--vapor-lock-in-detail)
- [Corrections vs the original source](#corrections-vs-the-original-source)

---

## 1. CPU↔video accuracy (cycle-exact sync)

The Apple II has **no dedicated video timer and no VBL IRQ** (on II/II+; the //e
adds read-only `$C019` RDVBL). All fine-grained sync relies on the **floating
bus**: reading an undriven I/O address returns the last byte placed by the video
scanner (TTL capacitive effect). See
[Vapor Lock appendix](#appendix--vapor-lock-in-detail).

| Program | What it tortures | Why it's an edge case | POM2 status |
|---|---|---|---|
| **deater — "megademos"** (Vince "deater" Weaver, `deater.net/weave/vmwprod`) | Vapor lock: detects VBL by looping on an undriven `$C0xx` read until it reads a marker byte written into video RAM. | If video is a framebuffer rendered asynchronously at end-of-frame instead of interleaving CPU reads and the scanner cycle-by-cycle, the loop never "locks" → frozen / glitched screen. | ✅ **Proven (2026-06-09)**: `Memory::floatingBus()` = verbatim MAME port of `apple2video.cpp:124-201 scanner_address`, indexed on `cycleCounter`. The `vapor_lock` test *runs a real 6502 loop* (`LDA $C058 / CMP marker / BNE`) and **it locks** onto the marker placed in video RAM. The scanner geometry now follows the **video standard** (262 NTSC / **312 PAL**) — it was hard-coded to 262, which made the per-frame lock of PAL demos drift; fixed. **Sub-instruction accuracy fixed**: the `$C0xx` CPU read samples the bus at the **access cycle** (`cycleCounter + getCurrentInstructionCycles()` = last cycle of an `LDA`/`CMP`/`BIT`, consistent with the event-log timestamp) instead of the **start** cycle of the instruction. **All undriven `$C0xx` reads return the bus**: `$C040`, **`$C050-$C057`** (2026-06-10) and `$C030-$C03F` used to be 0. Pinned `vapor_lock` (§(d) = DROL cut-scene) + `floatingbus_page2_smoke`. *(Still 🟢: non-last-cycle `$C0xx` accesses, e.g. RMW — not used for vapor lock.)* |
| **DROL** (Brøderbund 1983; `disks_5.4/woz/Drol*.woz`, `disks_5.4/gist/Drol.dsk`) | Real game, double edge case: (1) **unsynchronized double-buffer page-flip** (`$C054/$C055` every ~4 frames at drifting positions) for animation; (2) **vapor-lock cut-scene** via `LDA $C050 / CMP #$80`. | (1) A naive beam-raced replay paints the band above the flip from the page **currently being redrawn** (RAM read at render time, not at the beam) → half-erased sprites. (2) A `$C050` read returning 0 instead of the bus → the loop never locks → freeze (historical LinApple hang). | ✅ **DONE (2026-06-10)**, diagnosed via `tests/drol_probe.cpp` (boots the real WOZ). (1) `forEachBeamSegment` distinguishes unidirectional flip (= buffer → final full-frame page, anti-flicker) vs bidirectional (= exact beam-racing, DIX MODPAGE). Pinned `drol_pageflip_render`. (2) reading `$C050-$C057` toggles the mode AND returns `floatingBus()`. Pinned `vapor_lock` §(d). Bonus: the 6 560-wide painters now take the band state (the bug that masked the flicker under Chat Mauve). |
| **[DIX](https://github.com/Fr3nchT0uch/DIX/)** — French Touch anthology (29+ min, //e / //c PAL, GPLv3 sources) | **All-in-one integration suite**: vapor lock, mid-scanline, DHGR/NTSC, Mockingboard, 128 KB aux, 800 KB Unidisk via Liron/SmartPort. Bundles *Mad Effect*, *Plasmagical*, *Wave* and the other recent FT productions. | A single disk that chains the edge cases of §1–4; the reference to hit before declaring the emulation "perfect." **Requires PAL 50 Hz (not NTSC).** MAME oracle = `apple2eefr` (312 vtotal, 50.146 Hz, 14.2375 MHz) — not US `apple2ee`. | 🟡 **Priority #1**. Mid-scanline rendering ✅ (see next row). PAL 50 Hz timing ✅ (`iie-pal`/`iic-pal` profiles, 312-line geometry everywhere: scanner, `$C019`, events). **50/60 Hz hand-off ✅ (2026-06-10)**: the event log is published **per video frame** (65×312 cycles) and consumed by *copy* by the 60 Hz UI — the old per-worker-tick bracketing lost events between the UI take and the next tick (~1 empty log in 6 under PAL → 10 Hz flicker of the splits; invisible to the tests, which bracket synchronously). **Headless end-to-end run ✅ (2026-07-31)**: DIX boots from `disks_3.5/DIX.po` on //e PAL through a slot-5 Liron-class SmartPort + slot-4 Mockingboard and reaches its **main menu** — FRENCH TOUCH HGR logo, particle spiral, and the beam-raced bottom text scroller all animating. Beam-racing census over 2500 frames: **1858 frames carry mid-frame video events, 14 211 events total, peak 330 in one frame** (~1 switch per PAL scanline = the MODPAGE signature). Note `DIX.po` is a **raw-boot** image, not a ProDOS volume (block 2 is 6502 code), so POM2's "doesn't look ProDOS-formatted at block 2" warning is correct and cosmetic. **MAME cannot serve as the oracle for this**, established by control runs against MAME 0.287: `apple2eefr`+DiskII boots DOS 3.3 fine (machine + PAL ROMs are sane), but `apple2eefr` + `-sl4 mockingboard` + `-sl5 superdrive -sl5:superdrive:fdc:0 35dd` fails to boot DIX **and** a plain bootable ProDOS 800 K image (3000 frames) — so it is neither DIX-specific nor a missing-Mockingboard artefact; `apple2c0fr` has the //c UniDisk 3.5 ROM but MAME attaches **no 3.5 drive** (no such slot); and MAME has **no Liron/SmartPort device at all**. Remaining: **visual validation on a real screen** (audio/tempo unverified headlessly). Probes: `dix_modpage_split`, `horizontal_split*`, `dhgr_phase_signal`, `floatingbus_page2_smoke`, `pal_timing`, `video_event_publish`. |
| **Block ASCII Anthology** (French Touch, `disks_5.4/demo/French Touch Demos/`, ships `eprom2164.bin`) | An Unenhanced-//e demo (no MouseText) that draws block art from a custom 8 KB char generator AND shows a normal-text intro — switching fonts at runtime via annunciator 2 ($C05C/$C05D driving the char ROM A12, the localized-//e mechanism). | Needs (1) the custom char ROM and (2) POM2 to follow the runtime AN2 char-ROM-bank switch — a plain single-font load gets one screen right, the other garbled. | ✅ 2026-09-02: char ROM shipped (`iie_ft_block`, dual-bank `bank -1`); `loadCharRom` keeps both 4 KB sets and `charRomActiveData()` selects the live one from AN2. Both the normal-text intro and the block art render correctly. Select the `//e — French Touch (Block ASCII custom)` char ROM. |
| **Crazy Cycles II** (French Touch, `disks_5.4/demo/crazycycles2/`, GPLv3 sources + `notes.txt`) | Part 4 flips PAGE 1/2 mid-line with cycle-exact timing; the page-flip text at bottom-left. | **NOT a bug — an intentional CPU tell.** `notes.txt`: Part 4 uses `JMP (IND)` = 6 cycles on 65C02 vs 5 on 6502; on a 65C02 the extra cycle is compensated for sync but the PAGE 2 switch lands **one cycle late**, so the bottom-left "I" does **not** fully disappear. The authors left it in as a visual 65C02-vs-6502 identifier. | ✅ POM2 faithful: `JMP (IND)` is 5 cycles NMOS / 6 CMOS (`M6502.cpp` `IndAbs`, MAME-parity). On the **Unenhanced PAL (6502)** profile the "I" disappears; on Enhanced (65C02) it slivers — both authentic. User-confirmed 2026-09-02. The PAGE2 switch is fetch-side, correctly on `frameCycleToPos`'s -24 (unrelated to the OLDSKOOL display-side -25 fix). |
| **"French Touch" productions** (e.g. *Mad Effect*, *Plasmagical*, *Wave* — included in DIX) | **Mid-scanline video mode changes** (TEXT↔HGR, PAGE1↔2, lo↔hi-res between two cycles of the same line). | Requires a 6502 split into **real per-access sub-cycles**: an opcode executed atomically (effects applied in one block) shifts the switch by 1-2 cycles → mis-placed color bands. | ✅ **Intra-line rendering done (2026-06-09)**: `Apple2Display::renderBeamRacing` replays the event log at the **byte-column** level (`frameCycleToPos`), horizontal TEXT/HGR/LORES/DHGR/80-col **and** PAGE1↔2 / ALTCHAR splits on the same line, in RGBA *and* composite signal. Probes: `horizontal_split`, `horizontal_split_composite`, `horizontal_split_560`, `dix_modpage_split`, `dhgr_phase_signal` (registered pins); `artifact_phase_probe` is a build-only diagnostic (no `add_test`). *The exact transition cycle at the character-clock remains a refinement.* Detail → `DEV.md` § Beam-racing. |
| **DHGR demos / `dapple`-like + NTSC artifact tests** | DHGR soft-switch evaluation order (`80STORE`/`PAGE2`/`HIRES`/`AN3`) and color fringing (NTSC artifacting via signal interleaving). | Validates the exact Le Chat Mauve switch order (AN3 FIFO → `$C05E/F`) and composite demodulation. | ✅/🟡 Composite NTSC pipeline (`NtscPostProcessor`, `AppleWinNtsc`) + CPU/GPU paths. Covered by `dhgr_render_smoke_test`, `oe_demod_gpu_cpu_parity_test`, `display_golden_hash_test`. Residual gap: 1-px mono DHGR, floating-TTL (`#3`). |

### Source-level DIX analysis — 2026-06-09

Reading the GPLv3 source ([Fr3nchT0uch/DIX](https://github.com/Fr3nchT0uch/DIX/),
e.g. `MADEF2/main.a`) to frame the validation. The flagship loop (`INT_ROUT1`,
page-aligned, run on the last VBL line) does, **every 65-cycle scanline** and
over 6 lines:

```asm
MODPAGE0  LDA $C054,X          ; PAGE1/PAGE2 mid-line (X = scroll offset)
MODLINE0  LDA $C056 (×11)      ; HIRES mid-line, ~44 cycles
```

It is **synchronized by a Mockingboard Timer-2 IRQ**:
`DEFAULT_SYNC_TIMER = 7479 ; IRL machines PAL`.

Consequences for POM2, cleanly separated:

1. **Mid-scanline rendering (PAGE/HIRES/mode) — ✅ DONE.** Byte-column
   beam-racing replays these toggles at the right column. **Bug found + fixed
   during validation**: the RGBA painters (`renderText/HiRes/LoRes`) re-read
   `mem.getDisplayState()` internally → the **PAGE2** (and `ALTCHAR`) selection
   used the *end-of-frame* state, not the band's. Fixed by passing the per-band
   `state` to the painters (the composite path already did so). Pinned by
   `dix_modpage_split` (the exact MODPAGE technique: page 1 on the left, page 2
   on the right, same line).
2. **Mockingboard Timer-2 IRQ — ✅ supported** (`Via6522` T2 one-shot phase-2,
   `IFR_T2`/`t2Counter`). The sync IRQ *fires*.
3. **PAL 50 Hz machine timing — ✅ DONE.** The `iie-pal` / `iic-pal` profiles
   carry true PAL machine timing (312 lines, 20313 cyc/frame, ~50 Hz refresh,
   ~1.0156 MHz), and the geometry follows the video standard everywhere
   (scanner, `$C019`, event log). `DEFAULT_SYNC_TIMER=7479` and the 312-line
   PAL geometry now place the effect vertically and pace the music correctly.
   Pinned by `pal_timing`.

### DIX boot on //e PAL — DONE (SmartPort `$Cn0A` entry, 2026-06-09)

The target profile is **`//e PAL` + Mockingboard slot 4 + SmartPort slot 5** (the
//c has no slot for the Mockingboard that DIX requires). Two bugs found and
fixed by driving DIX through the AI server + direct memory reads:

1. **Mockingboard detection ("KO")** — DIX (`boot_unidisk.a` `BADGUY`) writes
   `K`,`O` to `$400/$401` if a detection fails (`STX $403`: A=model, C=CPU,
   **M=Mockingboard**). The MB detection reads the 6522 Timer-1 counter at
   `$CX04` twice (8 cycles) and expects `-8`. → passes with the Mockingboard in
   slot 4 (POM2's 6522 T1 counts down correctly).
2. **Post-banner freeze** — DIX loads its menu (8 blocks → `$D000` RAM-LC) via
   `JSR $C50A`, the **fixed `$Cn0A`** driver entry of the real Liron/Unidisk
   firmware. POM2 synthesized its dispatch at `$Cn50` → `$Cn0A` = `$00` =
   **BRK**, and since DIX had just enabled RAM-LC reads (`LDA $C083 ×2`), the BRK
   vector was read from cold RAM-LC → permanent storm. **Fixed**: `JMP $Cn50` at
   `$Cn0A` (`SmartPortCard::buildRom`, same `$42-$47` convention). Pinned
   `smartport_unidisk_entry`. **This was NOT the Language Card / aux** (zero LC
   writes, MMU flags at zero) — diagnosis corrected.

**Result**: DIX boots, loads its demo into RAM-LC (`$D000+`) and **runs** — PC in
the demo code (RAM-LC), **both HGR pages filled** (`$2000` + `$4000`, animation
page-flip). The fidelity layers are in place: **PAL 50 Hz** ✅, **mid-scanline**
✅ (RGBA + composite + 560), **vapor lock** ✅ (proven + PAL-aware + access cycle),
**SmartPort `$Cn0A` boot** ✅, **Mockingboard Timer-2 sync** ✅ (IRQ at
`N+IFR_DELAY = N+3`, MAME `6522via.cpp:959`; pinned `via_t2_timing` — DIX sets
`T2 = 7512 − latency`), **50/60 Hz event-log hand-off** ✅ (2026-06-10: per-
video-frame publication, otherwise ~1 frame in 6 lost its splits under PAL;
pinned `video_event_publish`). *(Headless verify: `/mem` text/HGR page +
`/status` PC; `/screen.ppm` frozen without a UI loop. Remaining: real visual
observation to confirm the fine placement of the effects.)*

### DIX menu: RETURN before any arrow key wedges — DIX bug, not POM2 (2026-08-08)

Reported as "launch DIX, press RETURN straight away, the program dies".
Reproduced headlessly by `tests/dix_return_crash_probe.cpp` (//e PAL +
slot-4 Mockingboard + slot-5 SmartPort on `disks_3.5/DIX.po`) and traced to
an **off-by-one in DIX's own menu**, faithfully reproduced by POM2.

Confirmed against DIX's own GPLv3 sources ([Fr3nchT0uch/DIX](https://github.com/Fr3nchT0uch/DIX/),
ACME syntax): `MENU/main.a` (assembles to `$E000`) and `loader.a` (`$D000`),
both declaring `CurrentChoice = $DFFF` (`MENU/main.a:21`, `loader.a:64`).

DIX keeps the highlighted menu entry in **`$DFFF`** (LC bank 2) and
initialises it to **0** at `$D026` (`LDA #$00 / STA CurrentChoice`,
`loader.a:119-121`) — once, at cold boot; the `JMP -` demo loop
(`loader.a:130`) never re-zeroes it, so 0 is reachable only on the *first*
menu entry. Only the arrow keys ever give it a valid 1..16 value
(`MENU/main.a:178-211`):

```
$E10F  LDA $C000 / BPL $E0F4 / STA $C010
$E117  CMP #$88  BEQ $E129     ; LEFT   0 -> 17 then DEC -> 16
$E11B  CMP #$95  BEQ $E13E     ; RIGHT  INC -> 1
$E11F  CMP #$8D  BEQ $E153     ; RETURN launch
$E123  CMP #$A0  BEQ $E153     ; SPACE  launch
```

Index 0 is a *deliberate* display state, not an accident: `RefreshChoice`
(`MENU/main.a:224-230`) indexes 17-entry name tables (`MENU/main.a:730-731`)
whose entry 0 is the "use arrows to select demo" prompt (`DEMOX`,
`MENU/main.a:639`). But the accept path has no matching guard — `.return`
(`MENU/main.a:213-222`) loads `CurrentChoice` into a dead A (the only test
on it, `CMP #16 / BNE`, is commented out) and falls straight through to
teardown + `RTS`. The launcher then indexes a 16-entry jump table
**one-based** (`loader.a:117-153`):

```
$D02C  LDX $DFFF
$D02F  DEX                     ; 0 -> $FF   <-- underflow
$D030  LDA $D076,X / STA $D03D ; $D175 = $E1
$D036  LDA $D086,X / STA $D03E ; $D185 = $17
$D03C  JSR $17E1               ; garbage -> BRK storm in unwritten RAM
```

So **RETURN or SPACE pressed before any arrow key** jumps to `$17E1`. The
main thread then grinds BRK-by-BRK upward through empty RAM while the 50 Hz
Mockingboard IRQ music engine keeps servicing itself — picture frozen,
music still playing. That is exactly the reported symptom.

"Right after launch" is a red herring: an early keypress simply sits in the
hardware keyboard latch (DIX polls `$C000` only once the menu is up, ~17 s
in), so the wedge lands later. Injecting RETURN at 18 s, 25 s or 40 s wedges
identically. The index arithmetic is deterministic 6502 (`abs,X` does not
page-wrap), so real hardware and MAME behave the same.

**POM2 is exonerated**: press LEFT or RIGHT first and RETURN loads the part
normally — SmartPort reads at `$C58B`, the selected part runs at `$7xxx`
with video updating. Verified for both wrap directions (RIGHT → part 1,
LEFT → part 16), matching the asymmetric handlers in `MENU/main.a:189-211`
that clamp any arrow press into 1..16.

Upstream fix, if ever reported: `LDA CurrentChoice / BEQ MAINLOOP` at the
head of `.return` (`MENU/main.a:213`). Initialising `CurrentChoice` to 1
instead would lose the `DEMOX` prompt and the first-boot intro trigger at
`MENU/main.a:66-67`.

#### `DIX-fix.po` — the patched disk (2026-08-31)

`tools/make_dix_fix.py` builds `disks_3.5/DIX-fix.po` from the pristine
`DIX.po` (12 bytes differ) so the dead keystroke does the most useful thing
available instead of dying: **RETURN or SPACE on the fresh menu launches
menu entry 16, "ALL DEMOS IN AUTOMATIC MODE"** (`AUTOMODE`,
`loader.a:132-148` — `ExpoMode = 1`, all fifteen parts chained forever).
Index 0 keeps its `DEMOX` prompt and its first-boot intro trigger; only the
*accept* path changes.

The loader image is packed solid ($D000-$DDFF, blocks 1-7, the demo-name
text cut mid-word at `$DDFF`), but the boot block reads **8** blocks
(`boot_unidisk.a` `SP_BLOCKS2READ = 8`), so block 8 lands at `$DE00-$DFFF`
and its tail `$DF56-$DFFE` is 169 resident zero bytes. Nine of them hold the
stub:

```
$D02C  20 60 DF  JSR PICKDEMO     ; was  AE FF DF  LDX CurrentChoice
$D02F  EA        NOP              ; was  CA        DEX
$DF60  AE FF DF  LDX CurrentChoice / CA DEX / 10 02 BPL + / A2 0F LDX #15 / 60 RTS
```

`JSR`/`RTS` leave X alone and `LDA DemosL,X` ignores the incoming flags, so
the selected-demo path stays bit-identical. Verified by booting both images
through `dix_return_crash_probe --disk` on the same machine with the same
RETURN at 20 s: `DIX.po` → `RUNAWAY: BRK at $17E4` + 25 s frozen;
`DIX-fix.po` → `expo=1`, part code at `$7xxx`, video moving for 70 s, loader
re-entry at 87 s (AUTOMODE chaining). RIGHT-then-RETURN still runs demo 1
with `expo=0`. The patcher refuses any image whose two sites are not the
exact original bytes, and re-checks that table slot 15 still points at an
`AUTOMODE` starting with `LDA #1 / STA ExpoMode`; `--verify` re-reads a
built image.

---

## 2. Disk II controller hell (flux / WOZ)

**Logical-sector** emulation (`.dsk`, `.po`) is not enough: these titles require
the **raw magnetic flux** (`.woz`) + the behavior of the stepper motor and the
300 RPM rotation.

| Program | Protection | Why it's an edge case | POM2 status |
|---|---|---|---|
| **Captain Goodnight and the Islands of Fear** (Broderbund) | **Spiradisc**: data written on a **continuous spiral** (track `$01`→`$0E`), not in concentric circles. | The controller must follow head moves **"on the fly"** while the flux streams by; an LSS that resyncs per track crashes at boot. | 🟡 Event-driven LSS + WOZ bit-stream present (`DiskIICard`, `DiskImage`, `#9/#10`). Half-tracks handled; continuous spiral tracking **to validate** on a real WOZ image. Nearby tests: `woz_bit_timing_smoke_test`, `diskii_lss_smoke_test`. |
| **Prince of Persia** (Broderbund / Roland Gustafsson) | **RWTS18**: quarter-tracks, modified sync bytes, timing bits / weak bits. | The rotation speed, the sync-nibble spacing and the weak-bit interpretation must be consistent with the 6502 cycles → otherwise the protected tracks fail to read. | 🟡 WOZ + event-driven bit-cell timing (cf. `CLAUDE.md` *"disk-turbo"* + `emuCycles`). Weak/fake bits depend on the WOZ master. Pinned on the flux side: `woz_writeflux_smoke_test`, `woz_bit_timing_smoke_test`. `Gap #9`: WOZ1 splice TRK+6650. |
| **"Floating bus as RNG" disks** (Beagle Bros protections, some demos) | Use the floating-bus byte as a random seed. | Requires a **bit-exact** replication of the scanner counter (HBL included, "$1000 phantom row"). | ✅ Handled by the verbatim `floatingBus()` port (cf. comments `Memory.cpp:1605-1606` / `:1905`). This is precisely the use case cited in the code. |

---

## 3. CPU & hardware quirks

The foundation must be flawless **before** the video demos can pass.

| Program | What it validates | POM2 status |
|---|---|---|
| **Klaus Dormann — `6502_functional_test`** | 6502 arbiter: page crossing (+1 cycle), exact decimal (D) flag, etc. | ✅ `klaus_6502_functional` **PASSES**. Binary auto-downloaded + SHA256 verified (`tests/CMakeLists.txt`). |
| **Klaus Dormann — `65C02_extended_opcodes_test`** | 65C02 extended opcodes (BBR/BBS/RMB/SMB, `STZ`, `(zp)`, etc.). | ✅ `klaus_65c02_extended` **PASSES** @ `$24F1` (cf. `DEV.md` §CPU). |
| **NMOS "illegal opcodes" suites** (visual6502-derived) | Behavior of the undocumented 6502 NMOS opcodes. | 🟢 Partially — the `#1` dashboard row now records the $5C 8-cyc delta as **deliberate** (matches MAME, not Harte). Mainly covers the subset used in practice. Complete via `cpu_cycle_count_test`. |

> 241 `ctest`s in total (Klaus 6502+65C02, `cpu_cycle_count`, disk, video,
> audio…) — the count `ci.yml` asserts on every push. Headless CI is ✅ DONE —
> `.github/workflows/ci.yml` runs the suite on every push.

---

## 4. Audio / Mockingboard (VIA IRQ)

Stress-test of the **hardware IRQs**: the Mockingboard's VIA 6522 timers must
neither desync the main bus nor miss their acknowledgment.

| Program | What it tortures | POM2 status |
|---|---|---|
| **Ultima V: Warriors of Destiny** (Origin) | Mockingboard music driven by VIA timer IRQ continuously during gameplay. | ✅/🟡 Mockingboard A/C (2×VIA + 2×AY) verbatim (`#6`, `ay8910.cpp`, `Via6522`). Wire-OR IRQ via `SlotBus` (`#8`). To be heard in real conditions. |
| **Music Construction Set / Willy Byte / Rescue Raiders** (confirmed Mockingboard titles) | AY-3-8910 sequencing + IRQ cadence. | ✅/🟡 Same path as above. Good test bench for the accuracy of the T1/T2 timers. |
| **Phasor / SSI263 (speech)** | 2×VIA + 4×AY (Phasor), SSI263 formant synthesis. | ✅ `PhasorCard` verbatim (`#19`); AppleWin-faithful SSI263 (`#20`). |

---

## 5. Le Chat Mauve — RGB cards

The corpus for `docs/chatmauve_plan.md`: software written FOR the French RGB
cards, by their maker or for their modes, so that every pixel rule the plan
states is argued from a picture. The cards are combinational logic on the
video stream — what these disks exercise is the mode latch, the Eve's
`$C0B0-$C0BF` switches and CPREG auto-write, and the dot-level decodes
(LCM HGR, mixed DHGR boundaries, COL280, CP280).

| Program | Where | What it tortures | POM2 status |
|---|---|---|---|
| **Purplesoft** (Le Chat Mauve, rev B octobre 1983 — `purplesoft-revb-oct83-{system,demos}.dsk`; juillet 1983 `-{system,fonts}`; `-s1-system` / `-s2-grload` are locked copies with a `CREATEUR D'ECRAN`) | `disks_5.4/chatmauve/` (DOS 3.3) | The Eve's Applesoft `&` extension by the card maker. `& GR 1..10` selects table IX-1's ten modes through five switch tables (`PURPLESOFT*` rev B, runtime `$E06F-$E0A0`: AN3, ENHRCPREG, HR1, HR2, HR3 per mode); `& TEXT 1..6` = 40-col damier/bloc B&W, 40-col **colour** (TXT16, `& BACK=` / `& COLOR=` through CPREG), 80-col B&W, 80-col **green** (TXTGREEN); `& PLOT x,y TO x,y` in every mode (the COL280 bit order lives in there); `& CHRS`, `& WINDOW`, `& PRINT` (software fonts on the graphics page). `DEMO GR16K` walks modes 6-10 (COL140, COL280A, COL280B, CP280, BW560) with 100 random lines each; `DEMO TEXTE` walks the six text screens. | 🟡 **The switch tables are read and pinned** (2026-09-01): they settle two rows the manual's scan left ambiguous — **BW560 = HR2+HR3** (the same pair that is HRBW with AN3 on; the scan's "HR3 alone" was a misread) and **SPEC2 = HR1+HR2** — and `le_chat_mauve_smoke` § 10 pins them. `tests/purplesoft_eve_probe.cpp` boots the demos disk with an Eve, types `RUN DEMO GR16K` / `RUN DEMO TEXTE`, logs each switch change against `dhgrMode()/hgrMode()/textMode()` and writes PPM frames — the visual check of the model against the maker's own software. **Screens frozen** (2026-09-01 evening): `purplesoft_eve_screens` pins the 6 stable `DEMO GR16K` screens and the 7 `DEMO TEXTE` screens as ordered (switch-state, frame-hash) pairs. **`SCHEMA` in `disks_5.4/gist/Extasie disk2.dsk` is corrupted in that preservation** (11 264 B mostly zeros vs 3 886 B intact; the maker's slideshow decompressor derails on it — non-deterministically, past EOF — which POM2 reproduces to 99.7 % of RAM against an offline 6502-faithful reimplementation validated 100 % on the CAMELOT reference pair from the Reloaded source disk). On the user's call the gist image was **replaced in place** by the intact preservation (underground2e.free.fr `Extasie_Slide.dsk`; every other file byte-identical, the damaged version stays in git history): SCHEMA now renders a clean 560-mono electronics schematic through POM2, pixel-identical to the offline decompression. |
| **Purple Pascal** 1.1 (fév. 83 ×2, juillet 83, rev B) | `disks_5.4/chatmauve/purple-pascal-*.dsk` (Pascal-formatted, not DOS) | The Pascal unit driving the same switches. | ⚪ Not run yet (needs the Apple Pascal system). |
| **Extasie** (Chat Mauve Reloaded — `Extasie disk1.dsk`, `disk2.dsk`) — **the mouse goes in slot 4, no scan**: DET reads the firmware entry table at `$C412+` and calls it by self-modified `JSR $C4xx` (`$75FA/$7639`), so a mouse card in any other slot is never touched (found 2026-09-02 with `tests/extasie_mouse_probe.cpp`, which boots the editor, presses the menu keys and injects host motion — both `mouseaw` and the MAME-LLE `mouse` work in slot 4: clamp `[0..559]×[0..191]`, cursor tracks) | `disks_5.4/gist/` (ProDOS, DOS-order sectors) | The **mixed DHGR** mode the Féline / //c adapter have and the Eve lacks: `bit 7 de l'octet dans lequel se trouve le bit 0 du quadruplet`, with the boundary cases the Manuel Arlequin advises against — colour cell **cut** at a BW byte, BW run's **last dot repeated** into a colour byte (measured on the //c adapter, AppleWin PR #837). `$F2` image format, slideshow, `DSP.IMG` viewer. **Féline/IIc only**: on an Eve or RVB Graph the mixed latch folds to COL140 (the manual's own statement) — POM2 logs it once and the Chat Mauve panel + Slot Config tooltip say which model to pick. | ✅ **Rule modelled and pinned** (2026-09-01): per-byte 560/140 mux over a free-running 4-dot cell latch, `chatmauve_dot_rules` compares 3×32×192 random rows with a port of AppleWin `UpdateDHiResCellRGB` and spells the three boundary cases out dot by dot; `display_golden_hash` freezes `cm/feline/dhgr-mixed-boundary`. The disks themselves are not booted headlessly yet (the viewer wants a 128 K //e + ProDOS; on the list). |
| **Chat Mauve demo sides** (side 1 = Pascal `EDITEUR`; side 2 = ProDOS `/ARLEQUIN`: `GLI16.2`, `GLIDATA`, `STARTUP`, `*.CHAR` fonts, image files `AIGLE`/`MIAOOU`/`MAMMOUTH`/…) | `disks_5.4/chatmauve/chatmauve-demo-side{1,2}-*.po` (fetched 2026-09-01 from apple2.org.za, Le Chat Mauve Eve / Disk Images) | The maker's own graphics library and slideshow — the reference `STA $C00C ; $C05E ; $C05F` sequences, the software fonts, and the Arlequin mixed-mode images (a **Féline** program; "la carte Eve n'est pas compatible"). | 🟡 **Boots headless** (2026-09-02, `extasie_mouse_probe` with `POM2_PROBE_DISK`): side 2 reaches the « MENU PRINCIPAL ARLEQUIN » (80-col), « DÉMONSTRATION » runs and its « MENU DE DEMO » renders full-screen **Féline mixed mode** (yellow condensed text on a magenta field, 560 wide) — the maker's own mixed-mode text driver working end to end. « ÉDITEUR GRAPHIQUE » asks for side 1 (Pascal). « LE MUSÉE » navigation is timing-fussy headless; a pinned golden of the demo menu screen is the natural next step. **Eve Leonard** is still to find (not on this mirror page). |
| **DIX** (French Touch), **Prince of Persia** title | see § 1 | Chat Mauve screens under a //c adapter: mid-line mode changes (DIX, plan P6) and PoP's title dropping to mono after the first attract loop — a quirk of the adapter's *inferred* 80COL, not of the Féline (plan P5). | 🟡 Per-frame card state today; the dot-clock tap and the adapter quirk are P6 / P5. |

The unit oracles behind the rows: AppleWin `RGBMonitor.cpp` (`UpdateHiResRGBCell`,
`UpdateDHiResCellRGB` — both validated by fenarinarsa on a real //c adapter) for
the Féline rules, MAME `apple2video.cpp` `dhgr_update` for the Video-7 modes
(`video7_parity_smoke`), and Purplesoft's own tables for the Eve's switches.

---

## Appendix — Vapor Lock in detail

A **purely software** solution to the lack of a VBL IRQ on II/II+. The
mechanics, from the physics to the POM2 C++:

1. **Shared bus (Φ0/Φ1 interleaving).** No dedicated VRAM: the CPU (6502) and
   the video scanner share the same RAM. Within a 1 MHz cycle, the **low phase**
   serves the scanner (generates the pixels), the **high phase** serves the CPU.
   Each µs, the bus carries first a video datum, then a CPU datum.
2. **Floating bus (TTL capacitance).** When no component drives the bus (reading
   an empty I/O, e.g. the `$C050-$C05F` mirrors during VBL), the lines hold the
   **last value** for ~½ µs via parasitic capacitance (~50 pF) — the one placed
   by the scanner just before.
3. **Algorithm.** The program writes a marker pattern (e.g. an isolated `$FF`)
   into a corner of video RAM, then tight-loops reading the floating bus. As
   soon as it reads `$FF` back, it knows the **exact beam position** at that
   cycle → "locked" sync. VBL is detected because the scanner stops reading
   structured video RAM.
4. **Emulator trap.** If the duration of `ExecuteCycle()` isn't exact (forgotten
   penalty cycle of a page-crossing `BCC`/`BCS`, etc.), the CPU drifts against
   the scanner and the lock slips after a few scanlines → glitches/crash. The
   alignment must be **perfect**.

**On the POM2 side.** `Memory::floatingBus()` (`src/Memory.cpp:1888/1897`)
computes the scanner address from the global `cycleCounter` (65 cycles/line ×
the video standard's line count — 262 NTSC / 312 PAL), a **verbatim** port of
MAME `apple2video.cpp scanner_address`. Reads of undriven soft-switches return
this byte (a dozen call sites — `grep floatingBus src/Memory.cpp`). It is the
foundation that makes vapor lock *possible*, and it is proven end-to-end: the
`vapor_lock` test locks a real 6502 loop (2026-06-09) and the headless DIX run
reaches its animated menu (2026-07-31) — see §1.

---

## Corrections vs the original source

The original conversation contained a few inaccuracies, corrected here:

- **"Megademo by Deater (Peter Ferrie)"** → **deater = Vince Weaver**. Peter
  Ferrie (aka *qkumba*) is a different person (cracks / protection analyses,
  distinct from deater's megademos). Do not confuse them.
- **"Skyfox … Mockingboard"** → *Skyfox* (Ariolasoft/EA) outputs mostly through
  the **speaker**, not the Mockingboard. Replaced with **confirmed** Mockingboard
  titles (Ultima V, Music Construction Set, Rescue Raiders, Willy Byte).
- **VBL.** The absence of a VBL IRQ applies to **II/II+**; the **//e** exposes
  `$C019` RDVBL on read (still no IRQ). Clarified in §1.
