// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Software framebuffer for the Apple II video subsystem. Renders into an
// RGBA buffer that the UI uploads as an OpenGL texture each frame. Three
// modes follow the soft-switch state held by Memory:
//   - Text  (40×24, char ROM glyphs, normal / inverse / flashing)
//   - Lo-res (40×48, 16 colours, same screen memory as text)
//   - Hi-res (280×192, NTSC artifact decoded by a 7-bit sliding window over
//             a linearised 560-sub-pixel bit stream — MSB-driven half-dot
//             delay applied at the stream level so byte-boundary fringing
//             emerges naturally)
// Mixed mode shows hi-res in the top 160 pixels and four text rows below.
//
// IIe extension. When 80COL + TEXT are on the renderer emits at the native
// 80-col resolution (560×192) by interleaving aux RAM (even columns) and
// main RAM (odd columns) text bytes. Mixed-mode bottom-4-rows-text follows
// the same path, with the top 20 HGR rows horizontally doubled into the
// 560-wide buffer. width()/height() reflect whichever buffer is live.
//
// The display owns no GL state — the MainWindow uploads `pixels()` to a
// texture it manages. Keeping this class GL-free makes it trivial to unit
// test and lets a future WASM build reuse the exact same renderer.

#ifndef POM2_APPLE2_DISPLAY_H
#define POM2_APPLE2_DISPLAY_H

#include "LeChatMauveCard.h"
#include "Memory.h"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

class Apple2Display
{
public:
    static constexpr int kWidth   = 280;
    static constexpr int kHeight  = 192;
    static constexpr int kWidth80 = 560;

    // Hi-res rendering style. ColorNTSC is the "real Apple II on a colour TV"
    // experience — bit-stream artifact colour with authentic byte-boundary
    // fringing. ChatMauveRGB is the French Péritel-RGB experience (clean
    // 16-color decode, two distinct grays, no fringing) — only available
    // when a LeChatMauveCard is plugged on the slot bus, and its FIFO
    // mode (BW560 / Mixed / Chunky / COL140) selects the sub-variant.
    // The three Mono variants render the same bit stream as luminance
    // through a phosphor tint: White is a reference monitor, Green
    // approximates Apple's standard P31 CRT, Amber adds long persistence
    // (history-buffer lerp) on top of an amber tint.
    enum class HiResMode {
        ColorNTSC,              // MAME composite_color_mode=0, LUT row 0
        ColorCompMedium,        // MAME composite_color_mode=1, LUT row 1
                                //   (4n medium-color runs; uglier 40-col
                                //    text but cleaner mid-tones)
        ColorComp4Bit,          // MAME composite_color_mode=2, no artifact —
                                //   each 4-dot nibble maps directly to one
                                //   palette index. The sharp / hard-edge
                                //   variant.
        ChatMauveRGB,
        // True NTSC composite simulation à la OpenEmulator: the display
        // emits a 1-bit luminance bitstream at 14.318 MHz (560 samples ×
        // 192 lines) instead of pre-decoded RGB; MainWindow runs that
        // through a GLSL shader that demodulates Y/I/Q on the subcarrier,
        // applies low-pass filtering, persistence, scanlines and barrel.
        // Falls back to ColorNTSC framebuffer when the signal can't be
        // produced (lo-res mode, no GL context, etc.).
        ColorCompositeOE,
        // Same OpenEmulator composite demodulation, but run on the CPU into
        // the RGBA framebuffer (no GLSL) — selectable so the composite look
        // is available without a GL shader path and so users can compare the
        // two. render() demodulates signalBuf into frame80 here, exactly like
        // ColorAppleWin; MainWindow then presents it as a normal framebuffer
        // (the shared CrtEffectStack still applies if "CRT effects on all
        // modes" is on). The demod honours the same knobs as the GPU shader
        // (hue, chroma-bandwidth sharpness, PAL line-phase alternation,
        // textSharp) via setOeDemodParams — pinned pixel-identical by
        // oe_demod_gpu_cpu_parity.
        ColorCompositeOECpu,
        MonoWhite,
        MonoGreen,
        MonoAmber,
        // AppleWin-style NTSC: CPU-only cascaded-IIR composite signal
        // simulation + 4-phase × 12-bit-history LUT (chromaLut[4][4096]).
        // Distinct from MAME's static 7-bit-window LUT and from
        // OpenEmulator's full subcarrier shader — see DEV.md § AppleWin
        // NTSC. Three sub-modes selected via setAppleWinSubMode():
        // Monitor (sharp), Tv (Monitor's comb luma + 50% frame blend),
        // Idealized (Monitor luma WITH the IIR filters, chroma boosted
        // 1.6× — POM2-only, see AppleWinNtsc.cpp buildPhaseTables).
        // Enum order no longer matters: the mono phosphor tint is picked
        // by an explicit switch (phosphorFor() in Apple2Display.cpp),
        // not by indexing a table with the enum value.
        ColorAppleWin,
    };

