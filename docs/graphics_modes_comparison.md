# POM2 graphics modes — deep comparison with the original sources

POM2 offers **ten hi-res render modes** (`Apple2Display::HiResMode`,
[`src/Apple2Display.h:57-101`](../src/Apple2Display.h)). Each mode is
ported from a reference emulator (MAME, AppleWin, OpenEmulator) or
models a hardware behaviour (Le Chat Mauve, monochrome phosphors).

For each mode, this document lists: the exact algorithm as implemented
in POM2, the original source with URL, the intentional deviations, the
pinned tests, and the capture of the intro screen produced by
`build/tests/render_total_replay_modes` (probes
`floppyemu/Total Replay v6.1.hdv`, then
`hdv/Total Replay II v1.0-alpha.4.hdv`; default 60 M instructions after
//e boot).

---

## Overview

| # | Mode | Original source | Type | Output | Pinned test | Image |
|---|---|---|---|---|---|---|
| 1 | `ColorNTSC` | MAME `apple2video.cpp` (PR #10773) | 7-bit LUT | 280×192 | `hgr_render_smoke`, `dhgr_render_smoke` | [↓](#1-colorntsc) |
| 2 | `ColorCompMedium` | MAME `apple2video.cpp` row 1 | 7-bit LUT | 280×192 | `dhgr_render_smoke` | [↓](#2-colorcompmedium) |
| 3 | `ColorComp4Bit` | MAME `apple2video.cpp` square filter | nibble→palette | 280×192 | `dhgr_render_smoke` | [↓](#3-colorcomp4bit) |
| 4 | `ChatMauveRGB` | AppleWin `RGBMonitor.cpp` (PR #837) + Péritel hardware | direct RGB | 560×192 | `le_chat_mauve_smoke`, `video7_parity_smoke` | [↓](#4-chatmauvergb) |
| 5 | `ColorCompositeOE` | OpenEmulator + apple2shader | GLSL shader | 560×384 | `oe_demod_gpu_cpu_parity`, `text_oecpu_crisp` | [↓](#5-colorcompositeoe) |
| 5b | `ColorCompositeOECpu` | OpenEmulator (same demod, on the CPU) | CPU demod → RGBA framebuffer | 560×192 | `oe_demod_gpu_cpu_parity`, `text_oecpu_crisp` | [↓](#5-colorcompositeoe) |
| 6 | `MonoWhite` | AppleWin VT_MONO_WHITE (empirical palette) | luminance | 280/560×192 | `display_persistence_smoke` | [↓](#6-monowhite) |
| 7 | `MonoGreen` | AppleWin VT_MONO_GREEN (P31 phosphor) | luminance×tint+decay | 280/560×192 | `display_persistence_smoke` | [↓](#7-monogreen) |
| 8 | `MonoAmber` | AppleWin VT_MONO_AMBER (long-persistence) | luminance×tint+decay 0.96 | 280/560×192 | `display_persistence_smoke` | [↓](#8-monoamber) |
| 9 | `ColorAppleWin` | AppleWin `NTSC.cpp` (Simms / Charlesworth) | LUT IIR 4-phase × 4096 | 560×192 | `applewin_ntsc_smoke` | [↓](#9-colorapplewin) |

Captures generated with:

```bash
cmake --build build --target render_total_replay_modes
./build/tests/render_total_replay_modes mode_captures   # default 60 M instructions
```

---

## 1. ColorNTSC

**Source**: MAME `apple2video.cpp` —
[`render_line_artifact_color()`](https://github.com/mamedev/mame/blob/master/src/mame/apple/apple2video.cpp)
introduced in [PR #10773 (benrg)](https://github.com/mamedev/mame/pull/10773),
refined by [PR #10792](https://github.com/mamedev/mame/pull/10792)
(128-entry table derived by symmetry) and
[PR #10835](https://github.com/mamedev/mame/pull/10835)
(`composite_color_mode` 0/1/2 extraction). Composite *color mode 0*.

**POM2 algorithm** ([`Apple2Display.cpp::renderHiRes`](../src/Apple2Display.cpp), `kArtifactColorLut` path):

1. Serialize each HGR scanline into 560 sub-pixels via `buildHgrWordRow`
   (`kBitDoubler[128]` doubles each bit; the MSB applies a half-dot
   shift handled in the stream).
2. Sliding 7-bit window (3 left-context bits + 1 center + 3 right)
   over the stream.
3. Index `kArtifactColorLut[0][w & 0x7F]` (128 entries copied verbatim
   from MAME, [`src/Apple2VideoDecode.h:53`](../src/Apple2VideoDecode.h)).
4. `rotl4b(lutEntry, absX)` extracts the 4-bit palette index for the
   current NTSC phase (4 phases every 4 dots).
5. `kLoResPalette[16]` (IIGS-corrected palette) yields the final RGB
   color ([`:1356`](../src/Apple2Display.cpp)).
6. Downsample 560 → 280 by averaging pairs (the optical low-pass of a
   real CRT; otherwise the 14 MHz pattern aliases against the 7 MHz grid).

**Deviations vs MAME**:

| Deviation | Detail |
|---|---|
| "39 seam fix-ups" | MAME applies 39 color corrections at byte boundaries when certain MSB combinations trigger spurious black. POM2 does not port these fix-ups — barely visible in practice. |
| Glow / bloom | MAME can optionally add a CPU Gaussian halo. POM2 delegates any "CRT glow" effect to the `ColorCompositeOE` mode (shader). |
| MSB rev-0 mask | DHGR with `dhgr=1` masks bit 7 (`bit7Mask=0x7F`) to emulate a rev-0 — `Apple2Display.cpp::renderHiRes`. Same in MAME. |

**Tests**: `hgr_render_smoke` (LUT corners, $7F→white, MSB shift),
`dhgr_render_smoke` (aux/main interleave, two distinct grays).

![ColorNTSC](img/total_replay_01_ColorNTSC.png)

---

## 2. ColorCompMedium

**Source**: MAME `apple2video.cpp`, *composite_color_mode = 1*. Row 1
of `kArtifactColorLut` — 8 of 128 entries differ from row 0,
biased toward "4n medium colors" that render 4-dot color runs
better but make 40-col text uglier.

**POM2 algorithm** ([`Apple2Display.cpp::renderHiRes`](../src/Apple2Display.cpp), `composite_color_mode = 1`):
identical to `ColorNTSC` except `lutRow = 1`.

**Deviations**: none.

**Tests**: no dedicated test — covered by `dhgr_render_smoke`.

![ColorCompMedium](img/total_replay_02_ColorCompMedium.png)

---

## 3. ColorComp4Bit

**Source**: MAME `apple2video.cpp`, *composite_color_mode = 2*. The
"square filter" variant: skip the 7-bit window and take each 4-dot
nibble directly as the palette index. Sharp edges, no inter-byte
fringing.

**POM2 algorithm** ([`Apple2Display.cpp::renderHiRes`](../src/Apple2Display.cpp),
`composite_color_mode = 2` branch, HGR + DHGR):

```
nibble = (window >> (kContextBits - 1)) & 0x0F   // kContextBits = 3
palette_idx = rotl4b(nibble | (nibble << 4), absX + is_80_column - 1)
pixel = kLoResPalette[palette_idx]               // is_80_column = 0 HGR, 1 DHGR
```

**Deviations**: none. Literal port of MAME `rotl4(w & 0x0f,
x + is_80_column - 1)` (`apple2video.cpp:487-494`). ⚠️ Fixed 2026-05:
the old formula (`window >> 3`, rotation `absX`) diverged from MAME on
~50 % of interior dots; verified bit-exact against a MAME oracle
(0/2.2 M dots) after a coupled fix of both the nibble origin **and** the
phase. See `docs/archive/video_parity_audit_2026-05-30.md` (history).

**Tests**: `dhgr_render_smoke` + the `*/4bit` hashes of `display_golden_hash`
(regenerated against the MAME-correct output → non-regression pin).

![ColorComp4Bit](img/total_replay_03_ColorComp4Bit.png)

---

## 4. ChatMauveRGB

**Source**: hardware card
**Le Chat Mauve / Video-7 AppleColor RGB** (Péritel, France).
Reference implementation: AppleWin `source/RGBMonitor.cpp`
[PR #837](https://github.com/AppleWin/AppleWin/pull/837)
("Feline" palette extracted from a white-balanced video capture of a
real card). MAME variant: `apple2video.cpp:896-977` (4 DHGR rgbmodes).

**POM2 algorithm** ([`LeChatMauveCard.h`](../src/LeChatMauveCard.h),
[`Apple2Display.cpp::renderHiResChatMauve80`](../src/Apple2Display.cpp) + table
`kChatMauveHGR` for HGR, [`::renderDhgr`](../src/Apple2Display.cpp) for DHGR).

The card taps the pre-modulation digital stream at the slot connector:

- **HGR**: byte-by-byte decode of the 7-bit stream, **per pixel** (port
  of the AppleWin `RGBMonitor.cpp UpdateHiResRGBCell` algorithm, updated
  2026-06-10). Bit 7 = palette bank (NOT a half-dot delay, as on
  composite). Each pixel is judged against its **two neighbors**: only an
  isolated `010`/`101` pattern takes the color of its aligned pair
  (`kChatMauveHGR[2][4]`); any other pattern is **black/white at full
  280 px resolution** (bit runs → sharp white). The earlier variant
  forced everything into pair blocks (1 color / 4 dots = 140 effective px
  → soft image, blurry text/outlines). Native 560-dot output via
  `renderHiResChatMauve80` (each pixel doubled into 2 dots).
- **DHGR**: 4 sub-modes driven by the AN3 FIFO (soft-switches
  `$C00C/$C00D` data + `$C05E/$C05F` clock):
  - `BW560` — strict mono 560×192.
  - `Mixed` — per-byte MSB chooses color-cell vs bit-mapped mono.
  - `Chunky160` — `aux | (main<<8)` → 4 pixels of 4-bit, 3 dots each, 480
    pixels centered within 560 with 40 dots of black margins.
  - `COL140` — default at reset. 4-dot block → nibble → `rotl4(n,1)` →
    `kChatMauveLoResPalette` (Feline palette with two distinct grays
    at indices 5 and 10 — the Chat Mauve "signature", where MAME collapses
    the two indices to a single neutral gray).
- **Colored fg/bg text** (`renderTextChatMauveFgBg`,
  [`:1305`](../src/Apple2Display.cpp)): active on IIe in 40-col text
  + DHGR (AN3) on. Char code from main RAM, fg/bg colors from aux RAM
  (hi/lo nibble). 7-bit glyph doubled into 14 dots. Port of MAME
  `apple2video.cpp:788-791`.

**Deviations vs MAME**:

| Deviation | Detail |
|---|---|
| Indices 5 ≠ 10 | POM2 keeps the two distinct grays of the Feline palette (AppleWin). MAME collapses both to `0xFF808080`. Intentional choice (the Chat Mauve signature). |
| Dragon Wars bit-7 toggle | `LeChatMauveCard::setInvertBit7()` (read back via `invertBit7()`) — palette-bank inversion switch for Dragon Wars, which sets the MSB the opposite way. Not in MAME nor AppleWin (a known issue in both). |
| Native 560 output | `renderHiResChatMauve80` writes directly into `frame80`, rather than upscaling a 280×2 `frame`. Framebuffer fidelity gain (sharp screenshot), visually identical to the screen. |
| Per-pixel HGR decode | POM2 follows AppleWin `UpdateHiResRGBCell` (010/101 pattern → color, otherwise full-resolution B/W), not the pair-based decode: restores the sharpness of the real RGB card on text and sprites. |

**Tests**: `le_chat_mauve_smoke` (FIFO clocking, COL140 vs BW560,
distinct palette indices 5 and 10, NTSC fallback without the card),
`video7_parity_smoke` (4 rgbmodes + fg/bg vs MAME oracle),
`dhgr_render_smoke` (two grays).

![ChatMauveRGB](img/total_replay_04_ChatMauveRGB.png)

---

## 5. ColorCompositeOE

**Source**: **OpenEmulator** (Marc S. Ressl, GPL v3) — NTSC demod in a
GLSL fragment shader. WebGL port by Zellyn Hunter
([`apple2shader`](https://github.com/zellyn/apple2shader),
[Observable explainer](https://observablehq.com/@zellyn/apple-ii-ntsc-emulation-openemulator-explainer)).
POM2 **reimplements** the public NTSC spec (FCC §73.682) — no
OpenEmulator code copied, POM2 stays under its own license.

**POM2 algorithm** (fragment shader at
[`NtscPostProcessor.cpp:148-253`](../src/NtscPostProcessor.cpp)):

1. `Apple2Display::fillCompositeSignal` ([`:2373`](../src/Apple2Display.cpp))
   serializes the current mode into 1-bit luminance at 14.318 MHz (560×192 R8).
   HGR/DHGR/40-col/80-col text/40-col lo-res all supported.
2. Upload R8 texture, ping-pong FBO for persistence.
3. Fragment shader, per fragment:
   - Optional barrel distortion of the UVs.
   - 17 Gaussian taps around the current column.
   - Y: narrow sigma (0.8) → sharp luma.
   - I/Q: OE chroma FIR (soft 0.6 MHz at Sharpness **0.5** = neutral, identical
     to the CPU path); demod `sin/cos(π/2·(x+phaseOffset))` — **DHGR:
     `phaseOffset=1`** (MAME `rotl4(absX+1)`), HGR/text = 0.
   - Hue rotation in the IQ plane.
   - YIQ → RGB (standard NTSC FCC matrix).
   - B/C/S/H in RGB.
   - Persistence: `max(rgb, prev * decay)`.
   - Scanlines: darken odd lines (2× vertical output).
   - Optional: procedural shadow mask (Triad / Aperture grille /
     Dot); PAL mode (Q-sign flip on odd lines).

**Deviations vs OpenEmulator**:

| Deviation | Detail |
|---|---|
| No comb filter | OE supports notch + comb (configurable). POM2 uses notch (consistent with the Apple II, which violates NTSC phase alternation). |
| Simplified persistence | OE models phosphor decay with configurable persistence + temporal ringing. POM2 does `max(decoded, prev × decay)` — less physically faithful, faster. |
| Approximated PAL | OE simulates the reduced PAL chroma band. POM2 only flips the Q sign on odd lines (line-phase alternation). |
| Lo-res supported in v2 | Initially OE-only, v2 adds lo-res signal generation via `(nibble >> (absX & 3)) & 1`. |
| Sharp text bypass | UX-only toggle: skip the shader in text mode for legibility (would otherwise lose authentic composite fringing). |

**Tests**: pinned by the ctests `oe_demod_gpu_cpu_parity` and
`text_oecpu_crisp` — the demod exists in three copies (GLSL, the CPU twin
`Apple2Display::renderCompositeOeCpu`, and the test's C++ re-simulation)
under the three-way PARITY CONTRACT stated at
[`NtscPostProcessor.cpp:143-147`](../src/NtscPostProcessor.cpp). The CPU
port in
[`tests/render_total_replay_modes.cpp`](../tests/render_total_replay_modes.cpp)
(`renderCompositeShader`, line 69) also serves as an offline oracle for the
captures.

![ColorCompositeOE](img/total_replay_05_ColorCompositeOE_shader.png)

---

## 6. MonoWhite

**Source**: AppleWin `VT_MONO_WHITE` (monitor reference). Exact RGB
provenance: empirical (visual measurement, no published CIE
chromaticity).

**POM2 algorithm** ([`Apple2Display.cpp::renderHiRes`](../src/Apple2Display.cpp), `monochrome` branch):

1. Raw bit stream (no LUT, no artifact window).
2. For each 280-wide pixel: sample 2 bits from the 560 stream → luminance
   (0/127/255).
3. Persistence: `max(target, prev × decay)` with `decay = 0.0` (no
   afterglow for white — it's the neutral reference).
4. Phosphor tint RGB = `(255, 255, 255)` — multiplicative on luminance.

**Deviations**: none. Neutral reference.

**Tests**: `display_persistence_smoke` (decay=0 control case).

![MonoWhite](img/total_replay_06_MonoWhite.png)

---

## 7. MonoGreen

**Source**: AppleWin `VT_MONO_GREEN` (P31 phosphor). POM2 comment:
*"CIE x=0.280, y=0.595"* — the published P31 chromaticity. RGB tint
empirical (0x33, 0xFF, 0x33), not formally derived from the CIE.

**Algorithm**: identical to MonoWhite + green tint + `decay = 0.85`
(short persistence, ~3 frames of glow).

**Deviations**:

| Deviation | Detail |
|---|---|
| Empirical RGB tint | (0x33, 0xFF, 0x33) calibrated visually, no published CIE measurement. AppleWin variant. |
| Decay = 0.85 | Empirical calibration. Real-world P31 has a multi-component decay (~10 ms primary + a slower secondary); POM2 approximates with a simple exponential. |

**Tests**: `display_persistence_smoke`.

![MonoGreen](img/total_replay_07_MonoGreen.png)

---

## 8. MonoAmber

**Source**: AppleWin `VT_MONO_AMBER` (long-persistence amber,
Tektronix / Sanyo type). RGB tint `(0xFF, 0xB0, 0x00)` empirical. Decay 0.96
— ~25 frames of visible afterglow.

**Algorithm**: MonoWhite + amber tint + `decay = 0.96`. Parallel
persistence buffer (`persistenceL` 280×192 + `persistenceL80` 560×192)
to avoid mixing HGR and DHGR.

**Deviations**:

| Deviation | Detail |
|---|---|
| Empirical tint | No formal reference. AppleWin variant reused. |
| Decay 0.96 | Exponential approximation of a multi-component reality. |

**Tests**: `display_persistence_smoke` — pin on the DHGR afterglow
(560-wide buffer).

![MonoAmber](img/total_replay_08_MonoAmber.png)

---

## 9. ColorAppleWin

**Source**: **AppleWin** `source/NTSC.cpp::initChromaPhaseTables`
(Sheldon Simms, Tom Charlesworth, Michael Pohoreski — GPL v2+).
Original algorithm described by Bill Buck. POM2 does a **faithful port**:
the algorithm, the IIR filter coefficients (`NTSC.cpp:115-132`), the
YIQ→RGB matrix and the white/black/gray special cases are ported
line-by-line and cited in comments (AppleWin = source of truth for these modes).

**POM2 algorithm** ([`AppleWinNtsc.cpp`](../src/AppleWinNtsc.cpp)):

1. **Pre-computation** at the first `ensureInitialized()`: tables of 4 phases ×
   4096 12-bit histories → RGBA8. For each entry:
   - Walk the 12 bits *oldest first*, **oversampled ×2**
     (`phi += 45°` per half-step = 90°/dot, 4× subcarrier alignment).
   - Three cascaded 2-pole IIR filters: `initFilterSignal` (input
     low-pass), `initFilterChroma` (**band-pass @ fs/4** — the inverted
     `x[0]` zero is what isolates the chroma), `initFilterLuma0/1` (luma
     low-pass).
   - Quadrature demodulation (cos→I, sin→Q, 1-pole smoothing `/8`).
   - YIQ → RGB FCC matrix. `y0` → Monitor table; `y1` (luma of
     *signal − chroma*, a comb) → Color-TV table.
2. **Render**: 12-bit **causal** shift register (`hist = ((hist<<1)
   | bit) & 0xFFF`), one lookup per dot of the 560-wide signal:
   `out[x] = lut[x & 3][hist12]`.
3. **Three sub-modes** (`AppleWinNtsc::SubMode`):
   - `Monitor`: `y0` luma table, sharp scanlines, full artifacts.
   - `Tv`: `y1` comb table + 50% blend with the previous frame
     (`appleWinPrev80`). Tube persistence + a receiver's comb filter.
   - `Idealized`: POM2-specific (no AppleWin equivalent) — Monitor luma
     + chroma boost ×1.6 for modern flat panels.

`CYCLESTART = 45°` aligns the hues to the MAME reference without
extra calibration.

**Fundamental differences with MAME / OpenEmulator**:

| Aspect | POM2 ColorAppleWin | POM2 ColorNTSC (MAME) | POM2 ColorCompositeOE (OE) |
|---|---|---|---|
| Approach | pre-computed 4-phase × 4096-hist LUT, 2-pole IIR (CPU) | static 128-entry 7-bit-window LUT (CPU) | 17-tap GPU shader demod |
| Frame cost | ~1 lookup/dot ≈ 0.3 ms | direct LUT lookup ≈ 0.3 ms | GPU pass ≈ 0.05 ms |
| Chroma separation | dedicated IIR band-pass @ fs/4 | implicit in the LUT | Gaussian (sigma=1.5-2.5) |
| Typical colors | green / magenta / blue (≈ MAME) | magenta / cyan / green | magenta / cyan / blue |

**Historical note**: before 2026-05 this mode used a Gaussian
approximation (luma σ=1.5, chroma σ=3 + DC-removal, boost ×10) that let
the luma absorb the subcarrier and then canceled it via `signal − luma` —
hence the near-total absence of color in flat areas (only the
transitions were tinted). AppleWin's faithful IIR band-pass fixes the
defect.

**Tests**: `applewin_ntsc_smoke` —
[`tests/applewin_ntsc_smoke_test.cpp`](../tests/applewin_ntsc_smoke_test.cpp).
Pins on:
- LUT idempotence.
- All-black signal → all-black RGB; all-white → RGB > 200 (white-ringing).
- `$7F` repeated → near-neutral high luma, no color cast.
- **`$2A` full flat area → saturated chroma** (avgSat > 40) — anti-regression
  guard for the "almost no color" bug.
- Idealized `$01` → non-black color; Tv converges toward Monitor.
- `renderFrame` iterates correctly over h scanlines.

**Captures**:

![AppleWin Monitor](img/total_replay_09_ColorAppleWin_Monitor.png)
*Monitor sub-mode — IIR + 4-phase LUT, sharp scanlines*

![AppleWin TV](img/total_replay_10_ColorAppleWin_Tv.png)
*TV sub-mode — first frame, blend with an initially black buffer (in a continuous capture the result is brighter at equilibrium)*

![AppleWin Idealized](img/total_replay_11_ColorAppleWin_Idealized.png)
*Idealized sub-mode — same 4-phase × 4096-history hue LUT as Monitor/Tv,
chroma ×1.6 (`AppleWinNtsc.cpp:75,193-197`), "modern flat panel" look*

---

## Cross-cutting comparisons

### Lo-res palettes used

POM2 exposes **two distinct 16-color palettes**:

| Index | `kLoResPalette` (NTSC/MAME) | `kChatMauveLoResPalette` (Feline AppleWin) | Difference |
|---|---|---|---|
| 5 | `0xFF808080` (neutral gray) | `0xFF7E979F` (olive) | distinct! |
| 10 | `0xFF808080` (neutral gray — same as 5) | `0xFF7F6878` (mauve) | distinct! |
| others | IIGS-corrected | Feline empirical | similar |

Source: tables `kLoResPalette` and `kChatMauveLoResPalette`
([`Apple2Display.cpp`](../src/Apple2Display.cpp)).

### Half-dot delay (MSB)

All implementations must handle the half-pixel shift when bit 7 of an
HGR byte is set (74LS74 flip-flop in the hardware).

| Implementation | Handling |
|---|---|
| MAME | Static lookup-symmetry in the 128 LUT. |
| AppleWin | Implicit transient in the IIR filter + phase offset. |
| OpenEmulator | Bitstream timing (the sample arrives 1/14 MHz later). |
| **POM2** | **Pre-stream** in `buildHgrWordRow` ([`src/Apple2VideoDecode.h:102`](../src/Apple2VideoDecode.h)): if MSB=1, shift word by 1 + carry from the last bit of the previous word. Consistent with MAME, applicable to all 4 color modes via the common stream. |

### Measured performance (single x86-64 core, Release -O3)

Approximate measurements on a recent i7, rendering one ARCHON frame (HGR
280×192):

| Mode | CPU cost | Note |
|---|---|---|
| ColorNTSC | ~0.30 ms | direct 7-bit LUT |
| ColorCompMedium | ~0.30 ms | identical, row 1 |
| ColorComp4Bit | ~0.25 ms | square filter, faster |
| ChatMauveRGB | ~0.40 ms | native 560-dot |
| ColorCompositeOE (CPU) | ~25 ms | first-class mode `ColorCompositeOECpu` (`pom2_bench --mode oecpu`), no longer only an offline oracle |
| ColorCompositeOE (GPU) | ~0.05 ms | frag shader (negligible) |
| MonoWhite/Green/Amber | ~0.40 ms | persistence buffer |
| ColorAppleWin Monitor | ~0.50 ms | LUT lookup + 6-sample delay |
| ColorAppleWin Tv | ~0.55 ms | + 50% line blend |
| ColorAppleWin Idealized | ~0.30 ms | same 4-phase × 4096-history hue LUT as Monitor with chroma ×1.6 — the figure dates from a mistaken "16-entry LUT" description; expect Monitor-class cost |

All CPU modes are well under the 16.6 ms / 60 FPS budget.

---

## Appendix — sources

- MAME `apple2video.cpp`:
  - PR #10773 — sliding-window LUT https://github.com/mamedev/mame/pull/10773
  - PR #10792 — LUT symmetry https://github.com/mamedev/mame/pull/10792
  - PR #10835 — factoring https://github.com/mamedev/mame/pull/10835
  - PR #11595 — DHGR regression fix https://github.com/mamedev/mame/pull/11595
- AppleWin:
  - `source/RGBMonitor.cpp` (Chat Mauve / Video-7) https://github.com/AppleWin/AppleWin/blob/master/source/RGBMonitor.cpp
  - `source/NTSC.cpp` (composite simulation) https://github.com/AppleWin/AppleWin/blob/master/source/NTSC.cpp
  - PR #837 — RGB videocards https://github.com/AppleWin/AppleWin/pull/837
  - Issue #523 — DHGR mixed mode https://github.com/AppleWin/AppleWin/issues/523
- OpenEmulator:
  - Repo (archived) https://github.com/openemulator/openemulator
  - libemulation https://github.com/openemulator/libemulation
  - apple2shader port (Zellyn Hunter) https://github.com/zellyn/apple2shader
  - Explainer https://observablehq.com/@zellyn/apple-ii-ntsc-emulation-openemulator-explainer
- Hardware:
  - Sather *Understanding the Apple IIe* (Enhanced Edition)
  - Apple II Reference Manual §7 — composite signal generation
  - Apple IIe Auxiliary Memory Softswitches (PDF) https://www.apple.asimov.net/documentation/hardware/machines/APPLE%20IIe%20Auxiliary%20Memory%20Softswitches.pdf
- External articles:
  - Nerdly Pleasures — Apple II Composite Artifact Color http://nerdlypleasures.blogspot.com/2021/10/apple-ii-composite-artifact-color-ntsc.html
  - Apple II Colors (MROB) http://www.mrob.com/pub/xapple2/colors.html
  - DHGR Tech Note (Apple Oldies) http://www.appleoldies.ca/graphics/dhgr/dhgrtechnoe.txt

---

## Regenerate the gallery

```bash
cd build && cmake --build . --target render_total_replay_modes
cd ..
./build/tests/render_total_replay_modes mode_captures   # default 60 M instructions
for f in mode_captures/total_replay_*.ppm; do
  base="${f%.ppm}"
  if [[ "$base" == *shader* ]]; then
    convert "$f" -filter Box -resize 200% "${base}.png"
  else
    convert "$f" -filter Box -resize 400% "${base}.png"
  fi
done
cp mode_captures/*.png docs/img/
```

The default (60 M instructions) stops on the title splash of the probed
image. To target another moment, set `POM2_RENDER_INSTRS=N`. The old
cue-sheet (11M = Mr. Robot, 15M ≈ ARCHON, 17M = Pitfall II, 22M = HERO,
30M = Bruce Lee) was tied to the retired Total Replay v5.2 image and no
longer lands on those screens.
