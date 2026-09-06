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

// Universal CRT effect stack (Phase 3 of the display layering refactor).
//
// Where NtscPostProcessor demodulates a 1-bit composite SIGNAL into RGB and
// then applies CRT post-effects (only reachable in ColorCompositeOE), this
// pass takes an already-RGBA framebuffer — the output of ANY colour pipeline
// (MAME LUT NTSC, Chat Mauve RGB-card, monochrome phosphor, AppleWin) — and
// applies the same composable post-effect layers on top: barrel geometry,
// brightness/contrast/saturation, phosphor persistence, scanlines and a
// shadow mask. This is what makes "effect layers, on any mode" possible:
// effects stop being welded to the OE shader.
//
// It reuses pom2::NtscParams so the one "CRT Settings" panel drives both the
// OE demod path and this universal stack. Hue is applied here too (a chroma
// rotation works fine on already-decoded RGB). Sharpness is honoured as a
// spatial unsharp-mask/soften (centre-neutral at 0.5) since the demod-stage
// chroma-bandwidth meaning has none on an RGB framebuffer; only palMode and
// textSharp are genuinely demod-only and stay ignored here.
//
// Safety: opt-in. MainWindow only routes the framebuffer through this when
// the user enables "CRT effects on all modes"; if the shader fails to
// compile / GL entry points are missing, available() stays false and the
// caller presents the raw framebuffer unchanged (graceful passthrough).

#ifndef POM2_CRT_EFFECT_STACK_H
#define POM2_CRT_EFFECT_STACK_H

#include <cstdint>
#include <string>

#include "NtscPostProcessor.h"   // pom2::NtscParams

namespace pom2 {

class CrtEffectStack
{
public:
    CrtEffectStack();
    ~CrtEffectStack();
    CrtEffectStack(const CrtEffectStack&) = delete;
    CrtEffectStack& operator=(const CrtEffectStack&) = delete;

    // Compile the shader + allocate the fullscreen-quad VAO. Lazy texture/
    // FBO allocation happens on the first process() (we need the source
    // dimensions). Returns true on success; on any GL failure available()
    // stays false and process() becomes a no-op (returns 0).
    bool initialize();
    bool available() const { return ready; }

    void setParams(const NtscParams& p) { params = p; }
    const NtscParams& getParams() const { return params; }

    // Apply the enabled effect layers to RGBA source texture `srcTex`
    // (logical size srcW × srcH — drives the scanline/mask frequency), and
    // render the result at the on-screen target size dstW × dstH. Rendering
    // at native output resolution lets the scanline/mask patterns be
    // analytically anti-aliased (no barrel moiré) and lets ImGui blit the
    // result 1:1 (no resample beat). Returns a GL texture name (dstW × dstH),
    // or 0 when not available().
    unsigned int process(unsigned int srcTex, int srcW, int srcH,
                         int dstW, int dstH);

    int outputWidth () const { return outW; }
    int outputHeight() const { return outH; }
    const std::string& lastError() const { return errorMsg; }

private:
    bool        ready       = false;
    bool        initialized = false;
    std::string errorMsg;

    unsigned int program      = 0;
    unsigned int outputTex[2] = {0, 0};   // ping-pong for persistence
    unsigned int fbo[2]       = {0, 0};
    unsigned int vao          = 0;
    unsigned int vbo          = 0;

    // Analog RGB bandwidth pre-pass (NtscParams::rgbBandwidthMHz). A separate
    // program rendering at SOURCE resolution: band-limiting is an operation on
    // the sample grid, not on the screen, so filtering here — before the glass
    // pass upscales — is both correct and cheap (560x192, once per frame).
    // Allocated lazily and only while the knob is non-zero.
    unsigned int bwProgram    = 0;
    unsigned int bwTex        = 0;
    unsigned int bwFbo        = 0;
    int          bwW_ = 0, bwH_ = 0;
    int          uBwSrc      = -1;
    int          uBwSrcSize  = -1;
    int          uBwCutoff   = -1;

    int uSrc         = -1;
    int uPrevFrame   = -1;
    int uSrcSize     = -1;
    int uOutSize     = -1;
    int uBrightness  = -1;
    int uContrast    = -1;
    int uSaturation  = -1;
    int uHue         = -1;
    int uSharpness   = -1;
    int uPersistence = -1;
    int uScanlines   = -1;
    int uBarrel      = -1;
    int uShadowMask  = -1;
    int uShadowStr   = -1;
    int uLuminanceGain = -1;
    int uCenterLighting = -1;
    int uPhosphorGamma = -1;

    int  outW = 0, outH = 0;
    // GL_MAX_TEXTURE_SIZE, read once at initialize(). The glass pass renders
    // at the on-screen size, and that can exceed the limit on the small GPUs
    // POM2 targets (the Pi's V3D caps at 4096): an unclamped glTexImage2D
    // there fails with GL_INVALID_VALUE, the FBO goes incomplete and the
    // window goes black with nothing in the log.
    int  maxTexSize_ = 0;
    int  srcW_ = 0, srcH_ = 0;
    int  pingPongIdx = 0;
    bool firstFrame  = true;

    NtscParams params{};

    bool createTextures(int w, int h);
    // w/h clamped to what this GL implementation can actually allocate.
    int  clampTexDim(int v) const;
    // Delete every GL object this stack owns. Called from the destructor —
    // the pass is torn down while the context is still current (~MainWindow
    // runs before glfwTerminate), and `= default` there leaked a program, a
    // VAO, a VBO and up to three textures + FBOs per instance.
    void destroyGL();
    // Run the bandwidth pre-pass over `srcTex` (srcW x srcH). Returns the
    // filtered texture, or 0 when the filter is inactive / unavailable — in
    // which case the caller simply keeps using srcTex.
    unsigned int applyBandwidth(unsigned int srcTex, int srcW, int srcH);
};

} // namespace pom2

#endif // POM2_CRT_EFFECT_STACK_H