    // Variant selector for the ColorAppleWin path. Mirrors AppleWin's
    // VT_COLOR_MONITOR_NTSC / VT_COLOR_TV / VT_COLOR_IDEALIZED entries.
    enum class AppleWinSubMode {
        Monitor,        // sharp, IIR-filtered NTSC artifact decode
        Tv,             // comb luma + 50% previous-frame blend (TV receiver)
        Idealized,      // Monitor luma (same IIR filters) with 1.6× boosted
                        // chroma — saturated, flat-panel-friendly look
                        // (POM2-only; see AppleWinNtsc.cpp:193-197)
    };

    Apple2Display();

    // Renders the current frame into the internal RGBA buffer based on
    // the soft-switch state read from `mem`. Frame-counter advances the
    // 2 Hz flash phase used by the text mode.
    void render(Memory& mem);

    /// Drops the static-text frame-skip key, forcing the next render() to
    /// repaint even if nothing the key covers has changed. Every mutation of
    /// the framebuffer this class performs itself already invalidates (see
    /// `framebufferMutated()`), so this exists for OUTSIDE callers that reach
    /// past pixels() — and for `display_dirty_skip_test`, which runs a
    /// forced-full-repaint reference display alongside a skipping one.
    void invalidateTextFrameCache() { framebufferMutated(); }

    // Runs the OE-CPU composite demod render() deferred (it only consumes
    // display-owned buffers, so it must NOT hold the caller's stateMutex —
    // ~1-2 ms of FP FIR that used to stall the CPU worker every UI frame).
    // Call after releasing the lock, before pixels(). No-op when nothing
    // is pending.
    void finishPendingCpuDemod();

    // Lazily completes any deferred OE-CPU demod so every consumer (tests,
    // screenshot paths) that does render() → pixels() stays correct without
    // knowing about the deferral; MainWindow calls finishPendingCpuDemod()
    // explicitly after releasing stateMutex, making this a no-op there.
    const uint32_t* pixels() const {
        const_cast<Apple2Display*>(this)->finishPendingCpuDemod();
        return useFrame80_ ? frame80.data() : frame.data();
    }

    /// Serializes the post-stateMutex demod/pixels phase between the UI
    /// thread and the AI control server's /screen handler. The demod
    /// deliberately runs OUTSIDE stateMutex (it costs ~1-2 ms and reads
    /// only display-owned buffers), which left frame80/signalBuf/
    /// pendingCpuDemodRows_ shared between two threads with no lock at
    /// all. Lock order: stateMutex → demodMutex, never nested the other
    /// way.
    std::mutex& demodMutex() { return demodMutex_; }
    int             width()  const { return useFrame80_ ? kWidth80 : kWidth; }
    int             height() const { return kHeight; }

    /// Raster position (visible scanline + 40-byte column index) of a CPU
    /// cycle within the current frame. Used by the beam-racing replay to map
    /// a video soft-switch's `VideoEvent::emuCycle` to *where on the screen*
    /// the beam was — `scanline` (vertical band) and `byteCol` (the new
    /// horizontal mid-scanline split). Public + static so it can be unit
    /// tested in isolation.
    struct RasterPos { int scanline; int byteCol; };
    static RasterPos frameCycleToPos(uint64_t emuCycle,
                                     VideoStandard std = VideoStandard::NTSC);

    /// Auxiliary 64 KB RAM pointer for IIe 80-column rendering. Set by
    /// MainWindow once the IIe ROM is detected and Memory::setIIEMode(true)
    /// has been called. Pointer is non-owning. May be nullptr; the 80-col
    /// path then falls back to reading main RAM in both halves of each pair
    /// (still produces 80 character cells, just without aux content).
    void setAuxMemory(const uint8_t* aux) { auxRam = aux; }

    /// Hi-res rendering mode. Switching modes resets the persistence buffer
    /// so an amber afterglow doesn't bleed into a freshly-selected green
    /// phosphor.
    void      setHiResMode(HiResMode m);
    HiResMode getHiResMode() const { return hiResMode; }

