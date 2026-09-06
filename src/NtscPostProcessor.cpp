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

#include "NtscPostProcessor.h"
#include "OpenGLShader.h"
#include "Logger.h"

#include <cstring>
#include <string>

#include "Pom2GL.h"
#include <GLFW/glfw3.h>

#if POM2_GL_ES
#elif defined(__APPLE__)
#else

// Lazily-loaded GL 2.0+ entry points (Linux/Windows path). We use the
// same dynamic-loader strategy as OpenGLShader.cpp — see the comment
// there for the rationale.
namespace {
PFNGLGENFRAMEBUFFERSPROC        glGenFramebuffers_        = nullptr;
PFNGLBINDFRAMEBUFFERPROC        glBindFramebuffer_        = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC   glFramebufferTexture2D_   = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus_ = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC     glDeleteFramebuffers_     = nullptr;
PFNGLGENVERTEXARRAYSPROC        glGenVertexArrays_        = nullptr;
PFNGLBINDVERTEXARRAYPROC        glBindVertexArray_        = nullptr;
PFNGLDELETEVERTEXARRAYSPROC     glDeleteVertexArrays_     = nullptr;
PFNGLGENBUFFERSPROC             glGenBuffers_             = nullptr;
PFNGLBINDBUFFERPROC             glBindBuffer_             = nullptr;
PFNGLBUFFERDATAPROC             glBufferData_             = nullptr;
PFNGLDELETEBUFFERSPROC          glDeleteBuffers_          = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray_ = nullptr;
PFNGLVERTEXATTRIBPOINTERPROC    glVertexAttribPointer_    = nullptr;
PFNGLUSEPROGRAMPROC             glUseProgram_             = nullptr;
PFNGLGETUNIFORMLOCATIONPROC     glGetUniformLocation_     = nullptr;
PFNGLUNIFORM1IPROC              glUniform1i_              = nullptr;
PFNGLUNIFORM1FPROC              glUniform1f_              = nullptr;
PFNGLUNIFORM2FPROC              glUniform2f_              = nullptr;
PFNGLACTIVETEXTUREPROC          glActiveTexture_          = nullptr;
bool entryPointsLoaded_ = false;
bool loadEntryPoints()
{
    if (entryPointsLoaded_) return true;
    auto get = [](const char* n) {
        return reinterpret_cast<void*>(glfwGetProcAddress(n));
    };
#define LOAD(t, v, n) v = reinterpret_cast<t>(get(n))
    LOAD(PFNGLGENFRAMEBUFFERSPROC,        glGenFramebuffers_,        "glGenFramebuffers");
    LOAD(PFNGLBINDFRAMEBUFFERPROC,        glBindFramebuffer_,        "glBindFramebuffer");
    LOAD(PFNGLFRAMEBUFFERTEXTURE2DPROC,   glFramebufferTexture2D_,   "glFramebufferTexture2D");
    LOAD(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus_, "glCheckFramebufferStatus");
    LOAD(PFNGLDELETEFRAMEBUFFERSPROC,     glDeleteFramebuffers_,     "glDeleteFramebuffers");
    LOAD(PFNGLGENVERTEXARRAYSPROC,        glGenVertexArrays_,        "glGenVertexArrays");
    LOAD(PFNGLBINDVERTEXARRAYPROC,        glBindVertexArray_,        "glBindVertexArray");
    LOAD(PFNGLDELETEVERTEXARRAYSPROC,     glDeleteVertexArrays_,     "glDeleteVertexArrays");
    LOAD(PFNGLGENBUFFERSPROC,             glGenBuffers_,             "glGenBuffers");
    LOAD(PFNGLBINDBUFFERPROC,             glBindBuffer_,             "glBindBuffer");
    LOAD(PFNGLBUFFERDATAPROC,             glBufferData_,             "glBufferData");
    LOAD(PFNGLDELETEBUFFERSPROC,          glDeleteBuffers_,          "glDeleteBuffers");
    LOAD(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray_, "glEnableVertexAttribArray");
    LOAD(PFNGLVERTEXATTRIBPOINTERPROC,    glVertexAttribPointer_,    "glVertexAttribPointer");
    LOAD(PFNGLUSEPROGRAMPROC,             glUseProgram_,             "glUseProgram");
    LOAD(PFNGLGETUNIFORMLOCATIONPROC,     glGetUniformLocation_,     "glGetUniformLocation");
    LOAD(PFNGLUNIFORM1IPROC,              glUniform1i_,              "glUniform1i");
    LOAD(PFNGLUNIFORM1FPROC,              glUniform1f_,              "glUniform1f");
    LOAD(PFNGLUNIFORM2FPROC,              glUniform2f_,              "glUniform2f");
    LOAD(PFNGLACTIVETEXTUREPROC,          glActiveTexture_,          "glActiveTexture");
#undef LOAD
    entryPointsLoaded_ =
        glGenFramebuffers_ && glBindFramebuffer_ && glFramebufferTexture2D_ &&
        glCheckFramebufferStatus_ && glDeleteFramebuffers_ &&
        glGenVertexArrays_ && glBindVertexArray_ && glDeleteVertexArrays_ &&
        glGenBuffers_ && glBindBuffer_ && glBufferData_ && glDeleteBuffers_ &&
        glEnableVertexAttribArray_ && glVertexAttribPointer_ &&
        glUseProgram_ && glGetUniformLocation_ &&
        glUniform1i_ && glUniform1f_ && glUniform2f_ && glActiveTexture_;
    return entryPointsLoaded_;
}
} // namespace
#  define glGenFramebuffers        glGenFramebuffers_
#  define glBindFramebuffer        glBindFramebuffer_
#  define glFramebufferTexture2D   glFramebufferTexture2D_
#  define glCheckFramebufferStatus glCheckFramebufferStatus_
#  define glDeleteFramebuffers     glDeleteFramebuffers_
#  define glGenVertexArrays        glGenVertexArrays_
#  define glBindVertexArray        glBindVertexArray_
#  define glDeleteVertexArrays     glDeleteVertexArrays_
#  define glGenBuffers             glGenBuffers_
#  define glBindBuffer             glBindBuffer_
#  define glBufferData             glBufferData_
#  define glDeleteBuffers          glDeleteBuffers_
#  define glEnableVertexAttribArray glEnableVertexAttribArray_
#  define glVertexAttribPointer    glVertexAttribPointer_
#  define glUseProgram             glUseProgram_
#  define glGetUniformLocation     glGetUniformLocation_
#  define glUniform1i              glUniform1i_
#  define glUniform1f              glUniform1f_
#  define glUniform2f              glUniform2f_
#  define glActiveTexture          glActiveTexture_
#endif

