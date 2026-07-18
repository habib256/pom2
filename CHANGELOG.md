# POM2 — Changelog

Notable changes, ordered most recent to oldest. The `git log` remains the
canonical source for the exact mechanics; this file captures the **"why"**
and the pitfalls we don't want to rediscover. Active backlog → `TODO.md`.
Current implementation → `DEV.md`.

## 2026-07-18 — Print with e-mail (printer spool → mailto)

The printer panel gained an "E-mail spool" button: composes an RFC 6068
`mailto:` URL (spool text as the body, timestamped subject, persisted
recipient) and opens the host's default mail client. **Why mailto and
not SMTP**: no credentials/network code in the emulator, works on every
desktop OS *and* the WASM build (where file saves are impossible — this
is the browser build's first way to get a printout off the page).
Pitfall pinned by `printer_email_smoke`: the URL is launched through
`system("xdg-open '...'")` on Linux, so *everything* shell-relevant must
be percent-encoded — the test asserts no quote/backtick/`$` can survive
`buildMailtoUrl`. Body capped at 8 000 chars (mail clients truncate
long URLs silently; we truncate loudly with an in-body marker instead).

## 2026-07-12 — SoftCard/Z80 bug hunt: 6 confirmed, 6 fixed, 1 refuted-as-faithful

Adversarial review (8 finder angles + per-candidate verification) over the
day's Z80/SoftCard/DMA work. Fixed same-day:

1. **WASM link break** — `runCpuSlice` was defined inside the
   `#ifndef __EMSCRIPTEN__` worker block while the unguarded `tickFrame`
   (the browser-RAF CPU driver) calls it. Moved above the guard.
2. **Z80 `IN r,(C)` MEMPTR** — WZ was computed *after* the register
   write, so `IN B,(C)`/`IN C,(C)` latched WZ from the modified BC. Port
   address captured first now. zex can't catch this class (no I/O).
3. **Unbounded DD/FD chains** — a whole prefix run used to fold into ONE
   `Z80::step()` (a crashed guest in a $DD/$FD sea → one giant
   advanceCycles lump under stateMutex; wrapped 64 K of prefixes → host
   hang). Each prefix now retires as its own 4-T step with interrupts
   deferred to the opcode (`State::pendingPrefix`, faithful to silicon);
   SoftCard blob bumped to `SFZ2`. `Z80::run()` (dead, misleading
   contract) deleted.
4. **Snapshot-load during CP/M** — file snapshots carry no SLOTn
   sections, so restoring left a live SoftCard `enabled_` and the stale
   Z80 executed over the restored RAM. `restoreMachineState` now
   force-disarms DMA claimants first (rewind, which captures slots,
   already round-tripped correctly).
5. **Step verbs during CP/M** — debugger/CLI/AI single-step always
   stepped the parked 6502, running its post-hand-back continuation
   early (a DMA-halted CPU executes nothing). New
   `EmulationController::stepBusMaster` steps the claimant's Z80
   (`dmaRun(1)` = one instruction) instead.
6. **POM2_TRACE_HANG false positive** — the detector samples the 6502 PC
   per frame; under Z80 ownership that PC is legitimately frozen →
   guaranteed "HANG DETECTED" spam on healthy CP/M. Sampling now skips
   (and resets the ring) while a DMA claimant owns the bus.

Refuted with evidence, kept as-designed (documented in DEV § SoftCard):
Z80 writes to $C007 via the $E000 window wedging a IIe until RESET is
what real MMU silicon does (UTAIIe 5-28) — MAME's write-through there is
its own simplification. Cleanup backlog (ByteIO for the SFZ2 blob,
findResource/textRowAddress reuse in the boot test, rp-selector/ccTest
dedup in the Z80 decoder, xlate LUT) → TODO [Arch].

## 2026-07-12 — CP/M 2.2 boots to A> (SoftCard Phase 3 — plan complete)

Microsoft CP/M 2.2 now boots end-to-end: Disk II loads the system tracks,
the 6502 loader finds the SoftCard by its $CnXX write probe, the Z80 runs
CCP/BDOS/BIOS out of the six translated windows, and a live `A>` prompt
lands on the text page in ~11 M cycles. Two media-gated ctest gates:
`softcard_cpm_boot` (II+, 44K v2.20 1980 master, 40-col) and
`softcard_cpm_boot_iie` (//e, 60K v2.23, 80-col) — the latter verified
against the MAME `apple2ee -sl4 softcard` oracle with a byte-identical
banner.

The bring-up lesson: every "failure" was a **sysgen/machine mismatch,
not an emulation bug**. The 56K/60K sysgens require the IIe-class
console — on a II+ they paint $00s (MAME does the same), and their
output goes through the IIe 80-col firmware, which stores even display
columns in AUX $0400: a main-RAM screen scrape shows every second
character missing ("Sfcr PM" for "Softcard CP/M") and reads like a Z80
bug until the aux page is interleaved in. The 44K 2.20 master is the
correct II+ image. Don't re-diagnose this; check the sysgen first.

## 2026-07-12 — Microsoft SoftCard card + dual-CPU DMA arbitration (CP/M Phase 2)

`SoftCardZ80` (catalog `softcard`) ports MAME `a2bus/a2softcard.cpp`. Two
findings corrected the Phase-1 plan the moment the MAME source was read —
worth remembering because both were "obvious" wrong guesses:

- The bus toggle is a **write to $CnXX** (the slot-ROM window), not a
  DEVSEL $C0nX access, and reads never toggle.
- The Z80→6502 translation is **six windows, not `+$1000` with wrap**:
  the Z80's $B000-$DFFF lands on the Language Card ($D000-$FFFF), $E000
  reaches the I/O page (that's how the Z80 releases the bus itself), and
  $F000 wraps to the zero page. CP/M's 60K layout only exists because of
  the LC remap.

Arbitration is a generic DMA daisy-chain hook (`SlotPeripheral::
dmaActive/dmaRun`, `SlotBus::dmaClaimant`, `EmulationController::
runCpuSlice`) rather than SoftCard special-casing — MAME's a2bus has the
same abstraction, and a future Applicard reuses it as-is. Hand-over is
instruction-precise both ways (the granting STA calls `M6502::stop()`,
whose `run()` re-arms on the next call — the same yield WAI/STP uses; the
chunk remainder goes to the other CPU). The Z80's 2× clock is converted
2 T-states → 1 cycle with an odd-T carry so `emuCycles` stays in the 6502
domain — video/LSS/audio never learn a second CPU exists. Pinned by
`softcard_toggle` incl. a full tickFrame 6502→Z80→6502 round trip.

## 2026-07-12 — Z80 core lands (SoftCard/CP/M Phase 1), zexdoc+zexall 100 %

First deliverable of the Microsoft SoftCard + CP/M plan: a standalone
`pom2::Z80` core (`src/Z80.h/.cpp`), bus-abstracted behind `Z80Bus` so it
links with zero Apple II sources — the SlotPeripheral card and the
dual-CPU arbitration come in Phase 2 (see TODO [Cards]). Full opcode
coverage including the undocumented surface (IXH/IXL, DD CB write-back,
SLL, X/Y flags, MEMPTR/WZ), IM 0/1/2, NMI, EI shadow, documented T-state
totals. Pinned by `z80_core` (committed smoke) + `z80_zexdoc`/`z80_zexall`
(configure-time downloads, SHA-256 pinned, Klaus pattern) — both
exercisers pass 100 %.

The pitfall worth remembering: **zexdoc green ≠ core correct**. zexdoc
masks the undocumented X/Y flags, and the one bug the first full run
surfaced was the `BIT n,r` X/Y rule — X/Y copy bits 3/5 of the *full
tested register*, not of the masked single-bit result (and for
`BIT n,(HL)` they come from WZ's high byte, which is why the core carries
a real MEMPTR). Only zexall's silicon-captured CRCs catch this class;
any future Z80 touch-up must keep both exercisers in the gate.

## 2026-07-12 — OE composite bug hunt: GPU/CPU demod knob parity

Bug hunt on the OpenEmulator composite pipeline, GPU shader vs CPU demod.
The demod *math* checked out OE-exact (kernels recomputed from libemulation's
`chebyshevWindow × lanczosWindow` realIDFT recipe to ≤5e-6; Y'UV matrix and
sin→U / cos→V / PAL-flips-V conventions match `OpenGLCanvas.cpp` line for
line) — every real bug was in *which path applies which knob*:

- **Hue / Sharpness / PAL / Sharp-text were GPU-only.**
  `renderCompositeOeCpu` ignored all four `NtscParams` demod knobs, yet
  MainWindow still *neutralised* hue+sharpness in the CrtEffectStack pass on
  the OE-CPU branch ("the demod already applied them" — only true for the
  GPU). Net effect: the sliders were silently dead in OE-CPU mode, and —
  worse — **popped off on OE-GPU mixed frames**, whose graphics band is CPU-
  demodulated (`mixedCompositeUsesFramebuffer`): entering a splitscreen
  (game score band, BASIC) visibly dropped the user's hue/PAL. Fixed by
  mirroring the live knobs into the display each frame
  (`Apple2Display::setOeDemodParams`) and implementing hue rotation, the
  soft↔sharp chroma-kernel blend and PAL V-sign alternation in the CPU
  demod — same formulas as the GLSL, pinned pixel-identical (maxDelta 0) by
  the extended `oe_demod_gpu_cpu_parity` (now also covers
  hue+sharpness+PAL engaged, and demodulated TEXT).
- **AI `/screen` lied in OE-GPU mode.** `pixels()` returns the LUT fallback
  framebuffer there (the composite image only exists in a GL texture), so
  agent screenshots showed ColorNTSC colours, not what's on screen. New
  `Apple2Display::demodCompositeForCapture()` schedules the pixel-identical
  CPU demod after the server's render; no-op in every other mode.
- **One-frame present race.** `drawScreenImage` re-polled
  `Memory::getDisplayState()` (the CPU worker may have advanced past the
  rendered frame) to route sharp-text/demod presentation; a text↔graphics
  switch in that window flashed one LUT frame. It now reads
  `Apple2Display::lastRenderState()` — the snapshot render() actually used.
- **Golden rebaseline (15 hashes).** `display_golden_hash` was red at HEAD:
  the intentional mono lo-res dot-pattern rendering (previous entry) landed
  without re-recording the `lores/dlgr × mono*` hashes, which still pinned
  the pre-fix "mono lo-res ≡ colour hash" behaviour. Re-recorded; diff
  audited to be exactly those 15 entries.
- **Parity contract documented.** The demod now exists in three deliberate
  copies (GLSL, `renderCompositeOeCpu`, the test's re-simulation); each site
  now carries a cross-pin comment naming the other two, since an edit to one
  alone is invisible to CI until the parity test is also updated.

## 2026-07-12 — paint-editor crumbs: mono lo-res, composite canvas, DHGR sprites

The three leftovers from the 17-item batch. 133 tests (pins folded in).

- **Mono lo-res rendering** (`renderLoRes` / `renderLoResDouble`). On a mono
  monitor a lo-res nibble is NOT a grey: the colour generator keeps cycling
  at 14.318 MHz, so a block displays as its repeating 4-bit dot pattern —
  the same serialisation the composite-signal path already used
  (`fillCompositeSignal`, absolute-sample indexed). GR's 280-wide pixels
  average their two samples (grey 5 → uniform 127); DLGR renders per-dot at
  560 (grey 5 → fine stripes), aux rotation included. Standard
  max(target, prev × decay) phosphor rule; `Phosphor`/`phosphorFor` moved
  above the lo-res painters. Pinned in `dhgr_paint_model`.
- **Composite canvas pipelines.** "AppleWin NTSC (composite)" and "OE
  composite (CPU)" join the paint editor's pipeline combo (scratch display
  in AppleWin *Monitor* sub-mode — Tv's frame blend would smear a static
  canvas). This is what makes the DHGR NTSC-8-px import previewable
  faithfully: its 86 colours only exist after composite demodulation, and
  the import preview now points at the pipeline combo instead of
  apologising. The 560-wide composite output of the 280 modes reuses the
  existing pair-averaging width adapter.
- **Sprite editor DHGR target** (gated on `supportsDhgr()`). The editing
  canvas stays the mono shape; a "DHGR target" toggle re-aims Stamp / Grab /
  colour preview / ca65 export at the DHGR page: lit pixels plot as 140-px
  colour pixels in a picked 16-colour hue (transparent background,
  plane-aware pokes), Grab lifts shape + dominant hue back, and the ASM
  export emits `name_aux` / `name_main` byte-pair tables (blit at an
  aligned byte-column pair). Placement is pixel-granular (`Px X`) — DHGR
  pixels are nibble-aligned, so there is no ×2-style parity honesty problem.

## 2026-07-12 — paint-editor batch: 17 items (tools, DLGR, NTSC 8-px, sprites)

The full improvement backlog proposed after the 560-dot import landed in one
wave. 133 tests (DLGR + NTSC-8 pins folded into the two existing paint tests).
Highlights and the non-obvious bits:

- **GR/DLGR screen holes masked.** Bulk editor ops (clear/import/load) used
  to write the text-page screen holes ($x78-$x7F per 128-byte group) on the
  LIVE machine — peripheral firmware (SmartPort, mouse) scratches there, so
  a Clear Page could corrupt a running driver. Text-page loads now go
  byte-wise through pokes with holes skipped (both planes in DLGR).
- **HGR import now scores LUT row 0.** Closes yesterday's ~22 % divergence:
  the importer's decode is pinned byte-identical to `renderHiRes` ColorNTSC
  in `dhgr_convert`. POM1 parity for this file is deliberately dropped
  (comment documents it).
- **DLGR mode.** 80×48 blocks over the aux+main text pages; the aux nibble
  displays ROTATED LEFT one bit (MAME renderLoResDouble), so the model
  stores rotr4(colour) in aux nibbles — pinned against the real renderer +
  a lo-res palette cross-pin in `dhgr_paint_model`. Editor paints it in a
  560-dot logical space (7 dots per block), pxScreenW()=0.5.
- **DHGR NTSC 8-px import** (`imageToDhgrPage560Ntsc`): scores against the
  86-colour trailing-8-dot palette from ii-pix (`DhgrNtsc8Palette.cpp`,
  BSD-2-Clause data, table anchors pinned). The model is CAUSAL — no right
  context, no guessed neighbour, no refinement pass needed. The extra
  colours only appear on composite viewing targets (OE/AppleWin modes);
  the import preview warns that the MAME-LUT canvas under-sells it.
- **Save to ProDOS**: `buildVolumeFromFolder` now parses CiderPress-style
  `NAME#TTAAAA` filename tags into file_type/aux_type (pinned in
  `prodos_volume_smoke`), and the paint editor's file browser homes to
  `prodos_folder/` — a default "PIC#062000" save is BLOAD-able by name at
  $2000 after the next host-folder mount.
- **Tools**: 16-colour copy/paste (mode-tagged clip + FlipH/FlipV/Rot90),
  MacPaint 8×8 fill patterns (page-anchored so strokes tile seamlessly;
  patterned fills over the same colour are allowed), X/Y mirror symmetry
  (applyPlot level — region ops deliberately exempt), DHGR text, onion-skin
  tracing overlay (fit/crop-aware placement), page 1↔2 flipbook + ghost
  (double-buffer animation authoring), canvas pipeline selector
  (NTSC/Medium/4-bit/Chat Mauve — ChatMauve's 560-wide HGR output is
  pair-averaged down to the 280 canvas), 4:3 aspect, DHGR fringing overlay,
  session persistence, and the POM1 sprite editor (`hgrsprite/`) wired to
  the same host seam.

## 2026-07-12 — DHGR import upgraded to the true 560-dot model

Follow-up to the paint-editor port after comparing against ii-pix and
bmp2dhr: the 140-px block quantiser is bmp2dhr's model, which ii-pix
dropped as "only useful to show why this is not the right approach to
DHGR". 133 tests (one new).

- **`imageToDhgrPage560`** — ii-pix's "4-pixel colour" model with the HGR
  converter's proven architecture: per-byte-column (7 dots, 128
  candidates) branch-and-bound analysis-by-synthesis, warm-started per
  row, in-candidate linear-RGB error walk scored in CAM16-UCS via the
  local Jacobian, then monotone cross-column ICM refinement with dirty
  tracking. Exploits the full 560-dot resolution and pilots the colour
  fringing the block model suffers blindly. ~180 ms/photo (vs 4 ms for
  140-px) — live-slider friendly. Default; the block model stays as the
  "140 px blocks (Dazzle Draw)" combo choice (instant, retouch-friendly).
- **Optimisation target = canvas, bit-exact.** The candidate renderer is a
  copy of POM2's ColorNTSC DHGR decode (Apple2VideoDecode LUT row 0 +
  rotl4b(absX+1)), pinned byte-identical to `renderDhgr` in the new
  `dhgr_convert` test — something ii-pix can't do for a specific emulator
  (it scores against colour-picker-measured palettes). Also pinned: exact
  solid fields with dither off, tone conservation dithered (mean linear
  RGB within 0.06 — full-strength diffusion turns the unavoidable
  black-context edge artifact into a never-decaying ripple, an inherent
  error-diffusion property, so exactness there would be a wrong pin), and
  refinement monotonicity.
- **LUT archaeology.** The ~22 % HGR importer/canvas divergence documented
  yesterday has a root cause: POM1's GraphicsCard NTSC LUT is MAME's
  medium-color **row 1**; POM2's ColorNTSC is **row 0**. HGR keeps row 1
  verbatim (POM1 parity); the DHGR converter carries row 0.
- **Resampler pixel aspect.** `resampleToLinearRgb` gained a
  `pixelAspect` parameter (default 1.0 — every existing caller unchanged);
  the 560-dot path passes 0.5 so fit/letterbox is computed in visual space
  (a square source fills the width instead of coming out 2× too narrow).

## 2026-07-12 — HGR Paint editor ported from POM1 + DHGR mode

The portable `hgrpaint/` module (MacPaint-style editor + ii-pix-style
image importer with CAM16-UCS perceptual dithering) is copied verbatim
from POM1 and driven by a new `Pom2HgrPaintHost` (Tools → HGR Paint
Editor). 132 tests (one new).

- **Offscreen canvas trick.** The host renders the editor page through a
  private, never-clocked IIe `Memory` + `Apple2Display` pair: the scratch's
  cycle counter never advances, so its video-event log never publishes and
  `render()` always takes the fast single-state path — the canvas is
  pixel-identical to the live screen with zero beam-racing interaction.
- **Pokes bypass IIe paging on purpose.** `writeRamUnchecked` (main) / raw
  aux bank (DHGR) instead of `memWrite`, so live 80STORE/RAMWRT state can't
  silently reroute the editor's writes to the wrong plane.
- **DHGR mode (POM2-only module extension).** New DHGR/DHGR2 pages (gated
  on `host->supportsDhgr()`): 140×192 16-colour aligned block model over
  the aux+main pair, 16 KB A2FC load/save, DHGR image import (CAM16-UCS +
  error diffusion at native resolution). The nibble↔colour law
  (`colour = rotl4(nibble,1)`) was derived from MAME's square-filter
  decode and is pinned by `dhgr_paint_model` against the real
  `renderDhgr` in two colour modes + a lo-res palette cross-pin — so an
  Apple2Display palette/decode change that would desync the paint model
  fails CI, not the user's picture.
- **stb gotcha.** MainWindow's `stb_image` impl is `STB_IMAGE_STATIC`
  (TU-local); the host TU compiles the only exported stb_image +
  stb_image_write implementations, which `HgrImageDecode.cpp` links
  against. Don't add a second non-static impl.

## 2026-07-12 — wave 4 (deferred fixes cleared + real Liron implemented)

The four items wave 3 deferred, plus the Liron follow-through now that
the real ROM is public. 131 tests (one new).

- **OE-CPU demod out of `stateMutex`.** The 17-tap × 560×192 FP demod
  (~1-2 ms) ran inside the lock every UI frame, stalling the CPU worker
  (read as emulation/audio jitter, worse under disk-turbo). `render()`
  now defers it (`pendingCpuDemodRows_`) and MainWindow runs
  `finishPendingCpuDemod()` after releasing the lock — it consumes only
  display-owned buffers. The mixed text band is patched under the lock
  (it reads guest RAM) and the per-row FIR only rewrites rows [0,160) in
  mixed mode, so the patch survives; `pixels()` finishes lazily so every
  render→pixels consumer (tests, screenshots) stays correct.
- **Frame-wrap video-event off-by-one.** An instruction straddling the
  17030/20280-cycle video-frame boundary stamped its soft-switch event
  past the boundary but publication ran after the instruction — the event
  closed into frame N and applied the switch one frame early. Stamps are
  non-decreasing, so boundary-crossers form the log's tail: they now
  carry into the new recording frame. Pinned by a real-6502 straddle case
  in `video_event_publish` (STA $C055 started 2 cycles before the
  boundary).
- **SmartPort STATUS pre-flights.** $CnC0 now returns SEC+$28 on no
  media and SEC+$2B on write-protect before handing back the block count
  (TRM driver conventions) — formatters that pre-flight STATUS used to
  get CLC on an empty/WP bay. Exactly 32 bytes, fills $CnC0-$CnDF.
- **Golden table: 112 → 164 pins.** New hash-frozen scenes: FLASH-on
  phase (16 emu-frames parked), text/HGR PAGE2, HGR 80STORE+PAGE2
  (asserted **equal** to the page-1 hash — the Sather 5.10 gate),
  HGR+AN3 rev-0 bit-7 mask (would have caught wave 3's paintHgr bug),
  80COL+HIRES+MIXED without DHGR (upscale path), and the Chat Mauve
  BW560/Mixed/Chunky160/Duochrome sub-modes. All 112 pre-existing hashes
  unchanged. Deliberate gap kept: mousetext/char-ROM glyphs need a user
  ROM; PAL beam-raced splits stay behavioural.
- **Real Liron controller ROM in `roms/liron.rom` + SmartPort-protocol
  dispatch.** The BMOW dump (4 KB, SHA1 fa94ecc2…, per-slot $Cn00 pages
  at slot×256 + $C800 bank at 2048) now ships in roms/; on slot-having
  machines SmartPortCard re-bases its page on the real dump — authentic
  identity $Cn07=$00/$CnFB=$00/$CnFE=$BF/$CnFF=$0A — with the HLE
  entries overlaid (the real IWM/UniDisk code can't run without the
  drive-side 65C02; never loaded on //c-class, see
  project_iic_smartport_boot). And $Cn0D is no longer fail-closed: a
  168-byte 6502 handler in the $C800 bank implements the real SmartPort
  call convention (inline cmd + param-list pointer, RA+3, ZP $42-$45
  saved) against a C++ engine — STATUS incl. unit-0 controller status
  and the 25-byte DIB, READ/WRITE (through the legacy commit machinery),
  FORMAT/CONTROL/INIT, real error codes ($01/$04/$21/$27/$28/$2B/$2D/
  $2F). Pinned by `liron_smartport_dispatch`: the full matrix executed
  by a real 6502, in both synthetic and real-ROM identity passes.

## 2026-07-12 — wave 3 (graphics system + Liron SmartPort audit)

Targeted hunts: display decode / composite pipelines / voxel & Chat Mauve
(three review agents), and the Liron-class SmartPort card audited against
primary sources (Apple Tech Notes, ProDOS 8 TRM, and the REAL Liron
firmware — see below). ASan+UBSan build of the full suite: clean (and it
exposed a real link bug: `test_ai_control_server` was missing
`MouseCardAppleWin.cpp`; Release builds optimized the dynamic_cast typeinfo
away, sanitizer builds didn't).

**Headline discovery: the Liron controller ROM is publicly dumped.**
BMOW/Yellowstone published `LIRONALL.bin` (4 KB) + full disassembly in
2018-2019; MAME's "WANTED — never dumped" listing (which CLAUDE.md echoed
as "cannot be implemented regardless of effort") is stale — MAME just
never ingested it. Verified byte-level: `$Cn07=$00`, `$CnFE=$BF`,
`$CnFF=$0A` → ProDOS entry fixed at `$Cn0A` (independently confirms the
DIX `JSR $C50A` fix), SmartPort dispatch at `$Cn0D`. CLAUDE.md corrected.

Display/composite fixes:
- **FLASH / phosphor / Tv-blur paced by the host monitor (medium).**
  `frameCounter` was ++ per render() call — the UI renders at vsync, so a
  120/144 Hz panel blinked FLASH 2-2.4× too fast, decayed MonoAmber
  afterglow 2.4× faster, and collapsed the AppleWin Tv 50 % blend; even at
  60 Hz the PAL profiles flashed at the NTSC rate. Now derived from the
  emulated frame index (`cycleCounter / 65·scanlines`, standard-aware) —
  exactly MAME's `frame_number() & 0x10`; decay is raised to the
  elapsed-emu-frames power and the Tv stash only advances with the
  machine. `hgr_render_smoke` now pins the invariant both ways (decay on
  frame step, NO decay on same-frame re-render).
- **CRT glass vanished on mixed/sharp-text/fallback frames in OE-GPU mode
  (medium-high).** The effect-stack gate only covered the pure GPU-demod
  and OE-CPU branches; a mixed graphics+text frame (score bands, BASIC)
  presents the CPU-rendered framebuffer and got NO scanlines/mask/
  persistence — effects flickered off/on during gameplay with a stale-
  persistence ghost on re-entry. Gate is now `oeFamily && presentTex ==
  screenTexture` — every OE path that still presents the raw framebuffer
  gets the glass.
- **Composite HGR dropped the IIe rev-0 DHIRES bit-7 mask (low-med).**
  `paintHgr` hardcoded `bit7Mask=0xFF` while the RGBA twin honors
  `state.dhgr ? 0x7F : 0xFF` (MAME `bit7_mask`): with AN3 on + 80COL off,
  the composite modes rendered the half-dot delay the LUT modes correctly
  suppressed — two POM2 outputs disagreed on the same frame.
- **Settings hardening.** NTSC/CRT floats + voxel tunables now clamped to
  slider ranges on load, and all 17 sliders got `AlwaysClamp` (a
  hand-edited `ntsc_center_lighting=0` was `1/x` → `exp(-inf)` → fully
  black screen in every glass mode, surviving restarts).
- **Chat Mauve rewind/snapshot hooks** ('CM' blob: FIFO/mode/AN3/80COL
  latches) — rewinding past a BW560/Mixed switch kept rendering the later
  mode until the guest re-clocked. Panel slot label unhardcoded (any
  slot 1-7 on //e). Voxel shader program no longer leaked at shutdown
  (via new `pom2::deleteShaderProgram`).

Liron/SmartPort fixes (spec citations in the audit, `$` = ProDOS codes):
- **READ/WRITE on an empty bay silently "succeeded" (medium ×2).** A read
  streamed a $FF buffer with CLC — ProDOS ONLINE saw a garbage volume and
  `PR#5` with no media booted 512 bytes of $FF and jumped into them; a
  write dropped 512 bytes. Both now latch the I/O error → carry-set.
- **`iicSmartPortArmed_` serialized (medium)** as a backwards-compatible
  MEX trailer byte — a rewind-ring entry captured after a //c HDV/3.5"
  boot restored with the $C500 stub swapped back to real //c firmware
  under a live ProDOS (next MLI call executed unrelated ROM bytes).
- **SmartPortCard transfer state serialized (medium-low)** ('SP' blob:
  unit/block/stream offset/half-filled write buffer/error latches) — a
  rewind landing mid-512-byte stream desynced the transfer.
- **//c arming scoped + fallback leak (low-med).** `bootFromSlot` armed
  the on-board SmartPort for ANY slot (a slot-6 5.25" Library boot with
  SmartPort media re-created the dual-device "garbled banner" scenario)
  and the no-signature fallback returned without disarming.
- **Capability byte honest:** `$CnFE` $13→$17 (write bit was missing —
  capability-inspecting utilities saw a read-only device). **`$Cn0D`
  fails closed** (SmartPort-convention callers used to fall through NOP
  padding INTO the boot routine). **Bad-command error** now $27 (real
  driver code) instead of the invented $01 Filer surfaced on FORMAT.
- **Host read-only images mount write-protected** (Block512Backing +
  Disk35Image probe writability at load) — write-back on a chmod-read-only
  .hdv used to accept a whole session of writes and lose them at flush.

Verified clean (highlights): text/HGR/DHGR interleave + `frameCycleToPos`
beam math both standards (13/13 display tests), GL lifecycle/resize/
GLES-WASM in both composite paths, Mat4/camera (pinned by `voxel3d_math`),
Chat Mauve FIFO edge model, SmartPort dispatch offsets against the real
ROM's conventions, `$Cn0A` DIX path. Deferred with TODO entries: OE-CPU
demod under `stateMutex` (~1-2 ms/frame of worker stall), frame-wrap
video-event off-by-one (≤7 cycles exposure), golden coverage gaps,
SmartPort STATUS pre-flight semantics.

## 2026-07-12 — wave 2 (seams: stale rewind ring, snapshot-load audio, kiosk K leak)

Adversarial re-review of wave 1's fixes + a sweep of the cross-subsystem
seams (snapshot/rewind × profiles, AI server × kiosk, CLI boot × slot
config). Wave 1's fixes all held; the seams gave up two mediums.

- **Stale rewind ring across profile switch / bootFromSlot (medium).**
  `RewindBuffer.h` documents "drop every retained frame on cold boot /
  profile switch", but the ONLY clear site was `coldBoot()`. `applyProfile`
  and `bootFromSlot` wipe RAM/aux (and applyProfile rebuilds the card set)
  without clearing — so with rewind enabled, F6 after a II+→//e switch
  restored II+ RAM/CPU/slot state onto the //e ROM (PC into II+ Applesoft →
  crash), and `truncateAfter` made that garbage the new timeline. Now
  cleared in `bootFromSlot`, `applyProfile` (after step-1 stop) and
  `restartEmulationFromSettings` (its SLOTn sections describe the card set
  being torn down).
- **Snapshot load left the speaker dead for minutes (medium).**
  `SpeakerDevice`'s reconstruction cursor only snaps FORWARD and purges
  older-stamped toggles as stale; rewind/scrub knew this
  (`flushAudioForRewind`) but `/snapshot/load` (AI server) and CLI
  `--snapshot-load` didn't — loading a snapshot with a smaller
  cycleCounter muted audio until the counter re-passed its pre-load value
  (10 min of play ≈ 10 emulated minutes of silence). Both callers now
  flush the speaker + drop the rewind ring (whose stamps would break
  `indexForCycle` monotonicity) after a successful restore.
- **3.5" boot without 3.5" hardware failed silently (low).**
  `insertAndBootImage`'s Sony35 branch fell through to
  `controller->mount35()` — the //c+-only on-board Sony hub — on ANY
  machine without a SmartPort card, then cold-booted to the BASIC prompt
  with `true` returned and "booted disk" logged. Now errors out ("no 3.5"
  device in this config") unless a SmartPort card is present or the
  profile is the //c+.
- **Kiosk K-key leak, open direction (minor, wave-1 fix was one-sided).**
  Closing the Keys band with K was fixed, but OPENING it still typed a
  live 'k' into the running game: the GLFW char callback fires while the
  menu is still closed, then the same frame's `updateKioskMenu` opens the
  non-pausing band. K (and Ctrl-K's $0B) is now reserved in kiosk mode.
- **Swallow latch scoped to gamepad-mapped pads.** Raw pads can't drive
  the menu (nav needs a mapping), so their held fire button across a
  keyboard-driven menu close is legitimate game input — no longer eaten.
- **Adversarially re-verified, no change needed:** onKey/onChar gate (F10
  via ImGui, AI keys via `pasteText` direct, Alt tracking above the gate),
  swallow drain on unplug/Emscripten, F6 re-park (worst case one worker
  tick behind the overlay), cassette clock domain split, stickToPaddles
  math (new tests fail on the old code: 143 vs ≤132, 134 vs 128). Owned
  trade-offs: ~13-count axis-snap detent at the cone boundary (no
  hysteresis until someone reports flicker), F6 inert during the Keys
  band, stick-nav edges without the history guard (can only move a cursor
  one step).
- **Release audit.** Docs drift fixed (README documents the kiosk in-game
  menu and drops "no menu to quit"; DEV.md §kiosk rewritten against
  `openKioskStartMenu`/`updateKioskMenu`/`renderKioskMenu` + §joystick
  against the stickToPaddles pipeline and GamepadPlay mapping; CLAUDE/TODO
  note speaker+cassette ARE retimed). CI now triggers on `v*` tags — tag
  builds previously ran NO CI. ⚠ **Open, needs a decision: copyrighted
  Apple ROM dumps are tracked in the public repo since the initial commit**
  (49 files under roms/), contradicting README/CI/packaging claims;
  cutting a public release re-publishes them. Full remedy = untrack +
  history scrub (filter-repo) + force-push.

## 2026-07-12 (pre-release bug hunt: kiosk input leaks, PAL cassette clock)

Adversarial review of everything landed since v0.7 (kiosk menu, square-gate
joystick, PAL speaker fix). Two real input-isolation leaks in the kiosk menu,
one sibling of the PAL speaker bug, and a fistful of minors.

- **Kiosk menu keyboard fallbacks leaked into the machine (major).** The
  menu's arrows/Enter/Esc/K are polled via `ImGui::IsKeyPressed`, but the
  overlay window never sets `WantCaptureKeyboard`, so `glfw_key_callback`
  kept forwarding every key to `MainWindow::onKey`/`onChar` → the $C000
  latch. Concretely: Enter on the key band sent the selected cell **and**
  injected $0D (every send double-typed); Esc closed the menu and delivered
  a stray $1B to the game on resume; K typed a literal 'k' into the running
  title. Fix: `onKey`/`onChar` early-return while `kioskMenuOpen_` (menu
  navigation itself is unaffected — it never used the callback path).
- **Gamepad close-press leaked PB0/PB1 into the game (major).**
  `pollJoystickAndPushToMemory` runs *before* `updateKioskMenu` and gates
  suppression on `kioskMenuOpen_`, which lags a close by one frame — and
  Circle/Cross double as the menu's B/A **and** the Apple game-port
  buttons. Dismissing the menu with B therefore fired PB0 in the game for
  several frames (ditto Cross→PB1 after Restart, and a D-pad direction held
  at close fired an arrow key). Fix: latch `kioskSwallowPad_` on the
  open→closed edge and keep faces + D-pad suppressed until the pad is fully
  released; analog paddles stay live (no edge to leak). The auto-repeat
  history (`padArrowHeld_`) resets while suppressed so a held direction
  re-arms cleanly.
- **F6 rewind unpaused the machine behind the open menu (minor).** A hold
  released while the Start menu had the worker parked ended in
  `rewindEndAndResume` → `Mode::Running`, and `kioskSetPaused` early-outs
  (it still believed "paused"), so the game ran with audio under the
  overlay until reopen. Fix: F6 is inert while the menu is open **and**
  `updateKioskMenu` re-parks the worker if anything resumed it behind a
  wanted pause.
- **PAL cassette pulse audio — same bug the speaker fix cured (minor).**
  `CassetteDevice::queueAudioSegment` converted cycle durations with the
  hardcoded NTSC `kRealtimeAudioTimebaseHz`; under PAL the queue fills
  ~0.7 % slower than the callback drains (~330 samples/s short at 48 kHz)
  → periodic level dips on sustained tones. Fix mirrors the speaker:
  atomic `realtimeTimebaseHz_` + `setCpuClock()` wired from
  `setVideoStandard`. The tape-**file** timebase stays NTSC-nominal on
  purpose — it's the format's cycle definition, not playback pacing.
- **Kiosk minors.** GAMES list now rescans on the RomDirs→List transition
  (a folder added via the browser used to stay invisible until the menu was
  reopened); the mounted-disk ● marker matches canonically (a kiosk
  launched with a *relative* path, `POM2 games/foo.dsk`, never matched the
  canonicalized scan entries — cursor landed on index 0, no ●).
- **Paddle deadzone: continuous engage + axis-snap (follow-up).** Two
  behavioral gaps left by the per-axis → radial deadzone switch: (1) the
  hard cutoff stepped ~12 counts the instant the stick left the dead zone —
  now the radial deadzone **rescales** ([dz..1] → [0..1] along the ray), so
  the reading is continuous (128 → 130 across the edge, pinned); (2) radial
  lost the old per-axis suppression of cross-axis drift — 5 % Y wobble
  during a full X push read PDL(1)≈134 and crept games. Now an
  **axis-snap notch** zeroes the small axis while it's under `dz × |big|`;
  the threshold scales with the dominant component, so diagonals are never
  notched and the square-gate corner guarantee (full diagonal → 255/255)
  survives — both pinned. The whole pipeline (invert → deadzone → notch →
  gate → 0..255) is now a pure static `stickToPaddles()` that
  `paddleValue()` routes through, closing the review's "composition not
  unit-testable" gap: `joystick_square_gate_test` pins center/NaN/corners/
  rails, the deadzone edge, drift suppression, the gate-off path and invert.
- **Joystick minors.** `edge()` now requires prior-poll history — the first
  poll after a (re)bind treated an already-held button as a fresh press
  (Start held across a rebind popped the kiosk menu). Explicit
  `#include <algorithm>` (was compiling through transitive includes).
  Removed dead `activeMouseSlot` (last -Wunused-variable in the GUI build).

## 2026-07-11 (kiosk follow-ups: PAL speaker clock, HDV/3.5 in menu, DS4 in-game mapping)

Fixes + refinements on top of the same-day in-game menu.

- **PAL speaker audio fix (real bug).** The 1-bit speaker's cycle→sample
  reconstruction hard-coded the NTSC CPU clock (`SpeakerDevice::kCpuClockHz =
  POM2_CPU_CLOCK_HZ`, 1.0227 MHz) and `setVideoStandard()` never retuned it. On
  a PAL profile (CPU actually 1.0156 MHz) the audio path consumed ~0.7 % more
  cycles/sec of toggles than the CPU produced, starving the reconstructor →
  periodic snap-forward glitches on continuous speaker music. **H.E.R.O. on the
  //e-PAL profile sounded broken; the same disk under `--preset iie` (NTSC) was
  clean** — which pinned it. Fix: `kCpuClockHz` → runtime `cpuClockHz_`
  (`std::atomic<double>`) + `SpeakerDevice::setCpuClock()`, called from
  `EmulationController::setVideoStandard()` with the standard's real clock
  (`pom2VideoTiming(s).cpuClockHz`). NTSC numbers unchanged → zero regression.
  (AY/SSI263 device clocks stay NTSC-nominal by design.) Confirmed by ear.
- **Kiosk pause no longer swallows audio on resume.** While the menu parked the
  worker (`Mode::Stopped`), the audio thread kept advancing the speaker cursor
  over silence, leaving it far ahead of the frozen production; on resume the
  catch-up purge would eat the game's first sounds for ~the pause duration.
  `kioskSetPaused(false)` now `speaker().reset()`s on the paused→running edge.
- **Mounted-disk marker.** The games list prefixes the disk currently in the
  boot drive with a ● (`ICON_FA_COMPACT_DISC`) and lands the cursor on it.
- **HDV + 3.5" reachable from the menu.** The scan no longer filters to 5.25"
  only — it accepts everything `classifyDiskForSlot` recognises (5.25"/3.5"/
  HDV). 5.25" still hot-swaps in place (flip-disk); 3.5"/HDV route through
  `insertAndBootImage` (the CLI launcher's path) and boot immediately. The ROM-
  folders browser reaches `hdv/` and `disks_3.5/` (add once, persisted).
- **DS4 in-game mapping.** New `JoystickInput::GamepadPlay` (from the standard
  gamepad layout, `valid` only when gamepad-mapped): the analog stick stays the
  Apple II paddles; **Cross/Circle → PB0/PB1**; the **D-pad → Apple II arrow
  keys** (←$08 →$15 ↑$0B ↓$0A) with //e-style auto-repeat (350 ms delay →
  ~16/s); **Square → SPACE, Triangle → RETURN** (one key per press, via
  `Memory::queueKey`). Menu-gated (no injection while the overlay is up) and
  only for gamepad-mapped pads — a raw pad keeps the legacy buttons 0/1/2 →
  PB0/1/2 fallback. The button D-pad is used here, NOT the stick (the stick is
  the joystick), so `GamepadPlay` never folds stickX/Y in the way `UiNav` does.

## 2026-07-11 (kiosk in-game menu — two-zone method)

Reworked the kiosk disk picker into a full two-zone in-game menu. Still
**exclusive to `--kiosk`** — the whole thing lives behind the `if (kiosk_)`
gate.

- **Two entry points.** **START** (pad, standard GLFW mapping) / **F10** opens
  the two-zone **Start menu**; **SELECT** (Back button) / **K** opens the
  **Keyboard band** directly, even mid-game.
- **Two-zone Start menu** — the headline UX win. A **GAMES** list and an
  **ACTIONS** column (Restart / Keyboard / ROM folders / Quit) coexist:
  **◀▶ swaps focus** between the two zones, up/down moves within the focused
  zone, **A** validates it. No more scrolling past every disk to reach an
  action. The focused zone is vivid with a green ▶; the other is dimmed.
- **Machine paused** (`Mode::Stopped`) on every Start-menu page — like
  inserting a disk on a real machine at rest — but the **Keyboard band leaves
  the game running** so injected keys land live. `kioskSetPaused()` only
  resumes a worker it parked itself, so it never fights F6-rewind's mode moves.
- **Keyboard band** = a 2D grid of Apple II keys; **A** sends one via
  `Memory::queueKey`. The Apple keyboard is a **latch+strobe**, so a one-shot
  queue with no key-up bookkeeping is correct.
- **Temporal auto-repeat** (400 ms delay → 150 ms cadence, clock-based off
  `ImGui::GetTime()`) for held directions, plus **L1/R1** fast page jump (±10)
  in the games list — needed because the paused menu loop runs unthrottled and
  would otherwise scroll unaimably fast.
- **Proximity SORT, not filter.** The old build *hid* every non-sibling disk;
  now all mountable 5.25" images are shown, with the mounted title's other
  sides sorted to the top (selection anchored on the mounted disk).
- **ROM-folders manager + gamepad directory browser** (`◀▶`/shortcuts to `/`,
  Home, removable mounts). Extra scan folders persist in a sibling
  **`kiosk_romdirs.txt`** — deliberately *outside* `state.cfg` so the kiosk's
  read-only main config is never written (keeps POM2's strict kiosk contract).
- Plumbing: `JoystickInput::UiNav` gained `left/right/select/pageUp/pageDown`
  edges + raw `*Held` levels (the latter feed the temporal repeat).

## 2026-07-10 (kiosk gamepad UX + Apple II square-gate joystick)

Kiosk mode made lean-back / controller-friendly, plus a faithful joystick fix.

- **Square-gate joystick** (`JoystickInput::applySquareGate`, default on, key
  `joystick_square_gate`). Modern analog sticks ride a *round* gate, so a full
  diagonal only reaches ~217/217 and the extreme corners are physically
  unreachable — but the original Apple II stick rode a *square* gate where full
  X **and** full Y at once (255/255) were reachable, which **Wings of Fury's
  take-off requires**. `paddleValue()` now processes the X/Y pair together
  (radial deadzone, not per-axis — a per-axis one notched the diagonals) and
  expands the inscribed circle to the full square (`s = mag / max(|x|,|y|)`),
  leaving pure-axis directions untouched. Toggle in the Joystick panel; pinned
  by the new `joystick_square_gate` test (129 → 130 ctests).
- **Kiosk gamepad disk selector.** In `--kiosk`, the pad's **Start** (standard
  GLFW gamepad mapping) — or **F10** when the pad has no SDL mapping — opens an
  on-screen picker of the 5.25" images sitting **next to** the booted disk,
  filtered by **name proximity** (longest common prefix) so only the same
  title's other sides/disks appear (Wings of Fury Side A ↔ Side B), not the
  whole 700-disk folder. D-pad/stick move, **A** mounts in-place (no reboot,
  the flip-disk gesture), and trailing **Reset** (reboot on the mounted disk)
  and **Quit** action rows finish the job — all without a keyboard.
- Pitfalls captured: the overlay first rendered *behind* the opaque
  full-viewport kiosk window (fixed by dropping `NoBringToFrontOnFocus` +
  `SetNextWindowFocus`), and the file list stayed tiny under
  `SetWindowFontScale` because a `BeginChild` is a separate ImGui window with
  its own scale (re-applied inside). The Start button silently did nothing on an
  unmapped pad — hence the F10 fallback + a one-shot `gamepad-mapped=yes/no`
  diagnostic log.

## 2026-07-09 (v0.7 — packaging, CI & desktop integration)

First tagged release. Focus on shipping, not the core emulator.

- **GitHub Actions CI** (`.github/workflows/ci.yml`): a `linux` job builds the
  full tree and runs the ~130-test ctest suite (Klaus 6502/65C02, Tom Harte,
  cpu_cycle_count, golden-hash display, boot traces) as the non-regression gate,
  plus a `wasm` Emscripten verification build. The suite was dormant — nothing
  ran it automatically before.
- **Fixed a latent packaging blocker**: `packaging/roms_README.txt` was
  referenced by `install(FILES …)` but never existed, so *every*
  `cmake --install` / `cpack` / `build_dist.sh` staging aborted. Created it (the
  ROM drop-here note). Linux install now succeeds end-to-end.
- **Desktop integration**: an application icon (`packaging/POM2.svg` +
  rasterised hicolor PNGs), a MIME type `application/x-apple2-disk`
  (`.dsk/.do/.po/.nib/.woz/.d13/.hdv/.2mg`) so a double-clicked disk opens in
  POM2, and Debian maintainer scripts that refresh the mime/desktop/icon caches.
- Metadata fixes: real homepage URL in the package (`github.com/habib256/POM2`)
  and a `.desktop` keyword typo.
- **Grappler+**: bundled the 4 KB Orange Micro Grappler+ EPROM dump so the card
  exposes its full firmware (graphics-dump entry points + ROM fingerprint)
  instead of the fallback stub.

## 2026-06-16 (Tom Harte 65x02 ProcessorTests — cycle-exact validation)

Added the Tom Harte [`SingleStepTests/65x02`](https://github.com/SingleStepTests/65x02)
suite — 10,000 random vectors per opcode pinning the full state (registers +
memory) **and** the cycle count. Data-driven harness `tomharte_cpu_test
<nmos|cmos> <dir>` (in-house JSON scanner, no vendored lib), curated +
SHA-pinned ctest gate (`tomharte_6502` / `tomharte_65c02`), download behind
`-DPOM2_FETCH_TOMHARTE=ON` (full corpus ~1.4 GB/CPU → `tests/fetch_tomharte.sh`
for exhaustive runs of all 256 opcodes).

Results: **NMOS 6502 = 100%** on 41 opcodes (410,000 vectors, decimal
included); **WDC 65C02 = 100%** everywhere except decimal SBC on **invalid**
BCD digits.

The suite flushed out **4 genuine decimal bugs** in `M6502::ADC/SBC`, all
**provably identical for valid BCD** (only invalid digits — never produced by
correct software — change), hence zero regression risk (`cpu_cycle_count_test`
+ Klaus stay green):

- **ADC low nibble**: `tmp+6` overflowed onto bit 5 when the low-nibble sum
  reached `$1A-$1F` (invalid BCD digit) → `accumulator & 0xF0` injected `$20`
  instead of the single `$10` carry, inflating the result by `$10`.
  The silicon re-packs: `((tmp+6)&0x0F)+0x10`.
- **ADC decimal carry**: tested `tmp & 0x100`, but the `+$60` high-nibble
  correction can push an invalid-BCD sum up to bit 9 (`$240`, bit 8 = 0) →
  lost carry. Fix `tmp >= 0x100` (identical for valid BCD ≤ `$190`).
  Pitfall: an `int tmp` made it look like `& 0x100` sufficed — it's the carry
  rising to bit 9 that betrays it, visible only via instrumentation.
- **ADC V (CMOS)**: was forced to the binary overflow; the WDC uses the
  "high-nibble-sum" V already computed at line ~421. Removed the overwrite.
- **SBC low nibble**: `tmp-6` left bit 4 (the borrow the high nibble reads via
  `accumulator & 0x10`) un-re-packed → lost borrow, result +`$10`.
  Fix `((tmp-6)&0x0F)-0x10`.

Documented pitfall (not fixed): the WDC 65C02 decimal SBC on **invalid** BCD
follows a silicon correction *distinct* from the NMOS (officially undefined,
data-dependent ±1 errors) that we don't model — `e9` diverges at ~3.4% on
`wdc65c02/v1`, the NMOS being exact (`6502/v1/e9` = 100%). Opcode excluded from
the CMOS gate, tracked in `tests/tomharte_wdc65c02.manifest`.

Architecture note: instruction-stepped core + non-virtual `Memory::memRead/Write`
→ we validate the final state + the cycle count (not the per-cycle bus order),
which covers exactly the class of timing bugs (cf. the historical RMW
under-count of `cpu_cycle_count_test`).

## 2026-06-12 (wave 4: remaining peripherals, UI, snapshot — MAME oracle)

Four read-only hunters over the never-audited areas (Grappler / LLE+hand
mouse, UI layer, //c-IWM-Sony stack, snapshot/rewind/cassette), then verified
fixes + pins. Suite: 127/127.

- **Grappler+: inverted register decode.** The real data port is
  `!(offset & 3)` ($C0n0/4/8/C) — POM2 spooled offset 1, which is the
  **ROM bank select** on the real card: the authentic 4 KB firmware printed
  into the void, and its status poll read $FF = "busy + out of paper"
  (the worst possible value). Rewritten per MAME grappler.cpp:
  IRQ|DIP|BUSY|PE|SELECT|ACK status, $C800 banks (A0 set / read $CnXX
  reset + A6 ACK-detection trick), A1/A2 IRQ on the bus. The ROM stub also
  wrote via offset 1 — hence green tests that pinned the stub, never the real
  ROM path (pitfall: the test validated the implementation against itself).
- **Sony 3.5": register table aligned with MAME `mac_floppy_device`.**
  Address bit 3 = HEAD-SELECT line (ssW), not the IWM drive-select;
  MotorOff is strobe 0x6 (0x3 = EjectOff, no-op — the old "boot-tuned" table
  put motor-off there: a conformant firmware killed the motor thinking it was
  cancelling an eject); the disk-change latch lives in sense 0x3 and clears
  via the DskchgClear STROBE (0xC), not on read; DIRTN polarity fixed; a
  write-protect sense (0x9) finally exists — a protected 3.5" image was
  invisible to the firmware (writes silently lost). IWM motor-off delay:
  8388608 ticks of the 7 MHz clock ≈ 1.17 s (was 1 s CPU, with a false
  comment about the IWM clock).
- **UI: Floppy Emu panel's `insertDisk` without `stateMutex`** while the
  worker streams nibbles — potential corruption/UAF on click; all neighboring
  paths locked. Slot Config state reads moved to snapshot-under-lock;
  Disk II `motorOn` made atomic (read by the UI-side auto-turbo); per-cell
  PushID in the memory viewer (hundreds of "00" cells shared the same
  ImGui ID).
- **Snapshot: cards that gained state didn't participate.**
  Grappler+ (banks/ACK/IRQ) and EchoPlus (full SSI263 — the Mockingboard
  SoundII captured the same chip from the start) now have their append/load
  hooks + round-trip pins. CLI `--snapshot-save/load` were silent **no-ops**
  documented as functional — wired onto the same mechanics as the AI server.
  Doc: the "CASS" section never existed.
- **No-Slot Clock: write cycles wired** (AppleWin parity — the DS1216E key
  bit travels on A0 of the ADDRESS, R/W indifferent: drivers feeding the key
  with STA never unlocked the clock).
- **68705: level-sensitive timer IRQ** (vector pull no longer clears the
  request while TIR=1/TIM=0 — MAME parity). **LLE mouse**: no more cursor
  jump after reset (delta counters re-primed from the host). Size caps on
  the .wav/.aci files (pre-slurp), joystick NaN guard, `POM2_AUTO_*` env
  timers fixed and cancellable.

## 2026-06-12 (bug hunt: full audit validated against the MAME oracle)

Systematic audit of the subsystems (CPU/memory, video, audio, storage,
threading/cards), each fix validated against the MAME sources (file+line
citations in comments) and pinned by a test. Suite: 127/127.

- **LSS disk writes angularly mispositioned** (the worst — silent corruption
  in the default config). `DiskImage::writeFlux` reduced the splice window
  with a raw `startLssCycle % period`, while the read (`getNextTransition`)
  is anchored on `revolutionStart` (port of MAME `find_position`). The anchor
  being arbitrary (2×cpuCycleTotal at motor-on), every bit-level write landed
  `revStart mod period` cells away from where the controller had just read the
  address — the data field overwrote another area of the track. **Pitfall #2
  on the same path**: the cell→nibble re-pack assumed 8 cells/byte, but the
  `expandTrackBits` timeline adds +2 cells of padding per sync $FF — drift of
  ~4.75 nibbles/sector on a standard .dsk. Why no test caught it:
  `diskii_lss_smoke_test::testLssWrite` **explicitly skipped the positional
  assertion**. `writeFlux` now takes the anchor (same convention as the read),
  the re-pack walks the padded timeline, and the positional pin is active
  (`disk_writeflux_anchor` + strengthened LSS test).
- **DHGR: hues rotated 90° in the composite OE demods (CPU+GPU).**
  The subcarrier phase shift was applied **twice** (sin/cos table construction
  with `(k+po)&3` AND indexing with `(xi+po)&3`). The GLSL shader comment
  documented the wrong conclusion: the old GPU formula (single application)
  was the right one, it "diverged" because the CPU was wrong. HGR (po=0) was
  unaffected — hence an "excellent" calibration that masked the bug. Pitfall:
  `dhgr_phase_signal_test` pinned the bug **tautologically** (its anchor
  replicated the buggy formula) — the test now derives its expectation from
  the independent MAME LUT path.
- **DLGR: nibble pattern restarted per 7-dot half-cell** instead of the
  absolute 14.318 MHz phase (`paintLoRes40` already did it right) — colors
  alternating per column. Pin: exact samples in absolute phase (the naive
  `sig[i]!=sig[7+i]` test is invalid: at rotl4(1)=2 from x=0 and main 1 from
  x=7 ≡ 3 (mod 4) yield the **same sequence** at different phases).
- **Sound II silent**: the SSI263 emulation (registers+IRQ) was complete but
  `fillAudioBuffer` never mixed `ssi_->fillAudio()` (only EchoPlusCard did).
  **VIA 6522**: the ORA access (reg 1) didn't clear IFR.CA1 (MAME
  `CLR_PA_INT()`) → speech IRQ stuck for drivers using the standard idiom;
  first T1 strike at N+1 instead of N+3 (the +2 bias already existed for T2,
  same DIX rationale). **Native Phasor**: MAME VIA decode
  (`$Cs10`→VIA1, `$Cs80`→VIA2, `$Cs90`→broadcast both, nothing at `$Cs00`).
  **AY**: envelope period 0 = double speed (MAME doesn't clamp to 1).
- **SSC/telnet**: `send()` without `MSG_NOSIGNAL` → a peer that disconnects
  rudely **killed the process** (SIGPIPE); EAGAIN treated as fatal + partial
  sends silently lost (breaks ADTPro/XMODEM); blocking `accept()` not woken by
  `shutdown()` on macOS/BSD (the same bug already documented+fixed in
  AiControlServer — ported the `poll()` pattern).
- **Slot Config "Apply" overwrote the slot config on //c** — exactly the bug
  fixed at release on 2026-06-10, but the Apply path lacked the `builtInSlots`
  guard. **`applyProfile` without lock**: `stop()` didn't wait for the worker
  to park and the frame loop didn't re-check `mode` → ROM/SlotBus/disks
  rebuilt while a turbo frame was still running (UB). The worker re-checks
  between 4096-cycle chunks and the switch waits for `workerParked_`.
  CLI `--speed` clamped to 2 M like the AI server.
- **2IMG: lock bit = bit 31** (spec/CiderPress/AppleWin), not bit 0 — locked
  images were writable; the DOS volume read 0 instead of 254 on a locked dump
  without bit 8. The test pinned the wrong interpretation (written from the
  code, not from the spec — classic pitfall).
- **NMOS CPU: undocumented multi-byte opcodes** ($x3, $4B/$6B/$8B/$AB/$CB,
  $1B..$FB) dispatched as 1-byte NOP → instruction-stream desync (the exact
  class already fixed for $0B/$2B/$EB); $CB/$DB were remapped
  "undefined WAI/STP on NMOS" while NMOS puts SBX #imm (2 bytes) and DCP
  abs,Y (3 bytes) there. 1-byte NOP: 1 cycle on 65C02 (fetch only, MAME
  ow65c02) vs 2 on NMOS; WAI/STP 3 cycles. NMOS decimal SBC: deterministic V
  (binary difference, MAME `do_sbc_d`) — decimal ADC already did it, V stayed
  stale on the SBC side.
- **IIe/II+ memory**: $C010-$C01F is the keyboard strobe mirror on II/II+
  (MAME `.mirror(0xf)`, read OR write — `STA $C01x` never cleared it);
  on IIe any $C01x write clears it (reads $C011-$C01F stay status-only).
  **$FE sentinel**: `iieReadStatus` returned $FE for "not a status" — but
  `0x80|transchar($7E '~')` == $FE is a legitimate read → RDRAMRD polls sent
  to the floating bus (OFF read while ON). Out-of-band signal now. **INTC8ROM**:
  arms on any $C3xx access with SLOTC3ROM=off **including under INTCXROM=on**
  (UTAIIe 5-28) and on the write path; a $C3xx write no longer steals the
  $C800 window from the legitimate card. **Video events in VBL**: stamped at
  line 192 ("end of frame") instead of being clamped to 191 — a mode switch
  thrown in VBL (the canonical anti-tearing practice) no longer paints a
  spurious split on the last visible line.
- **Misc validated**: HDV STATUS returns the block count in X/Y (the BITSY
  crash already fixed on the SmartPortCard side); a 32 MiB volume of exactly
  65,536 blocks clamped to $FFFF (read 0 before); `writeBackEnabled`
  propagated to images on snapshot restore; ClockCard no longer loses the time
  on Ctrl-Reset (battery-backed uPD1990AC); the AI server's `/mouse`
  recognizes the AppleWin HLE mouse (//c default); mouse VBL in profile cycles
  (PAL 20313); PAL clock: provenance honestly documented (locked at
  line 15625×65 by design; MAME = 1,016,966 — assumed 0.13% gap, same class
  as the "device clocks stay NTSC" approximation).

## 2026-06-10 (//c: NMOS CPU freeze, rear Chat Mauve, slot config preserved)

- **"POM2 crashes when I select the Apple //c (1984) profile"** — it was a
  **CPU freeze**, not a segfault. Diagnosis: the user's config had
  `cpu_mode_override=nmos` (sticky setting, set once on a II+).
  `resolveCpuMode` therefore returned **always NMOS**, including for the //c —
  which has a **soldered 65C02**. The //c ROM runs 65C02 opcodes
  (`LDA (zp)`=$B2…) that **decode as KIL on NMOS** (`M6502::Hang` = `PC--` →
  infinite loop) → frozen CPU → dead screen = "crashed". (Not reproducible
  headless because the symptom is frozen emulation, not a process crash;
  isolated by analyzing `M6502.cpp` + reproducing the mid-frame switch.)
  **Fix**: `resolveCpuMode` honors an **NMOS** override only if the profile is
  NMOS by default (II/II+///e-unenh); the **65C02-only** machines (//c, //c+,
  //e enhanced, PAL variants) always run in CMOS. The Machine→CPU menu
  **greys out** "NMOS 6502" on these profiles. Also answers "the CPU should
  switch NMOS↔65C02 per the //e/enhanced profile". Verified: //c resolves
  `CPU = 65C02` despite the override.
- **Le Chat Mauve on //c (rear connector).** The //c took the
  **"IIc Adapter"** Le Chat Mauve on its DB-15 video port (cf.
  fenarinarsa.com/?p=1370 + CLAUDE.md § profiles). POM2 ignored any card on a
  `noPhysicalSlots` profile. **Fix**: exception for `chatmauve` — the RGB card
  plugs into the //c-class machines (it's a video adapter, not a peripheral
  slot card). The Slot Config panel offers a combo **{(empty), Le Chat Mauve
  RGB (rear connector)}** on //c/+ (nothing else is pluggable; the duplicate
  check limits it to one adapter).
  **The "Apple //c PAL (Le Chat Mauve)" profile now wires the card in hard**
  (built-in **sl7** = "IIc Adapter") — it carried the name without plugging in
  the card. On this profile the other slots' combo is greyed out (a single
  adapter), and a redundant user `chatmauve` elsewhere is ignored (no double
  card).
- **Slot config overwritten on exit on //c.** `persistSettings` saved the
  **live** `slotCards` mapping, so quitting on //c wrote the forced built-ins
  (`mouseaw`…) over the user choice (`slot_4_card=mockingboard` lost on return
  to //e). **Fix**: don't persist profile-forced slots (built-ins + slots
  cleared by `noPhysicalSlots`, except the user-controllable Chat Mauve) — the
  user setting stays intact. Same class as
  [[pom2-cffa-profile-switch-drop]]. Verified: `slot_4_card=mockingboard`
  preserved after a clean exit on //c.

## 2026-06-10 (DROL cut-scene: $C050-$C057 reads → floating bus)

- **DROL cut-scene hang.** Disk image scan: the cut-scene overlays
  (offsets 0x14359/0x143d5/0x14be0 of `Drol.dsk`) sync via
  `LDX #$02 / LDA $C050 / CMP #$80 / BNE / DEX / BPL` — three consecutive
  reads of the **video scanner** through a display soft-switch. POM2 returned
  a hard 0 on `$C050-$C057` reads → infinite loop (LinApple's historical hang;
  AppleWin fixed it in 1.13.0 by implementing the floating bus). **Fix**: a
  `$C050-$C057` read toggles the mode AND returns `floatingBus()` (MAME
  `apple2.cpp do_io` does the same); likewise for the speaker `$C030-$C03F`
  (latch undriven). **Pitfall avoided**: the block held `stateMutex` and
  `floatingBus()` re-takes it — work scoped before the return. Pinned by
  section (d) of `vapor_lock` (the game's exact loop locks on an HGR page
  filled with $80, and the TEXT-off side effect is preserved).

## 2026-06-10 (DROL: double-buffer flips vs beam-racing; Chat Mauve: AppleWin decode)

- **DROL flicker (unsynchronized page flips)** — all display modes, NTSC as
  well as PAL. Diagnosed by probing the real disk (`tests/drol_probe.cpp`,
  WOZ): DROL flips `$C054/$C055` every ~4 frames at **drifting** positions
  (23/31 flips in the visible zone) — free double-buffering, NOT beam-racing
  (its flipper is self-modifying at `$6138`; DROL's floating bus only serves
  the cut-scene, cf. AppleWin 1.13.0 "fixed the hang at Drol's cut-scene").
  **Why it flickered**: the beam-raced replay paints the band above the flip
  from the page the game is **already redrawing** — POM2 reads RAM at render
  time, not at beam passage → half-erased sprites. (The real beam read the
  still-intact page; before per-frame publishing, these events were often lost
  → accidentally "clean" full-page render.) **Fix**: in
  `forEachBeamSegment`, a frame whose PAGE2 events all go in the SAME
  direction = buffer flip → final page applied to the whole frame (= the RAM
  actually displayable); a frame that flips in BOTH directions (DIX MODPAGE:
  page 1 left, page 2 right of the same line) keeps the exact replay. Pinned
  `drol_pageflip_render`; `dix_modpage_split` unchanged.
- **The 6 560-wide painters re-read the live state** (`renderText80`,
  `renderDhgr`, `renderLoResDouble`, `renderTextChatMauveFgBg`,
  `renderHgrDuochrome`, `renderHiResChatMauve80` → internal
  `mem.getDisplayState()` instead of the band's `state`) — same class of bug
  as the one already fixed on the legacy painters: PAGE2/ALTCHAR mid-frame
  splits were ignored in 560 (that's why "Chat Mauve didn't flicker": it
  masked the flips). Threaded signatures, state passed everywhere.
- **Chat Mauve HGR resolution**: the color decode overwrote EVERYTHING in
  aligned-pair blocks (1 color / 4 dots = 140 effective → "soft" image).
  Ported the AppleWin `RGBMonitor.cpp UpdateHiResRGBCell` algo: a pixel is
  COLOR only if it forms an isolated 010/101 pattern with its neighbors
  (its aligned pair's color, 2 dots); everything else is black/white
  **at the full 280 px resolution** — the white runs (text, sprite outlines)
  recover their sharpness, faithful to the real RGB card. `*/hgr*/chatmauve`
  goldens regenerated; semantics pinned by `le_chat_mauve_smoke` +
  updated `display_persistence_smoke`.

## 2026-06-10 (PAL beam-racing: per-video-frame publishing + 1× speeds per standard)

- **Publishing the video event log per video frame** (the 50/60 Hz hand-off).
  The old model opened the log on every worker CPU tick
  (`beginVideoEventFrame`) and the UI **stole** it at vsync (`takeVideoEvents`
  closed the bracket); any event recorded between the UI take and the next
  tick was **silently lost** (`recordVideoEvent` no-op with the bracket
  closed). **Why it mattered**: in PAL the worker runs at 50 Hz and the UI at
  60 Hz → systematic 10 Hz beat: ~1 UI render in 6 fell in the same tick and
  received an *empty* log (→ `renderInternal`, zero splits), the others a
  *partial* log — the French Touch mid-scanline effects (*Mad Effect*, DIX)
  flickered and lost bands. No test caught it (they bracket synchronously).
  **Fix**: continuous recording; `Memory::advanceCycles` **publishes**
  `{frame-start state, events}` at each crossing of a video-frame boundary
  (65 × 262 NTSC / 312 PAL cycles — aligned on the scanner geometry, not on
  the worker's 17045/20313 budget); `takeVideoEvents` returns a **copy** of
  the last published frame, re-renderable at will by the 60 Hz UI. The
  synchronous bracket remains available for tests (`legacyEventBracket_`). A
  reset purges both logs (otherwise a phantom replay against the wiped state).
  Bonus: the WASM path (which never called `beginVideoEventFrame`) gains
  beam-racing. Pinned `video_event_publish`.
- **`$C019`/VBL follows the video standard**: the VBL edge detection
  (`advanceCycles`) and the `$C019` read used a hard-coded 262 lines; a PAL
  demo that measures the VBL period saw a 17030-cycle frame while the floating
  bus swept 20280 — two contradictory machines. Pinned by extending
  `pal_timing` (§ 4: lines 262–311 = VBL under PAL, wrap at 312).
- **1×/2×/4× speed derived from the active standard** (toolbar, AI server
  `/speed` preset, disk-turbo restoration re-seeded by `applyProfile`).
  **Why**: the hard-coded 17045 ran a PAL machine at 17045 × 50 Hz = 852 kHz
  (−16%) on the first "1×" click — effects that drag, sluggish Mockingboard
  music. Remaining assumption: `MouseCardAppleWin::kCyclesPerVbl` = 17045
  (60 Hz VBL pacing of the mouse HLE even under PAL — no effect on the demos,
  to be reworked with the //c port).

## 2026-06-01 (Release v0.7)

- **Version bump v0.6 → v0.7.** Updated the version string in the
  **5 canonical locations** listed by `CLAUDE.md` § Version string locations:
  `CMakeLists.txt` (`project(... VERSION 0.7 ...)`, which also drives
  `CPACK_PACKAGE_VERSION` + the `build_dist.sh` archive name), `src/main.cpp`
  (console banner + initial window title), `src/MainWindow_Slots.cpp`
  (runtime title that overwrites `main.cpp`'s once the profile is resolved — at
  the constructor **and** at the profile switch), `src/MainWindow.cpp`
  (*About* dialog), and `README.md` (title). `CLAUDE.md` itself updated
  (`Current release: **v0.7**`). **Why note it**: the version lives in
  duplicated strings not derived from a single source, so any bump must touch
  these points en bloc on pain of drift (window title vs About vs package).
  Single CMake source → generated header = separate backlog item.

## 2026-05-31 (Composite: signal beam-racing + phosphor curve)

- **Composite signal beam-racing.** `fillCompositeSignal` read *a single*
  end-of-frame `getDisplayState()`: the mid-scanline display switches
  (text↔graphics, page flip, DHGR on/off) were invisible in **all** composite
  modes (`ColorCompositeOE` GPU, `ColorCompositeOECpu`, `ColorAppleWin`) —
  only the LUT modes benefited from beam-racing (`renderBeamRacing` on the
  RGBA side). **Why it mattered**: a demo that goes from text to HGR mid-screen
  displayed entirely in HGR under OE/AppleWin. **Fix**: `render()` takes the
  event log **once** and passes it to both consumers;
  `fillCompositeSignal(mem, events)` replays the log band by band (zero
  `signalBuf` → `getDisplayStateAtFrameStart()` → `paintSignalBand(y0,y1)`
  which reuses the same `bandRows`/`bandScanlines` clipping as
  `renderInternalBand`). The painted `state` is a *mutable* local so the
  helpers (captured by reference) see each switch. Empty log →
  `paintSignalBand(0,192)` = byte-for-byte the old dispatch (OE GPU/CPU
  goldens unchanged). **Pitfalls**: `signalPhaseOffset_` stays a per-frame
  constant (last graphics band wins → mid-frame HGR↔DHGR split approximated);
  lo-res clips at the block-row (4 lines), like the RGBA path. Pinned
  `beam_race_composite` (text→HGR frame at scanline 96: top band = text
  waveform, bottom band = HGR waveform, and **not** HGR at the top like the
  pre-fix bug).
- **Phosphor curve (CRT gamma).** The signal-level NTSC pipeline (already
  complete: FIR Y@2.0 MHz / chroma@0.6 MHz, YUV→RGB, PAL line-phase) had no
  phosphor response. Added a `phosphorGamma` (per-channel power-law `rgb^γ`)
  in `CrtEffectStack`, **after** BCS and **before** scanlines/mask (the mask
  attenuates the light the phosphor has already produced). γ = 1.0 = identity →
  **no golden/parity touched**; γ > 1 deepens the shadows, γ < 1 lifts them.
  It's the *luminance* half of the phosphor model; `persistence` is the
  *temporal* half. Slider "Phosphor curve (gamma)" 0.6–2.6, persisted
  `ntsc_phosphor_gamma`. NB: a *glass* effect, hence always active under OE and
  under the other modes when "CRT effects on all modes" is on.

## 2026-05-31 (3D voxel view — phases 0+1; toolbar rewind button)

- **3D voxel view — "Voxel Cube" rework faithful to MicroM8 (fix).**
  The first version extruded **height = luminance** on a screen laid **flat**
  (XZ plane): bright pixels turned into stalactites, catastrophic angle,
  misshapen voxels. Scraped MicroM8 (`paleotronic.com` Quick Start +
  Features): the "Voxel Cube Color" mode stands the screen **upright**
  (monitor, XY plane) and gives **each pixel a cube of the same thickness**
  extruded toward the viewer on **+Z** ("Voxel Depth"). The height is **never**
  tied to luminance; the per-color relief (`colorShift`, "Z-axis 3D offset")
  is an **option** (default 0 → a flat slab you rotate to see the thickness).
  - **Geometry**: cube footprint XY + depth Z; column→X, row→Y (row 0 = top);
    **real 4:3** plane (2.0 × 1.5) to keep the shape of Apple II pixels.
    `heightScale`→`voxelDepth` (0.06), `+colorShift`.
  - **Camera**: defaults near front-on + slight 3/4 (azimuth 0.32 /
    elevation 0.20 / distance 2.8 / fovY ~40°), target recentered at the
    origin. **Orbit on left-drag + wheel zoom** (in `drawScreenImage`, mutate
    `voxelCam_`). `voxel3d_math` stays green (the camera math is unchanged).
  - **Follow-up (same day)**: (1) **top/bottom inverted** — the
    FBO→`ImGui::Image` presentation is a vertical mirror (like the 2D NTSC
    passes); pre-flip `gl_Position.y` in the vertex shader. (2) **Native
    resolution** — `gridW/gridH` driven by `display->width()/height()`
    (280/560 × 192) → one voxel per Apple II pixel (before: 140×96, half the
    info lost); `voxelDepth`/`colorShift` passed in **cell units** to stay
    constant between 280 and 560 wide. (3) **Per-color relief enabled**
    (`colorShift` 8 cells, luminance-weighted) → requested pin-art "pop".
    (4) **CRT-independent** — the voxel taps the color image **before**
    `CrtEffectStack` via a separate `voxelSrcTex` handle (otherwise
    scanlines/mask/barrel ended up baked into the 50k cubes).
    (5) **Pan/strafe on the middle button** — `OrbitCamera::pan` slides the
    target in the camera's right/up plane, scaled in world-units/pixel for 1:1
    tracking (orbit = left-drag, zoom = wheel). (6) **Moiré removed** —
    **abutting** cubes (`cubeFill` 0.9→1.0: a flat fill becomes a continuous
    slab again, end of the grid of gaps) **+ supersampling** (`superSample`
    2×: FBO rendered at 2×, mip-chain, trilinear minify by ImGui →
    anti-aliasing without MSAA resolve).
  - **Phase 3 — settings panel** (`renderVoxelSettingsWindow`, View ▸
    "3D voxel settings…"): live sliders `voxelDepth` / `colorShift` /
    `cubeFill` / `superSample` (pushed to **3×** by default) / `ambient`, +
    Reset view / Reset settings buttons. The renderer is **owned from the
    moment settings load** (ctor without GL) so the panel and the `voxel_*`
    keys (persisted) wire directly onto `voxel3d_`, even before enabling the
    3D view. The grid resolution stays auto (= screen).
  - **P4 — WASM perf guard**: under `__EMSCRIPTEN__`, `process()` caps
    `superSample ≤ 2` + FBO ≤ 2048² (reduces the factor until it fits) and
    `MainWindow` caps `gridW ≤ 280` (halves the 560 DHGR geometry).
    Native unchanged (`ss ≤ 4`, 8192²). WASM/WebGL2 compile re-checked.
  - **Fidelity bonus — Mono + per-color-index depth**: `mono` checkbox
    ("Voxel Cube Mono", grey output, relief preserved) and `perColorDepth`
    (snap to the nearest of the 16 lo-res `kVoxelPalette` colors → discrete
    per-color relief instead of continuous luminance). 16-iteration search
    loop in the vertex shader (per instance, not per pixel); `glUniform3fv`
    added to the loader. Persisted `voxel_mono` / `voxel_percolor_depth`.
    P5 (rewind tie-in) **deferred** on request.
  - **Wheel fix in WASM**: Emscripten's GLFW port doesn't deliver `wheel`
    events to ImGui (`io.MouseWheel` stayed at 0 → 3D zoom inoperative in the
    browser). Added an `emscripten_set_wheel_callback("#canvas")` in
    `main.cpp` that feeds `io.AddMouseWheelEvent` (same scale as the ImGui
    backend) — a surgical choice so as not to touch the shell's canvas sizing
    (vs `ImGui_ImplGlfw_InstallEmscriptenCallbacks` which also hooks
    resize/fullscreen).

- **Rewind button in the toolbar** (left of Pause, mirroring Step on the
  right): `ICON_FA_BACKWARD_FAST`, **hold = live-rewind** (same gesture as
  `F6` / the Devices ▸ Rewind bar). Greyed out while there's no history.
  `F6` and the button share a single edge-tracker (`driveRewindHold`).

- **MicroM8-style 3D voxel view — foundations (phases 0+1).**
  - **Why / arch**: extrude the screen into cubes (height = pixel luminance)
    with an orbital camera. Key choice: it's an **orthogonal view axis**, not
    a `HiResMode` — a render pass that consumes the **already-decoded RGBA
    texture** (any color mode + NTSC/CRT), exactly like `CrtEffectStack`.
    Universal, free for all modes.
  - **Phase 0 — `Mat4.h`**: Vec3 + column-major Mat4 (perspective/lookAt/
    multiply) + `OrbitCamera` (azimuth/elevation/distance → view-proj). No
    dependency (no glm). Pinned `voxel3d_math` (perspective entries,
    orthonormal lookAt basis, projection of the target to the center).
  - **Phase 1 — `Voxel3DRenderer.{h,cpp}`**: **instanced** cubes
    (`glDrawElementsInstanced`, ~13k for 140×96), height+color per
    **vertex texture-fetch** of the framebuffer, shading by **screen
    derivatives** (no normal attribute → stays on the single `aPos` bound by
    the shared shader helper). FBO **with depth** (the 2D passes have none).
    Same lazy-init + GL state save/restore pattern as `NtscPostProcessor`;
    WebGL2/GLES3 compatible (instancing + VTF + derivatives, no geometry
    shader). Toggle **View ▸ "3D voxel view"** (persisted `show_3d_voxel`),
    wired into `drawScreenImage` before the final blit.
  - **To follow**: orbital camera on mouse drag + zoom (P2), settings panel +
    lighting (P3), resolution steps / heightfield (P4), rewind tie-in
    "freeze + orbit" (P5). The GL render is verified by running the app
    (no golden hash — the camera math, itself, is tested).

## 2026-05-31 (Rewind — delta codec, UI, disk state, heavy cases: phases 2→5)

- **MicroM8-style rewind completed (phases 2 to 5).** The base (phases 0+1,
  below) stored full snapshots; these phases make it actually usable.
  All pinned: `rewind_delta`, `rewind_transport`, `rewind_slot_state` (+
  `rewind_roundtrip` unchanged = the API's regression safety net).
  - **Phase 2 — XOR delta + keyframes codec** (`RewindBuffer`): a full
    keyframe every `keyframeInterval` frames (default 120 ≈ 2 s), XOR deltas
    between (only the modified spans, coalesced over gaps < 16 bytes). 30 s
    goes from ~315 MB to ~10 MB. Reconstruction = nearest keyframe ≤ i + XOR
    of the deltas. **rebase-on-evict** eviction: the front always stays a
    keyframe (the next delta is promoted before dropping). Public API
    unchanged → `rewind_roundtrip` (phase 1) passes as-is = proof of
    non-regression. *Why keyframes+delta rather than reverse-delta alone:
    XOR is its own inverse, so a single delta direction serves bidirectional
    scrubbing, and keyframes bound the cost of random seek.*
  - **Phase 3 — UI + transport + live-rewind**: `Rewind_ImGui` (Devices ▸
    Rewind) — Record toggle, timeline, transport |< / << (hold) / <| / |> /
    resume, history-duration slider; `F6` = hold-to-live-rewind everywhere
    (MicroM8 gesture). Restore served with the **worker parked**:
    `rewindBeginScrub()` sets Stopped then `waitUntilParked()` waits for
    `workerParked_` (set in the worker's Stopped CV wait) → a UI restore can't
    be overwritten by an in-flight Running frame (the Running branch exhausts
    its whole budget before re-checking the mode). `rewindEndAndResume`
    restores + `truncateAfter` (discards the abandoned future) + restarts.
    Ring emptied on `coldBoot`.
  - **Phase 4 — slot card state**: `SlotPeripheral::append/loadSnapshotState`
    (no-op by default); `DiskIICard` serializes its mechanical state + LSS
    (quarter-track head, motor, phase magnets, data register, sequencer,
    rotational timing — **not** the media or the PROMs). `MachineSnapshot`
    writes `SLOTn` sections **only if `includeSlots=true`** (rewind opt-in;
    the AI-control `/snapshot` API keeps its "disk excluded" contract — an
    archive file may survive a media change). The restore routes to the slot's
    card (magic+version → a foreign card ignores a blob that isn't its own)
    and tolerates their absence. A rewind during a disk I/O no longer leaves
    the head on the wrong nibble. Full bit-for-bit machine round-trip (incl.
    SLOT6) pinned.
  - **Post-review hardening** (multi-agent review): (a) the park handshake
    race fixed — `setMode(non-Stopped)` clears `workerParked_` on the setter
    side, otherwise a fast resume→rescrub read a stale flag; (b)
    `DiskIICard::loadSnapshotState` bounds `activeDrive` (index guard); (c)
    history slider disabled during scrub (eviction would shift the indices);
    (d) `SnapshotIO` memory backend rewritten as a zero-copy streambuf
    (`VectorOutBuf`/`ArrayInBuf`) — removes the double-copy via `stringstream`
    on each capture (~21 MB/s on IIe, much more with RamWorks).
  - **Phase 5 — heavy cases**: `maxBytes_` memory budget (default 256 MiB) on
    top of the frame cap → RamWorks (~10 MB/keyframe) bounded (less history
    rather than RAM that explodes). `flushAudioForRewind()` (speaker reset) on
    each restore → a time jump is silent, not a "pop". Capture also wired into
    `tickFrame()` (single-thread WASM path).
  - **Audio chips serialized** (closing the audio gap): `MockingboardCard` and
    `PhasorCard` serialize the register/timer state of their `Via6522` (24 b) +
    `Ay3_8910` (34 b) via the `SlotPeripheral` hook — `append/loadSnapshot`
    helpers shared by both cards, LE packing pooled in `ByteIO.h`. So the music
    survives a rewind (not just the speaker flush). The AY is a register model
    (the synthesis derives from the 16 registers) → restoring the registers
    restores the sound exactly. Pinned `rewind_audio_state` (full bit-for-bit
    machine round-trip incl. Mockingboard).
  - **SSI263 speech serialized**: `Ssi263::append/loadSnapshot` (30 b: 5
    registers + the phoneme read cursor), wired into the Sound II variant of
    `MockingboardCard` → speech also survives a rewind. Covered by
    `rewind_audio_state` (Sound II block).
  - **Disk writes undone on rewind** (Phase 6): the DiskIICard snapshot bumped
    to v2 — it carries the nibble track buffers for the loaded disks that are
    physically writable and non-WOZ
    (`DiskImage::append/loadMediaSnapshot`), so a disk write is undone by a
    rewind. The delta codec keeps the cost ~nil as long as no track is
    written; the read caches re-derive from the restored nibbles. Read-only /
    WOZ / empty disks = 1 flag byte. Pinned `rewind_disk_write` (media COW +
    write-via-card undone end-to-end).
  - **Remaining gap**: writes on a writable WOZ not undone (WOZ stores its bits
    in `wozRaw`, a distinct store; WOZ originals are generally
    write-protected). Tracked cleanly if needed. Detail → `DEV.md` § Rewind.

## 2026-05-31 (Rewind — foundations, phases 0+1)

- **MicroM8-style rewind — capture/restore state base (no UI).**
  - **Why**: record the machine state continuously to allow going back in time
    (scrub/step-back). Architecture choice: a ring-buffer of state snapshots
    (RetroArch-style) rather than deterministic input replay — decoupled from
    the CPU hot-path, robust, and reuses `SnapshotIO` as-is. The delta/keyframe
    (to reduce the ~175 KB/frame cost) is phase 2; here we store full snapshots
    to validate the capture→restore loop bit-for-bit.
  - **Phase 0 — `SnapshotIO` memory backend**: `SnapshotWriter(vector&)` /
    `SnapshotReader(ptr,len)` alongside the existing file backend, via a
    `std::stringstream` bound to a `std::ostream&`/`std::istream&` member
    (all the section/length logic reused). Binary format identical between the
    two backends. Pinned: `snapshot_memory_roundtrip` (round-trip +
    byte-for-byte parity vs the file writer).
  - **`MachineSnapshot.{h,cpp}`**: extraction of the canonical
    `CPU`/`MEM`/`MEX` sequence out of `AiControlServer` (which slims down by
    ~63 lines). Single source of truth shared by the AI-control API AND the
    rewind, so no more possible divergence. The security hardening stays: a
    16-byte length gate on the CPU section (over-read of a forged blob,
    "round 10 #3") + a 16 MiB MEX cap → `RestoreResult{false,…}` (the API
    always returns 400). Covered by `ai_control_server_smoke` (no regression).
  - **Phase 1 — `RewindBuffer.{h,cpp}`**: a `std::deque` ring of full
    snapshots, oldest-first eviction beyond `maxFrames` (default 1800 ≈ 30 s
    @ 60 Hz), `restore(i)` / `restoreToCycle(cycle)`. Capture wired at the
    `workerLoop`'s quiescent frame boundary (after the CPU budget + IWM tick),
    guarded by `enabled()` before taking `stateMtx` → zero cost when disabled
    (default). Pinned: `rewind_roundtrip` (bit-for-bit round-trip + eviction +
    `restoreToCycle` seek).
  - **Assumed gaps this phase**: card/disk state out of the snapshot (a rewind
    during disk I/O leaves the head where the live sim put it → phase 4:
    `SlotPeripheral` hook + `DiskIICard` drive state); audio chips desynced;
    no UI (phase 3); WASM not wired (phase 5). Detail → `DEV.md` § Rewind /
    time-travel.

## Earlier (≤ 2026-05-30) — archived

The entries from 2026-05-30 back to 2026-05-14 (pre-v0.7) are moved into
[`docs/archive/CHANGELOG-2026-05.md`](docs/archive/CHANGELOG-2026-05.md) to
keep this file focused on the current cycle. Full history → `git log`.