    /// Demod-stage knobs for the OE composite CPU path — the same subset the
    /// GPU shader consumes (NtscPostProcessor uHue / uSharpness / uPalMode +
    /// the textSharp bypass). MainWindow mirrors the live NtscParams here
    /// every frame so ColorCompositeOECpu (and the OE-GPU mixed-frame CPU
    /// demod) track the CRT-Settings sliders exactly like the GPU shader —
    /// they used to silently ignore hue/sharpness/PAL/textSharp. Defaults are
    /// the shader's neutral values, keeping headless consumers (tests, tools)
    /// byte-identical when they never call setOeDemodParams.
    struct OeDemodParams {
        float hue       = 0.0f;   // -0.5..+0.5 → ±π U/V rotation
        float sharpness = 0.5f;   // 0.5 = neutral OE 0.6 MHz chroma kernel
        bool  palMode   = false;  // line-phase alternation (V sign per line)
        bool  textSharp = true;   // true = crisp framebuffer TEXT (no demod)
    };
    void setOeDemodParams(const OeDemodParams& p) { oeDemod_ = p; }
    const OeDemodParams& getOeDemodParams() const { return oeDemod_; }

    /// Display state consumed by the most recent render() — the *published*
    /// frame's soft-switch snapshot, captured under the caller's stateMutex.
    /// MainWindow's present-path decisions (sharp-text bypass, demod routing)
    /// read this instead of re-polling Memory::getDisplayState(), which the
    /// CPU worker may have advanced past the rendered frame in the meantime
    /// (a text↔graphics switch in that window flashed one LUT-fallback frame).
    const Memory::DisplayState& lastRenderState() const { return lastRenderState_; }

    /// Capture helper for the ColorCompositeOE (GPU) mode: schedules the CPU
    /// demod of signalBuf into frame80 so pixels() returns the composite
    /// image the shader shows on screen instead of the LUT fallback
    /// framebuffer (AI /screen, headless captures). No-op unless the last
    /// render() produced a signal that the GPU path would demodulate (mixed
    /// frames and sharp-text frames already present the framebuffer). Call
    /// after render(); the demod itself runs lazily in pixels() /
    /// finishPendingCpuDemod(). When it does schedule, it rewrites the whole
    /// framebuffer behind render()'s back and drops the static-text skip key
    /// with it, so the next render() rebuilds the on-screen image instead of
    /// leaving the capture's demodulated pixels on display.
    void demodCompositeForCapture();

    /// AppleWin sub-mode selector. Only consulted when hiResMode ==
    /// ColorAppleWin; ignored otherwise. Switching to Tv clears the
    /// line-blur history so the previous Monitor frame doesn't bleed.
    void            setAppleWinSubMode(AppleWinSubMode m);
    AppleWinSubMode getAppleWinSubMode() const { return appleWinSubMode; }

    /// Le Chat Mauve / Video-7 RGB card. When non-null AND its render mode
    /// is anything other than NTSC-passthrough, the hi-res renderer takes
    /// the clean-RGB path: 4-bit-window palette decode (no artifact LUT,
    /// no inter-byte fringing, two distinct grays for the $5/$A patterns).
    /// The pointer is non-owning; the card lifetime is managed by the
    /// SlotBus that holds it.
    void setChatMauveCard(LeChatMauveCard* c) { chatMauve = c; }

