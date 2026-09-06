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

// OpenEmulator-inspired NTSC composite simulation as a GLSL shader pass.
// Consumes a 560×192 R8 luminance signal produced by Apple2Display when
// hiResMode == ColorCompositeOE, demodulates Y/I/Q from the 14.318 MHz
// subcarrier, low-pass filters chroma, applies user knobs (brightness,
// contrast, saturation, hue, sharpness, persistence, scanlines, barrel),
// and renders into an RGBA framebuffer texture MainWindow then draws via
// ImGui::Image() in place of the regular `screenTexture`.
//
// The algorithm is reimplemented from public NTSC spec (FCC/CCIR §73.682
// composite encoding), Linards Ticmanis' Apple II video timing notes,
// and Zellyn Hunter's openemulator-explainer notebook. No OpenEmulator /
// libemulation source is copied — POM2 stays MIT-licensed.

#ifndef POM2_NTSC_POSTPROCESSOR_H
#define POM2_NTSC_POSTPROCESSOR_H

#include <cstdint>
#include <string>

namespace pom2 {

struct NtscParams
{
    // Standard composite-TV knobs (range 0..1 unless noted).
    float brightness  = 0.0f;   // -0.5..+0.5 added to luma
    float contrast    = 1.0f;   //  0.5..1.5 scaling around 0.5
    float saturation  = 1.0f;   //  0..2 chroma multiplier
    float hue         = 0.0f;   // -0.5..+0.5, full I/Q rotation at ±0.5

    // Sharpness controls the chroma low-pass bandwidth. 0.5 is neutral and
    // matches the OE-faithful CPU path; higher values sharpen the chroma
    // bandwidth, while lower values stay on the safe soft kernel.
    float sharpness   = 0.5f;

    // Phosphor persistence: 0 = no afterglow, 1 = infinite. Reasonable
    // CRT values are 0.3..0.6.
    float persistence = 0.4f;

    // Scanlines + barrel are pure post-effects (no NTSC physics).
    float scanlines   = 0.25f;  // 0 = off, 1 = black between every line
    float barrel      = 0.02f;  // 0 = flat, 0.2 = old curved CRT

    // Shadow-mask emulation (post-effect, after demodulation). The mask
    // is a multiplicative pattern in RGB space — triad (3-stripe RGB
    // mask of consumer TVs), aperture grille (vertical RGB stripes of
    // Trinitron/Sony), or Bayer-like dot-mask (offset triads). Strength
    // 0 = off (the GPU bypasses the mask multiplication entirely);
    // strength 1 = full darkening of the off-channels in each cell.
    enum class ShadowMask : int {
        Off            = 0,
        Triad          = 1,   // classic 3-stripe shadow mask
        ApertureGrille = 2,   // Trinitron vertical stripes
        Dot            = 3,   // offset triads (consumer CRT)
    };
    ShadowMask shadowMask         = ShadowMask::Off;
    float      shadowMaskStrength = 0.5f;  // 0..1

    // Post-glass luminance gain (multiplicative, applied after the shadow
    // mask). Re-brightens the picture that scanlines + mask necessarily dim,
    // mirroring OpenEmulator's `luminanceGain` stage. 1.0 = neutral; raise
    // toward ~1.5 to compensate heavy scanlines/mask. Only used by the
    // CrtEffectStack glass pass.
    float luminanceGain = 1.0f;  // 1.0..2.0

    // Center lighting (vignette), OpenEmulator-faithful: the shader computes
    // `lighting = cuv·(1/centerLighting − 1); rgb *= exp(−dot(lighting))`, so
    // 1.0 = perfectly flat (OE's Apple II default — vignette off) and lower
    // values darken the edges. CrtEffectStack glass pass only.
    float centerLighting = 1.0f;  // 0.5..1.0 (1.0 = flat)

    // Phosphor response curve: a per-channel power law applied to the beam
    // intensity → emitted light, modelling the CRT's gamma. `rgb = rgb^γ`,
    // applied after BCS and before the spatial scanline/mask modulation.
    // 1.0 = identity (off — preserves every existing golden/parity test);
    // γ > 1 deepens shadows for more CRT-like contrast, γ < 1 lifts them.
    // CrtEffectStack glass pass only (pairs with `persistence`, the temporal
    // half of the phosphor model).
    float phosphorGamma = 1.0f;  // 0.6..2.6 (1.0 = flat)