namespace pom2 {

#if POM2_GL_ES || defined(__APPLE__)
namespace { [[maybe_unused]] bool loadEntryPoints() { return true; } }
#endif

namespace {

// Fullscreen-quad vertex shader: passes UV in [0..1] to the fragment.
const char* kVertexShader = R"GLSL(
in vec2 aPos;
out vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// Fragment shader: NTSC composite DEMODULATION only (Phase 4 final).
//
// This pass now recovers colour from the composite signal and nothing
// else — barrel geometry, brightness/contrast/saturation, phosphor
// persistence, scanlines and the shadow mask all moved to the shared
// CrtEffectStack so OE and every other colour mode go through ONE effects
// implementation. MainWindow chains this demod into CrtEffectStack.
//
// Pipeline per output fragment:
//   1. Sample N taps around the current column of the signal texture.
//      Y is a narrow-gaussian sum (sharp luma); chroma multiplies each
//      tap by sin/cos of the 4×fsc subcarrier phase (π/2 per dot — the
//      Apple II pixel clock IS the colorburst) under a WIDER gaussian
//      whose width the sharpness slider sets (chroma bandwidth).
//   2. Hue rotation in the I/Q plane, then the standard NTSC YIQ→RGB
//      matrix. PAL line-phase alternation flips the Q sign on odd lines.
//   3. Output the demodulated RGB (clamped). Grading + CRT glass happen
//      downstream in CrtEffectStack. Output is 1× (no scanline doubling).
//
// PARITY CONTRACT: this demod exists in three copies — this GLSL, the CPU
// twin (Apple2Display::renderCompositeOeCpu) and the C++ re-simulation in
// tests/oe_demod_gpu_cpu_parity_test.cpp. Any edit here (kernels, phase,
// hue/PAL/sharpness handling, matrix) must be mirrored in BOTH and re-pinned
// by that test.
const char* kFragmentShader = R"GLSL(
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uSignal;
uniform vec2  uSignalSize;     // (width, height) of the signal texture
uniform float uHue;
uniform float uSharpness;
uniform int   uPalMode;        // 0=NTSC, 1=PAL (line-phase alternation)
uniform int   uPhaseOffset;  // 0=HGR/text, 1=DHGR (MAME rotl4 absX+1)

const float PI = 3.14159265358979;

float sampleSignal(float x, float y)
{
    if (x < 0.0 || x >= uSignalSize.x || y < 0.0 || y >= uSignalSize.y) return 0.0;
    return texture(uSignal, vec2(x / uSignalSize.x, y / uSignalSize.y)).r;
}

void main()
{
    // Fragment centres land at px+0.5, so vUv.x*size = px+0.5: sampling
    // texture((px+0.5+i)/size) hits the CENTRE of texel px+i (correct
    // NEAREST sampling). But the subcarrier PHASE must be referenced to the
    // integer sample index px+i, not px+0.5+i — otherwise it carries a half-
    // sample = 45° offset that rotates the artifact wheel (blue/orange wrong).
    // So: keep sigX for sampling, floor(fx) for the phase. The CPU demod uses
    // integer x and is correct; this aligns the GPU shader to it.
    float sigX = vUv.x * uSignalSize.x;
    float sigY = vUv.y * uSignalSize.y;

    // ── PAL line-phase alternation ────────────────────────────────
    float palQSign = 1.0;
    if (uPalMode == 1) {
        float lineIdx = floor(sigY);
        palQSign = (mod(lineIdx, 2.0) < 1.0) ? 1.0 : -1.0;
    }

    // ── Y / U / V accumulation over a small kernel ────────────────
    // OpenEmulator-faithful decode (libemulation OpenGLCanvas.cpp):
    // chroma = composite·(sin φ, cos φ) → U,V, then a YUV→RGB decoder
    // matrix. The earlier code demodulated the same sin/cos but fed a YIQ
    // matrix (axes rotated ~33° from YUV) — that mis-rotated the wheel:
    // it could be coaxed to get green/violet right via a phase hack but
    // never blue/orange (blue rendered orange). Probe-calibrated against
    // the MAME LUT: with the YUV matrix the correct phase offset is 0.
    // OpenEmulator-exact 17-tap FIR kernels: a Dolph-Chebyshev(50 dB) window ×
    // sinc lowpass, built with libemulation's own realIDFT recipe (OEVector.cpp
    // chebyshevWindow/lanczosWindow + OpenGLCanvas.cpp) at the AppleColor
    // Composite Monitor IIe config — luma 2.0 MHz, chroma 0.6 MHz, Y'UV. Coeffs
    // are symmetric, [0]=centre … [8]=edge.
    //   lumaK  : sum 1, NOTCHES the fs/4 colour subcarrier (|H(0.25)| ≈ 0.002,
    //            -3 dB ≈ 1.64 MHz) — kills the dot-crawl the old gaussian
    //            (sigmaY=0.8, |H(0.25)| ≈ 0.46) produced.
    //   chroma : sum 2 (the ×2 demod gain). Sharpness is neutral at 0.5,
    //            matching the CPU path / OE-faithful 0.6 MHz kernel. Only
    //            the upper half of the slider blends toward a sharper 2.0 MHz
    //            kernel; using that kernel at the default caused hue-ringed
    //            edges (green/orange, blue/violet swaps) while solid fills
    //            stayed correct.
    float lumaK[9] = float[](0.27941, 0.23593, 0.13462, 0.03665, -0.01538,
                             -0.02210, -0.00999, -0.00072, 0.00130);
    float chromaSoft[9] = float[](0.26030, 0.24788, 0.21373, 0.16602, 0.11509,
                                  0.07008, 0.03648, 0.01543, 0.00515);
    float chromaSharp[9] = float[](0.55882, 0.47185, 0.26923, 0.07331, -0.03077,
                                   -0.04421, -0.01999, -0.00144, 0.00259);
    float sharp = clamp((uSharpness - 0.5) * 2.0, 0.0, 1.0);

    const int N = 8;
    float Y = 0.0, U = 0.0, V = 0.0;
    for (int i = -N; i <= N; ++i) {
        float fx = sigX + float(i);
        float s  = sampleSignal(fx, sigY);
        int   a  = i < 0 ? -i : i;
        float wc = mix(chromaSoft[a], chromaSharp[a], sharp);
        // Subcarrier phase: π/2·((floor(fx) + po) & 3) — the offset applied
        // exactly ONCE, matching MAME rotl4(absX+1) and the AppleWin LUT
        // (AppleWinNtsc.cpp renderLine `lut[(x + phase) & 3]`). History
        // note: this single application was the original GPU formula; the
        // CPU demod (renderCompositeOeCpu) used to apply the offset twice
        // (in its sin/cos table AND its index), and a previous "fix"
        // wrongly concluded the GPU diverged +90° and doubled it here too.
        // The CPU was the wrong one — both now apply it once.
        int phaseIdx = (int(floor(fx)) + uPhaseOffset) & 3;
        float phase = PI * 0.5 * float(phaseIdx);
        Y += s * lumaK[a];                     // FIR luma (sum=1, notches fs/4)
        U += s * sin(phase) * wc;              // FIR chroma (sum=2 → ×2 gain)
        V += s * cos(phase) * wc * palQSign;
    }

    // ── Optional hue rotation in the U/V plane (user knob) ─────────
    float h = uHue * PI;
    float cs = cos(h), sn = sin(h);
    float Ur = U * cs - V * sn;
    float Vr = U * sn + V * cs;

    // ── YUV → RGB (OpenEmulator decoder matrix) ───────────────────
    vec3 rgb = vec3(
        Y                 + 1.139883 * Vr,
        Y - 0.394642 * Ur - 0.580622 * Vr,
        Y + 2.032062 * Ur
    );
    rgb = clamp(rgb, 0.0, 1.0);
    fragColor = vec4(rgb, 1.0);
}
)GLSL";

} // namespace