    /// Composite signal buffer for the ColorCompositeOE mode. 8-bit
    /// per-sample luminance at 14.318 MHz (560 samples per scanline,
    /// 192 scanlines, one byte per sample = 0 or 255). The MainWindow
    /// uploads this as an R8 texture and feeds it to a GLSL shader that
    /// demodulates the NTSC subcarrier. The buffer is filled by render()
    /// whenever ColorCompositeOE (or ColorAppleWin) is selected, for every
    /// Apple II video mode: HGR, DHGR, 40/80-col text AND lo-res (lo-res
    /// emits each 4-bit colour nibble as its repeating 4×-fsc bit pattern
    /// (paintLoRes40 for GR, paintLoResDouble for DLGR). fillCompositeSignal
    /// produces a signal for all supported modes, so signalProduced() is
    /// true whenever one of those two modes is active.
    static constexpr int kSignalWidth  = kWidth80;   // 560
    static constexpr int kSignalHeight = kHeight;    // 192
    const uint8_t* signal() const { return signalBuf.data(); }
    int signalWidth () const { return kSignalWidth;  }
    int signalHeight() const { return kSignalHeight; }
    /// True when the last render() filled signalBuf with a valid composite
    /// waveform — i.e. whenever the selected mode is ColorCompositeOE or
    /// ColorAppleWin (all video modes serialise, lo-res included). False for
    /// every other hi-res mode.
    bool signalProduced() const { return signalProducedFlag; }
    /// NTSC subcarrier phase offset (0 = HGR/text, 1 = DHGR) applied by
    /// OE CPU/GPU demod and ColorAppleWin — matches MAME rotl4(absX+1).
    int signalPhaseOffset() const { return signalPhaseOffset_; }
    /// True when mixed-mode composite output was assembled in frame80
    /// (demod top + crisp text band) — MainWindow should present
    /// screenTexture instead of the GPU demod texture.
    bool mixedCompositeUsesFramebuffer() const { return mixedCompositeUsesFb_; }

private:
    std::mutex demodMutex_;          // see demodMutex()
    std::vector<uint32_t> frame;     // kWidth   * kHeight RGBA pixels
    std::vector<uint32_t> frame80;   // kWidth80 * kHeight RGBA pixels (IIe)
    // Which buffer pixels()/width() publish. Write it ONLY through
    // setUseFrame80() / scheduleCpuDemodInto80() — the trailing underscore is
    // there so a plain `useFrame80 = true` no longer compiles: flipping the
    // published buffer is a framebuffer mutation, and the static-text skip is
    // only sound while every such mutation announces itself.
    bool useFrame80_    = false;     // true for the current frame when 80-col
    const uint8_t* auxRam = nullptr; // IIe auxiliary RAM (non-owning)
    HiResMode hiResMode = HiResMode::ColorNTSC;
    AppleWinSubMode appleWinSubMode = AppleWinSubMode::Tv;
    LeChatMauveCard* chatMauve = nullptr;   // non-owning, owned by SlotBus
    // Previous-frame RGBA buffer used by ColorAppleWin's Tv sub-mode for
    // its 50% line-blur. Same dimensions as `frame` / `frame80`; one set
    // each so HGR and DHGR don't share state.
    std::vector<uint32_t> appleWinPrev;
    std::vector<uint32_t> appleWinPrev80;
    // `appleWinPrev80` holds a frame this path rendered (so it may be blended
    // against), and frame80 does (so it may be stashed). Both false at
    // power-on and after a mode / sub-mode switch: blending the first frame
    // against a cleared-to-black stash halved its brightness, and stashing
    // frame80 before it held an AppleWin frame ghosted the outgoing mode in.
    bool appleWinPrevValid_ = false;
    bool appleWinPainted_   = false;
    // History buffer for monochrome phosphor decay. One byte per pixel
    // (0..255 luminance). Color mode leaves this untouched; switching modes
    // clears it. We carry two parallel buffers so HGR (280×192) and DHGR
    // (560×192) can each accumulate afterglow without their dimensions
    // fighting — without `persistenceL80`, DHGR mono had no decay because
    // the path wrote a 560-wide frame against a 280-wide history.
    std::vector<uint8_t> persistenceL;
    std::vector<uint8_t> persistenceL80;
    // OpenEmulator-style composite signal: one byte per 14.318 MHz sample
    // (0x00 = black, 0xFF = white). 560 samples × 192 lines = 105 600 bytes.
    // Populated alongside `frame` / `frame80` when hiResMode ==
    // ColorCompositeOE. The shader path in MainWindow only consumes this
    // when `signalProducedFlag` is true.
    std::vector<uint8_t> signalBuf;
    bool signalProducedFlag = false;
    int  signalPhaseOffset_ = 0;
    bool mixedCompositeUsesFb_ = false;
    OeDemodParams oeDemod_{};
    Memory::DisplayState lastRenderState_{};