    // Analog RGB bandwidth, in MHz — the video chain between the machine and
    // the tube, modelled the way OpenEmulator models its "connection" types.
    // A digital/TTL RGB link carries square dots and needs no filter; an
    // ANALOG one does not. The Le Chat Mauve cards are the concrete case:
    // they have two connectors (docs/chatmauve_plan.md § 3.7) — a TTL RGB
    // header, and a Péritel/SCART socket whose R, G and B each leave through
    // a resistor ladder and three trim pots, then a metre of cable. That path
    // rolls off well before the dot rate, so single-dot detail arrives with a
    // rise time instead of a vertical edge.
    //
    // The filter is a windowed-sinc FIR applied horizontally, per channel, on
    // the SOURCE sample grid — which is what makes it self-consistent across
    // video modes without a second special case. A 560-wide framebuffer is
    // sampled at 14.318 MHz, a 280-wide one at 7.16 MHz (`Apple2Display`'s
    // `frame80` / `frame`), so the same MHz figure is a different fraction of
    // Nyquist in each: ~5 MHz visibly softens a true 560-dot DHGR / COL280
    // picture, barely touches a 280-dot HGR one doubled into frame80, and is
    // above Nyquist — hence a no-op, and skipped outright — on the 280-wide
    // buffer. That is the real behaviour of one cable carrying all of them.
    //
    // 0 = off (the TTL connector, and the default: no existing look changes).
    // ~5-6 MHz is the analog Péritel figure. CrtEffectStack only.
    float rgbBandwidthMHz = 0.0f;  // 0 = off, else ~2..8

    // PAL composite mode: alternates the Q-subcarrier sign every other
    // scanline (line-phase alternation). On a real PAL TV this cancels
    // hue errors at the cost of vertical chroma resolution; here it
    // produces the characteristic softer-coloured European Apple II
    // look. Off by default — POM2 ships defaults that match the NTSC
    // Apple II that 90% of users have in mind.
    bool palMode = false;

    // When ColorCompositeOE is selected and the Apple II is in TEXT
    // mode (40 or 80 col), this toggle decides whether the shader
    // demodulates the glyph bit-stream (composite-faithful, but blurry
    // for white-on-black text) or whether MainWindow draws the sharp
    // RGB framebuffer directly (legible, but breaks immersion). On by
    // default — readability wins for daily use.
    bool textSharp = true;
};

class NtscPostProcessor
{
public:
    NtscPostProcessor();
    ~NtscPostProcessor();
    NtscPostProcessor(const NtscPostProcessor&) = delete;
    NtscPostProcessor& operator=(const NtscPostProcessor&) = delete;

    // First-call setup. Compiles the shader, allocates the signal
    // texture, FBOs and a fullscreen-quad VAO. Returns true on success;
    // on failure (shader compile error, GL entry points missing, …) the
    // postprocessor reports `available()` = false and process() becomes
    // a no-op so callers can fall through to the regular RGB path.
    // Must be called with a current GL context. Safe to call multiple
    // times — second and later calls are no-ops.
    bool initialize();

    bool available() const { return ready; }

    // Replace the live parameter set. Cheap (only writes a struct
    // copy); the new values take effect on the next process() call.
    void setParams(const NtscParams& p) { params = p; }
    const NtscParams& getParams() const { return params; }

    // Run one frame of the NTSC simulation. `signal` points to a
    // signalWidth × signalHeight R8 buffer (typically 560×192 = the
    // Apple II's 4×-subcarrier sample grid). Returns the GL texture
    // name holding the RGBA output (the caller draws it via ImGui).
    // Returns 0 if the postprocessor isn't `available()`.
    unsigned int process(const uint8_t* signal,
                         int signalWidth, int signalHeight,
                         int phaseOffset = 0);

    // Dimensions of the texture returned by process(). Since the Phase-4
    // demod-only split these equal the signal dimensions (560×192) — the
    // scanline/upscale work moved to CrtEffectStack.
    int outputWidth () const { return outW; }
    int outputHeight() const { return outH; }

    // Diagnostics for the Settings panel.
    const std::string& lastError() const { return errorMsg; }

private:
    bool   ready         = false;
    bool   initialized   = false;
    std::string errorMsg;

    // GL objects (declared unsigned int to keep this header GL-include
    // free; the cpp casts as needed). Demod-only since Phase 4: a single
    // output texture (no persistence ping-pong — persistence moved to
    // CrtEffectStack downstream).
    unsigned int program   = 0;
    unsigned int signalTex = 0;
    unsigned int outputTex = 0;
    unsigned int fbo       = 0;
    unsigned int vao       = 0;
    unsigned int vbo       = 0;

    // Uniform locations resolved at link time. -1 if absent. Only the
    // colour-recovery knobs remain; the CRT-glass uniforms (brightness,
    // contrast, saturation, persistence, scanlines, barrel, shadow mask)
    // moved to CrtEffectStack.
    int uSignal     = -1;
    int uSignalSize = -1;
    int uHue        = -1;
    int uSharpness  = -1;
    int uPalMode    = -1;
    int uPhaseOffset = -1;

    int outW    = 0;
    int outH    = 0;
    int signalW = 0;
    int signalH = 0;
    // GL_MAX_TEXTURE_SIZE, read once at initialize() — see clampTexDim().
    int maxTexSize_ = 0;

    NtscParams params{};

    bool createTextures(int signalW_, int signalH_);
    // A texture dimension this GL implementation can actually allocate.
    int  clampTexDim(int v) const
    {
        return (maxTexSize_ > 0 && v > maxTexSize_) ? maxTexSize_ : v;
    }
    void destroyGL();
};

} // namespace pom2

#endif // POM2_NTSC_POSTPROCESSOR_H