NtscPostProcessor::NtscPostProcessor() = default;
NtscPostProcessor::~NtscPostProcessor() { destroyGL(); }

bool NtscPostProcessor::initialize()
{
    if (initialized) return ready;
    initialized = true;

#if !POM2_GL_ES && !defined(__APPLE__)
    if (!loadEntryPoints()) {
        errorMsg = "GL 3.x entry points unavailable";
        return false;
    }
#endif

    {
        GLint mts = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mts);
        maxTexSize_ = (mts > 0) ? static_cast<int>(mts) : 2048;
    }

    program = compileShaderProgram(kVertexShader, kFragmentShader, &errorMsg);
    if (!program) return false;

    uSignal      = glGetUniformLocation(program, "uSignal");
    uSignalSize  = glGetUniformLocation(program, "uSignalSize");
    uHue         = glGetUniformLocation(program, "uHue");
    uSharpness   = glGetUniformLocation(program, "uSharpness");
    uPalMode     = glGetUniformLocation(program, "uPalMode");
    uPhaseOffset = glGetUniformLocation(program, "uPhaseOffset");

    // Fullscreen quad: two triangles covering NDC [-1..1].
    const float verts[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
        -1.0f,  1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
    };
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    ready = true;
    pom2::log().info("NTSC", "OpenEmulator-style composite shader ready");
    return true;
}