    // ── Static-text frame skip ──────────────────────────────────────────
    // A Callgrind profile (2026-07-30) put ~30 % of POM2's total work in the
    // display, and 98 % of THAT in text: renderText + glyphRows7, ~887 host
    // instructions per character cell, re-decoding all 960 cells every frame
    // even on a screen that had not changed a byte since the last one — which
    // is most of the time under DOS, BASIC or a text app.
    //
    // The skip is deliberately narrow, because the general case is not safe:
    //   * BEAM RACING — a frame carrying video events is painted as several
    //     bands with different DisplayStates (and, on the 560-wide path, with
    //     a column-bounded save/restore). Never skipped: `render()` only
    //     consults this on the `events.empty()` branch.
    //   * PERSISTENCE — the graphics painters implement a phosphor rule
    //     (`max(target, prev x decay)` into persistenceL/persistenceL80), so
    //     their output legitimately changes every frame from identical inputs.
    //     Only FULL-SCREEN TEXT is skipped; renderText/renderText80 write no
    //     persistence at all (verified), making their output a pure function.
    //   * FLASH — the blinking-glyph phase is
    //     `frameCounter / kFlashHalfPeriodFrames & 1`, so it is part of the
    //     key. frameCounter is the EMULATED frame index and is already
    //     standard-aware (cycleCounter / (65 * scanlinesPerFrame)), so PAL's
    //     312-line/50 Hz frame and NTSC's 262-line/60 Hz frame both advance it
    //     at their own rate and the key follows automatically.
    //
    // The key is a byte-exact copy of the source, not a hash: at 4 KB of video
    // RAM against ~850 K instructions of glyph decoding, a memcmp is ~200x
    // cheaper and cannot collide.
    //
    // The card fields below exist because `Memory::DisplayState` is NOT the
    // whole picture: a Le Chat Mauve "Eve" writes its own $C0B8-$C0BB
    // registers, which select the colour-TEXT renderer (and with it the
    // 560-wide frame80), reach the card through SlotBus::broadcastVideoSwitch,
    // and — unlike $C05E/$C05F — push NO video event. So a frame right after
    // `STA $C0B9` has an empty event log and an unchanged DisplayState: every
    // other term of the key agrees, and without these the skip served a stale
    // screen at the wrong geometry. That card is the //c PAL profile's
    // built-in slot 7, i.e. the French Touch / DIX target hardware.
    //
    // `textSharp` is in the key for the same reason: unchecking "Sharp text"
    // makes the OE-CPU path demodulate full-screen TEXT instead of painting it
    // crisp (`oeDemodsText` in render()), and on the OE-GPU path it decides
    // whether MainWindow presents this framebuffer at all. It is a host-side
    // knob — no guest write, no video event — so like the colour mode it is
    // invisible to every other term.
    struct TextFrameKey {
        bool                 valid = false;
        Memory::DisplayState state{};        // POD of bools — compared wholesale
        bool                 iie = false;
        bool                 flashPhase = false;
        bool                 textSharp = true;
        int                  hiResModeId = -1;
        const void*          charRom = nullptr;
        size_t               charRomSize = 0;
        const void*          chatMauve = nullptr;   // card identity (may be unplugged)
        int                  chatMauveState = -1;   // mode + Eve toggles
        std::vector<uint8_t> vram;           // $0400-$0BFF, main + aux
    };
    // The key describes the pixels that are in the framebuffer RIGHT NOW, so
    // it is published (`commitTextFrameKey`) at the very END of render() and
    // torn down by every framebuffer mutation. Two slots, not one:
    //   * `textFrameKey_`      — what the framebuffer currently holds. Any
    //                            mutation clears it, whoever performs it and
    //                            whenever: render()'s own painters (harmless,
    //                            they are about to republish) or a caller
    //                            poking the buffer AFTER render() returned
    //                            (demodCompositeForCapture on the AI /screen
    //                            path), which is the case a single well-placed
    //                            invalidate call kept getting wrong.
    //   * `nextTextFrameKey_`  — this frame's candidate, filled by
    //                            staticTextFrameUnchanged() iff the frame is a
    //                            skippable full-screen text frame. Everything
    //                            else (beam-raced frames, CPU demods, graphics)
    //                            leaves it invalid, so "those frames never
    //                            enable a skip" falls out of the commit rather
    //                            than out of scattered invalidations.
    TextFrameKey textFrameKey_;
    TextFrameKey nextTextFrameKey_;

    // Per-scanline decode cache for the NTSC-LUT hi-res path (renderHiRes).
    // The 280 output pixels of a row are a pure function of its 40 doubled
    // words (bit7Mask already folded in) and the decode flavour (`key` =
    // LUT row | square-filter); so the decode is skipped when both match
    // the previous call and the cached pixels are copied instead. It caches
    // INPUT → OUTPUT, never "the framebuffer already holds this", so it is
    // correct no matter who else painted the row since (mixed-mode text,
    // beam-raced column splits, a capture demod): the copy is made every
    // time, only the decode is elided. Lode Runner's attract screen
    // measured renderHiRes at a third of the whole disk-boot profile; with
    // the cache a static row costs an 80-byte compare and a 1 KB memcpy.
    struct HgrRowCache {
        uint16_t words[40];
        uint8_t  key   = 0;
        bool     valid = false;
        std::array<uint32_t, 280> out;
    };
    std::array<HgrRowCache, 192> hgrRowCache_{};
    /// Publishes this frame's candidate key. Called on every exit from
    /// render() (RAII), so a future early return cannot leave the previous
    /// frame's key describing pixels that have since been repainted.
    void commitTextFrameKey() {
        textFrameKey_ = std::move(nextTextFrameKey_);
        nextTextFrameKey_.valid = false;   // moved-from: `valid` was copied
    }
    /// Announces that the published framebuffer (its pixels, or which of the
    /// two buffers is published) no longer matches what render() last painted.
    /// EVERY such mutation must funnel through here — that is what the private
    /// `useFrame80_` name and the two helpers below enforce.
    void framebufferMutated() { textFrameKey_.valid = false; }
    void setUseFrame80(bool v) { useFrame80_ = v; framebufferMutated(); }
    /// Arms the deferred OE-CPU demod over rows [0, rows) and routes the UI to
    /// frame80 — the single door to `pendingCpuDemodRows_`, so a demod
    /// scheduled from outside render() (screen capture) can never be forgotten
    /// by the skip. The demod itself runs in finishPendingCpuDemod().
    void scheduleCpuDemodInto80(int rows) {
        useFrame80_          = true;
        pendingCpuDemodRows_ = rows;
        framebufferMutated();
    }
    /// True when this frame's full-screen text is byte-identical to the one
    /// already in the framebuffer, so painting it again would be a no-op.
    /// Fills `nextTextFrameKey_` as a side effect.
    bool staticTextFrameUnchanged(Memory& mem, const Memory::DisplayState& state);
    static constexpr int kMixedTextFirstScanline = 160;

    /// Repaint the bottom text band of a mixed frame over a demodulated
    /// picture. `state` MUST be the published frame's state (render()'s
    /// `state` local), never `mem.getDisplayState()` — see the definition.
    void patchMixedTextBand(Memory& mem, const Memory::DisplayState& state);
    // Frame counter — drives the FLASH attribute animation for screen
    // bytes in the $40-$7F range (the Apple II Monitor's blinking cursor
    // and inverse-blinking spaces). Wraps freely; only the parity of
    // (frameCounter / kFlashHalfPeriodFrames) is read. Set from the
    // EMULATED frame index each render() (cycleCounter / 65·scanlines) so
    // flash runs at the machine's own 50/60 Hz, not the host monitor's.
    uint32_t frameCounter = 0;
    // Emulated-frame bookkeeping for render-rate-independent pacing:
    // delta = emu frames elapsed since the previous render() call (0 when
    // the same frame is re-rendered on a >60 Hz host, clamped at 8 across
    // stalls, 0 on backwards jumps). Drives phosphor decay + Tv blur.
    uint64_t lastEmuFrame_  = 0;
    uint32_t emuFrameDelta_ = 0;
    // Half-period of the inverse-flashing animation. 16 frames @ 60 Hz →
    // 32-frame cycle ≈ 1.875 Hz, matching MAME IIe's `frame_number() & 0x10`
    // (toggles every 16 frames) and AppleWin's `(++counter & 0xF)==0`. (Was 15
    // — ~6.7% too fast and inconsistent with the cited & 0x10 model.)
    static constexpr uint32_t kFlashHalfPeriodFrames = 16;