bool NtscPostProcessor::createTextures(int sw, int sh)
{
    // Clamp to what this GL can allocate: past GL_MAX_TEXTURE_SIZE the
    // allocation fails with GL_INVALID_VALUE, the FBO goes incomplete and the
    // demod silently stops (the signal is 560 wide today, so this is a guard,
    // not a workaround).
    sw = clampTexDim(sw);
    sh = clampTexDim(sh);
    signalW = sw;
    signalH = sh;
    outW    = sw;          // keep horizontal sample rate
    outH    = sh;          // demod-only is 1×; CrtEffectStack does the 2×

    // Signal texture (R8) — one byte per 4×fsc sample.
    glGenTextures(1, &signalTex);
    glBindTexture(GL_TEXTURE_2D, signalTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#if POM2_GL_ES || defined(__APPLE__)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, sw, sh, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, sw, sh, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
#endif

    // Single demod output texture + FBO. NEAREST: this is the demod
    // intermediate that CrtEffectStack samples; nearest keeps its texels
    // clean (the effect pass does its own filtering / 2× scanline expansion).
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &outputTex);
    glBindTexture(GL_TEXTURE_2D, outputTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, outputTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        errorMsg = "FBO incomplete";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &outputTex);
        glDeleteTextures(1, &signalTex);
        fbo = 0; outputTex = 0; signalTex = 0;
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void NtscPostProcessor::destroyGL()
{
    // We don't call glDelete* in WASM/dtor — the context is being torn
    // down anyway and unloading isn't guaranteed to be GL-context-safe.
    // (No leak in practice: the context owns the resources.)
    ready = false;
}

unsigned int NtscPostProcessor::process(const uint8_t* signal,
                                        int sw, int sh,
                                        int phaseOffset)
{
    if (!ready) return 0;

    // Lazy texture creation: we need the signal dimensions before we
    // can allocate the FBOs, so the first call sizes everything up.
    if (signalTex == 0) {
        if (!createTextures(sw, sh)) {
            pom2::log().warn("NTSC",
                "cannot allocate the " + std::to_string(sw) + "x" +
                std::to_string(sh) + " demod target (" + errorMsg +
                ") — composite shader disabled");
            ready = false;
            return 0;
        }
    } else if (clampTexDim(sw) != signalW || clampTexDim(sh) != signalH) {
        // Reallocate on dimension change (e.g. someone wired this up
        // for a hypothetical 80-col-only Apple II). Cheap; not on the
        // per-frame path in practice. Clamped and re-checked for the same
        // reason createTextures clamps: a resize past the driver's limit
        // leaves the attachment invalid, and demodulating into a broken FBO
        // shows as a black screen with nothing in the log.
        signalW = clampTexDim(sw);
        signalH = clampTexDim(sh);
        outW = signalW;
        outH = signalH;
        glBindTexture(GL_TEXTURE_2D, signalTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, signalW, signalH, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, outputTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, outW, outH, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        const bool complete =
            glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!complete) {
            errorMsg = "FBO incomplete after resize";
            pom2::log().warn("NTSC",
                "resize to " + std::to_string(outW) + "x" + std::to_string(outH) +
                " left the FBO incomplete (GL_MAX_TEXTURE_SIZE " +
                std::to_string(maxTexSize_) + ") — composite shader disabled");
            ready = false;
            return 0;
        }
    }
    // The signal rows the shader samples are the ones the texture holds.
    sw = signalW;
    sh = signalH;

    // Upload the new signal frame. GL_UNPACK_ALIGNMENT is context-global and
    // ImGui's own uploads assume the default 4 — leaving it at 1 made every
    // later texture upload in the frame use a stride this pass chose.
    GLint prevAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glBindTexture(GL_TEXTURE_2D, signalTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh,
                    GL_RED, GL_UNSIGNED_BYTE, signal);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);

    // Save current FBO + viewport + enables so we don't disturb ImGui.
    int prevFbo = 0;
    int prevViewport[4] = {0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    const GLboolean prevDepth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prevCull  = glIsEnabled(GL_CULL_FACE);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, outW, outH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, signalTex);
    glUniform1i(uSignal, 0);

    // Demod-only uniforms (CRT glass lives in CrtEffectStack now).
    if (uSignalSize >= 0) glUniform2f(uSignalSize, float(sw), float(sh));
    if (uHue        >= 0) glUniform1f(uHue,       params.hue);
    if (uSharpness  >= 0) glUniform1f(uSharpness, params.sharpness);
    if (uPalMode       >= 0) glUniform1i(uPalMode,       params.palMode ? 1 : 0);
    if (uPhaseOffset   >= 0) glUniform1i(uPhaseOffset,   phaseOffset & 3);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Leave no private texture bound on unit 0.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Restore previous FBO + viewport + enables.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<unsigned int>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1],
               prevViewport[2], prevViewport[3]);
    if (prevBlend) glEnable(GL_BLEND);      else glDisable(GL_BLEND);
    if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prevCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    return outputTex;
}

} // namespace pom2