    // `col0`/`col1` bound rendering to the 40-byte column window [col0, col1)
    // for the beam-racing horizontal (mid-scanline) split; default (0, 40) is
    // the full width and leaves every existing caller byte-identical. text/
    // lo-res restrict their per-column write loop; hi-res still decodes the
    // whole scanline (so the NTSC artifact window keeps its neighbour context)
    // and only the write-back + persistence update are clipped to the window.
    // `state` is passed in (not re-read from mem) so beam-raced bands paint
    // with the display state active for THAT band — page select (PAGE1/PAGE2),
    // 80STORE base and ALTCHAR all switch mid-frame, not just the mode. The
    // full-frame path passes render()'s published `state` down too.
    //
    // `clipY0`/`clipY1` clip the row/block painters to exact scanlines for
    // non-row-aligned beam splits: bandRows() hands a straddled text row (or
    // lo-res block row) to BOTH adjacent bands, and each band paints only the
    // scanlines inside its own [clipY0, clipY1) window. Defaults cover the
    // full frame, leaving every existing caller byte-identical.
    void renderText  (Memory& mem, const Memory::DisplayState& state,
                      int firstRow, int lastRow, int col0 = 0, int col1 = 40,
                      int clipY0 = 0, int clipY1 = kHeight);
    void renderLoRes (Memory& mem, const Memory::DisplayState& state,
                      int firstRow, int lastRow, int col0 = 0, int col1 = 40,
                      int clipY0 = 0, int clipY1 = kHeight);
    void renderLoResDouble(Memory& mem, const Memory::DisplayState& state,
                           int firstRow, int lastRow,           // DLGR (80-col)
                           int clipY0 = 0, int clipY1 = kHeight);
    void renderHiRes (Memory& mem, const Memory::DisplayState& state,
                      int firstScanline, int lastScanline, int col0 = 0, int col1 = 40);
    // Single HGR under a Le Chat Mauve / Video-7 card, at the card's native
    // 560-dot output (each HGR dot = 2 frame80 dots). `hm` is what the card
    // makes of the switches (LeChatMauveCard::hgrMode): the LCM colour rule
    // (2-bit cell colour, 3-bit window — docs/chatmauve_plan.md § 3.2, pixel
    // for pixel AppleWin `UpdateHiResRGBCell`), plain monochrome, or the
    // Eve's SPEC1/SPEC2 variants. FgBg is not handled here (see
    // renderHgrDuochrome).
    void renderHiResChatMauve80(Memory& mem, const Memory::DisplayState& state,
                                int firstScanline, int lastScanline,
                                LeChatMauveCard::HgrMode hm);
    // Foreground/background HGR: the bitmap in MAIN $2000-$3FFF, and at the
    // same address in AUX one byte of colours per 7-dot block. Two cards
    // have it with the nibbles the other way round — the Video-7's F/B mode
    // (AN3 off, 80COL off; hi nibble = foreground) and the Eve's CP280
    // (table IX-1; hi nibble = background) — hence `auxHiIsForeground`.
    // Each HGR dot becomes 2 dots in frame80. Callers gate on `auxRam`.
    void renderHgrDuochrome(Memory& mem, const Memory::DisplayState& state,
                            int firstScanline, int lastScanline,
                            bool auxHiIsForeground);
    // Eve COL280A/B (table IX-1): the 560-dot stream (aux then main per
    // column, bit 0 first) in 2-dot cells, code = dot + 2 × next dot, through
    // one of two fixed 4-colour palettes — read off Purplesoft's `& PLOT`
    // (see the definition). Each cell = 2 dots in frame80.
    void renderDhgrCol280(Memory& mem, const Memory::DisplayState& state,
                          int firstScanline, int lastScanline, bool paletteB);
    // Eve TXTGREEN: the text rows of a band, white → green (P31 tint) in
    // whichever framebuffer the band was painted into. Text is black and
    // white before this pass, so the remap is exact.
    void tintTextGreen(const Memory::DisplayState& state, int scanY0, int scanY1);
    // The RGB-card pipeline is selected AND a card is plugged. Without the
    // card ChatMauveRGB silently renders as ColorNTSC (a real machine pulled
    // out of its adapter still has composite on the wire).
    bool chatMauveActive() const {
        return hiResMode == HiResMode::ChatMauveRGB && chatMauve != nullptr;
    }
    void renderInternalBandImpl(Memory& mem, const Memory::DisplayState& state,
                                int scanY0, int scanY1);
    // IIe-only. Renders text rows [firstRow, lastRow) into `frame80` at
    // 560×192. Reads aux RAM for even columns and main RAM for odd
    // columns (per AppleWin's scanner). `altCharSet` toggles flashing
    // inverse vs. mousetext+non-flashing inverse (the IIe ALTCHAR switch).
    void renderText80(Memory& mem, const Memory::DisplayState& state,
                      int firstRow, int lastRow,
                      int clipY0 = 0, int clipY1 = kHeight);
    // IIe-only. Renders DHGR scanlines [firstScanline, lastScanline) into
    // `frame80`. Reads main + aux HGR pages: aux byte at offset c
    // contributes 7 bits to dots [c*14 .. c*14+6], main byte contributes
    // dots [c*14+7 .. c*14+13]. Color: each 4 consecutive dots form a
    // 4-bit lo-res palette index (560 dots → 140 color cells per line).
    // Monochrome HiResModes render dot-by-dot luminance through the
    // selected phosphor.
    void renderDhgr  (Memory& mem, const Memory::DisplayState& state,
                      int firstScanline, int lastScanline);
    // Colour TEXT with an RGB card: 40 columns, char code from main RAM, the
    // aux byte at the same text address holds the cell's two lo-res colours.
    // Video-7 F/B text (AN3 on, 80COL off) has the foreground in the high
    // nibble; the Eve's TXT16 ($C0B9, 80COL off) has the BACKGROUND there
    // (manual IV-2.2: `POKE -16199,16*F+C`... F = fond) — `auxHiIsForeground`.
    // The 7-bit glyph row is doubled to 14 dots, each dot painted fg/bg.
    // Renders rows [firstRow, lastRow) into `frame80` at 560 wide. Port of
    // MAME `apple2video.cpp` text_update (:788-791) + render_line_color_array
    // (:571-583).
    void renderTextChatMauveFgBg(Memory& mem, const Memory::DisplayState& state,
                                 int firstRow, int lastRow,
                                 int clipY0, int clipY1, bool auxHiIsForeground);
    // Horizontally double `frame[firstRow*8 .. lastRow*8)` into `frame80`.
    // Used when mixed-mode HGR is on top and 80-col text is at the bottom.
    void upscaleFrameToFrame80(int firstScanline, int lastScanline);

    // Populate signalBuf from RAM for the active display state. Returns
    // true on success (HGR / DHGR / 40-col text / lo-res / DLGR).
    // Always called from render() when hiResMode == ColorCompositeOE.
    // `events` is the mid-frame video soft-switch log: when non-empty the
    // signal is recomposed band-by-band (beam-racing) so mid-scanline mode
    // switches land in the composite waveform; when empty the whole frame is
    // painted from the single end-of-frame display state (fast path).
    bool fillCompositeSignal(Memory& mem,
                             const std::vector<Memory::VideoEvent>& events);

    // CPU OpenEmulator demod: demodulate signalBuf (560×192 R8) rows
    // [0, rows) into frame80 (560×192 RGBA) — the same Y/I/Q math as the
    // GLSL demod shader, run on the CPU (per-row 1D FIR, so a row limit is
    // exact). Used by HiResMode::ColorCompositeOECpu and mixed OE frames;
    // scheduled via pendingCpuDemodRows_ + finishPendingCpuDemod().
    void renderCompositeOeCpu(int rows);
    // 0 = none; else demod rows [0, n). Armed only by scheduleCpuDemodInto80();
    // cleared by render() (a demod armed against the previous frame's signal
    // must never fire onto this frame's pixels) and by finishPendingCpuDemod().
    int  pendingCpuDemodRows_ = 0;

    // The actual frame dispatch (text / hires / dhgr / mixed). render()
    // is a thin wrapper that calls this then optionally fills signalBuf.
    /// Full-frame repaint of the PUBLISHED frame — `state` is render()'s,
    /// not the live one (see the definition).
    void renderInternal(Memory& mem, const Memory::DisplayState& state);
    void renderInternalBand(Memory& mem, const Memory::DisplayState& state,
                            int scanY0, int scanY1);
    // Column-bounded variant of renderInternalBand: paints the rectangle
    // [scanY0, scanY1) × [col0, col1). The legacy 280-wide path (text /
    // hi-res / lo-res) threads the column window into its painters; the
    // 560-wide IIe / Le Chat Mauve modes (80-col, DHGR, DLGR, Chat Mauve)
    // paint full width into frame80 then save/restore the columns outside the
    // window (their painters carry cross-column context). Mixing a 280-wide
    // and a 560-wide segment on one scanline targets different buffers and is
    // a documented v1 scope-out.
    void renderInternalSegment(Memory& mem, const Memory::DisplayState& state,
                               int scanY0, int scanY1, int col0, int col1);
    // True when `state` renders through the legacy 280-wide path (so a
    // mid-scanline column split is meaningful). Mirrors the branch
    // conditions at the top of renderInternalBand.
    bool usesLegacyPath(Memory& mem, const Memory::DisplayState& state) const;
    // Shared beam-race decomposition: sorts `events` into raster order, builds
    // per-scanline column segments [col0, col1), merges vertically-identical
    // scanlines into bands, and invokes `paint(state, y0, y1, col0, col1)` for
    // each band × column segment. Both renderBeamRacing (RGBA) and
    // fillCompositeSignal (composite signal) drive their painters through it,
    // so the two horizontal-split replays can never diverge.
    // `startLatch` + the per-segment latch: the Chat Mauve mode latch is
    // walked in parallel with DisplayState from the same event log ($C05E/F
    // rising edges clock the $C00C/D level), so a mid-frame latch change
    // paints each band with the value of ITS moment. Callers without a card
    // pass 0b11 (COL140) and ignore the parameter.
    static void forEachBeamSegment(
        const Memory::DisplayState& frameStart,
        std::vector<Memory::VideoEvent> events,
        VideoStandard std,
        uint8_t startLatch,
        const std::function<void(const Memory::DisplayState&,
                                 int y0, int y1, int col0, int col1,
                                 uint8_t latch)>& paint);
    void renderBeamRacing(Memory& mem, std::vector<Memory::VideoEvent> events);
    // The Chat Mauve latch for the band being painted by the beam-raced
    // replay (renderDhgr consumes it); < 0 outside a replay = ask the card.
    int bandLatch_ = -1;

    static void applyVideoEvent(Memory::DisplayState& state, Memory::VideoEventKind kind,
                                bool value);

    // Address of the first byte of text/lo-res row `y` in the active page.
    static uint16_t textRowAddress(int y, bool page2);
    // Address of the first byte of HGR scanline `y` in the active page.
    static uint16_t hgrRowAddress(int y, bool page2);

    // Apple II lo-res palette (16 colours, IIGS-corrected approximation).
    static const uint32_t kLoResPalette[16];
    // Le Chat Mauve / Video-7 lo-res palette — 16 distinct colours with
    // visually distinct grays at indices 5 and 10 (the "Chat Mauve
    // trademark" actually surfaces in lo-res, not HGR — see Apple2Display.cpp).
    static const uint32_t kChatMauveLoResPalette[16];
};

#endif // POM2_APPLE2_DISPLAY_H
