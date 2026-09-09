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

// Pins two properties of the CrtEffectStack glass pass that only exist on the
// way to the WINDOW — no headless render reaches them, and both are invisible
// in /screen.ppm because that capture is taken before this stage.
//
// 1. HORIZONTAL MINIFICATION MUST LOW-PASS. MainWindow derives the on-screen
//    target from the 280-dot geometry (`size.x = 280 * s`), so a 560-wide
//    framebuffer (DHGR, 80-col, Chat Mauve, the OE demod output) is minified
//    horizontally by s/2 whenever the screen widget is narrower than 560
//    physical pixels — while the vertical axis still magnifies by s. Neither
//    branch of the shader's sampleSrc() filtered that: the `mag <= 1.25`
//    shortcut point-samples, and above it Catmull-Rom is a reconstruction
//    filter, not a decimation one. A flat 1-on/1-off 560-dot grid — which
//    must present as a flat mid-grey — came out swinging the full 0..255,
//    i.e. scattered white dots and dotted lines, with single-dot vertical
//    detail dropping out entirely.
//
// 2. THE FIRST FRAME AFTER A (RE)ALLOCATION HAS NO PHOSPHOR HISTORY. The
//    ping-pong output textures are freshly allocated there, so the pass binds
//    the SOURCE to uPrev instead — un-warped, un-glassed, full brightness —
//    and `max(rgb, prev * persistence)` then re-lights everything the glass
//    darkened, notably the black border outside the barrel-warped picture.
//    A window drag makes every frame a reallocation frame, so what should be
//    a one-frame flash is a permanent bright halo around the tube while the
//    user drags.
//
// Needs a real GL context; exits 77 (ctest SKIP) where none can be made.

#include "CrtEffectStack.h"
#include "NtscPostProcessor.h"
#include "Pom2GL.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

GLuint gTex = 0;

void uploadSrc(const std::vector<uint8_t>& rgba, int w, int h)
{
    if (gTex == 0) {
        glGenTextures(1, &gTex);
        glBindTexture(GL_TEXTURE_2D, gTex);
        // NEAREST, exactly like MainWindow::uploadScreenTexture().
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, gTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

std::vector<uint8_t> readTex(GLuint t, int w, int h)
{
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    glBindTexture(GL_TEXTURE_2D, t);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    return out;
}

struct Span { int lo, hi; };
Span spanOf(const std::vector<uint8_t>& rgba, int w, int /*h*/,
            int x0, int y0, int x1, int y1)
{
    int lo = 255, hi = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            const int v = std::max({int(p[0]), int(p[1]), int(p[2])});
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
    return {lo, hi};
}

pom2::NtscParams neutral()
{
    pom2::NtscParams p;
    p.brightness = 0.0f; p.contrast = 1.0f; p.saturation = 1.0f; p.hue = 0.0f;
    p.sharpness = 0.5f;  p.persistence = 0.0f; p.phosphorGamma = 1.0f;
    p.scanlines = 0.0f;  p.barrel = 0.0f; p.centerLighting = 1.0f;
    p.luminanceGain = 1.0f; p.rgbBandwidthMHz = 0.0f;
    p.shadowMask = pom2::NtscParams::ShadowMask::Off;
    p.shadowMaskStrength = 0.0f;
    return p;
}

// A 560-wide 1-on/1-off vertical dot grid: the finest thing a 560-dot mode
// can put on the wire, and a flat mid-grey once band-limited.
std::vector<uint8_t> dotGrid560()
{
    std::vector<uint8_t> v(static_cast<size_t>(560) * 192 * 4, 255);
    for (int y = 0; y < 192; ++y)
        for (int x = 0; x < 560; ++x) {
            const uint8_t c = (x & 1) ? 255 : 0;
            const size_t i = (static_cast<size_t>(y) * 560 + x) * 4;
            v[i+0] = c; v[i+1] = c; v[i+2] = c; v[i+3] = 255;
        }
    return v;
}

// ── 1. minification must low-pass ────────────────────────────────────────
// 307x230 is what AspectMode::Crt43 produces in a short content region
// (magX = 0.55 against a 560-wide source, magY = 1.20); 350x240 is a small
// docked panel; 420x288 is the Square s = 1.5 case, which clears the
// `mag <= 1.25` shortcut, takes the Catmull-Rom branch — and aliased just as
// badly, because Catmull-Rom reconstructs, it does not decimate.
//
// The bound is the physics, not a magic number. One output pixel covers
// w = 560/dstW source texels; over a period-2 square wave an exact BOX filter
// of width w (1 < w < 2) still leaves a residual swing of (2-w)/w — 0.095 at
// w = 1.82, but 0.50 at w = 1.33, where 1.33 texels simply cannot average a
// 2-texel period away. So each case is checked against its own analytic
// ceiling plus 15 % + 8 codes of driver headroom. The shipped code swings the
// full 0..255 at every one of them.
bool checkMinify(pom2::CrtEffectStack& fx)
{
    fx.setParams(neutral());
    const auto src = dotGrid560();
    struct D { int w, h; const char* tag; } ds[] = {
        {307, 230, "Crt43 short   (plain-texture branch)"},
        {350, 240, "small dock    (plain-texture branch)"},
        {420, 288, "Square s=1.5  (Catmull-Rom branch)"},
    };
    bool ok = true;
    for (const D& d : ds) {
        uploadSrc(src, 560, 192);
        GLuint out = 0;
        for (int i = 0; i < 3; ++i)              // past firstFrame
            out = fx.process(gTex, 560, 192, d.w, d.h);
        if (out == 0) { std::printf("FAIL process() returned 0\n"); return false; }
        const auto img = readTex(out, d.w, d.h);
        const Span s = spanOf(img, d.w, d.h, 10, 20, d.w - 10, d.h - 20);
        const int span = s.hi - s.lo;
        const double texelsPerOutPx = 560.0 / d.w;
        const int    ceiling = 8 + static_cast<int>(
            255.0 * (2.0 - texelsPerOutPx) / texelsPerOutPx * 1.15);
        std::printf("  %-38s %4dx%-4d span=%3d (lo=%3d hi=%3d) ceiling=%3d\n",
                    d.tag, d.w, d.h, span, s.lo, s.hi, ceiling);
        if (span > ceiling) {
            std::printf("FAIL a flat 560-dot grid must not swing more than an "
                        "exact box filter would; span %d > %d\n", span, ceiling);
            ok = false;
        }
    }
    return ok;
}

// ── 2. no phosphor history on a (re)allocation frame ─────────────────────
// The barrel border is BLACK: edgeMask fades to 0 outside the warped picture.
// It must stay black on the frame that reallocates the ping-pong pair, and on
// every frame of a drag that reallocates it again and again.
bool checkFirstFramePersistence(pom2::CrtEffectStack& fx)
{
    pom2::NtscParams p = neutral();
    p.persistence = 0.4f;    // ntsc_persistence, as shipped
    p.barrel      = 0.014f;  // ntsc_barrel, as shipped
    p.scanlines   = 0.25f;
    fx.setParams(p);

    std::vector<uint8_t> white(static_cast<size_t>(560) * 192 * 4, 255);
    int dstW = 840, dstH = 576;
    GLuint out = 0;
    for (int f = 0; f < 6; ++f) {
        uploadSrc(white, 560, 192);
        out = fx.process(gTex, 560, 192, dstW, dstH);
    }
    Span s = spanOf(readTex(out, dstW, dstH), dstW, dstH, 0, 0, 6, 6);
    std::printf("  steady          corner max=%3d\n", s.hi);
    if (s.hi > 8) {
        std::printf("FAIL the barrel border is not black even in steady "
                    "state (max %d) — the rest of this check is meaningless\n",
                    s.hi);
        return false;
    }

    bool ok = true;
    for (int f = 0; f < 4; ++f) {           // a drag: resize every frame
        ++dstW; ++dstH;
        uploadSrc(white, 560, 192);
        out = fx.process(gTex, 560, 192, dstW, dstH);
        s = spanOf(readTex(out, dstW, dstH), dstW, dstH, 0, 0, 6, 6);
        std::printf("  resize %dx%-4d corner max=%3d\n", dstW, dstH, s.hi);
        if (s.hi > 8) {
            std::printf("FAIL a reallocation frame lit the barrel border to "
                        "%d/255 — the persistence blend fed on the raw source "
                        "because there is no previous output to decay\n", s.hi);
            ok = false;
        }
    }
    return ok;
}

} // namespace

int main()
{
    if (!glfwInit()) {
        std::printf("SKIP: glfwInit failed (no display)\n");
        return 77;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "crt_glass_resample",
                                       nullptr, nullptr);
    if (!win) {
        std::printf("SKIP: no GL 3.2 context available\n");
        glfwTerminate();
        return 77;
    }
    glfwMakeContextCurrent(win);

    pom2::CrtEffectStack fx;
    if (!fx.initialize()) {
        std::printf("SKIP: CrtEffectStack unavailable (%s)\n",
                    fx.lastError().c_str());
        glfwDestroyWindow(win);
        glfwTerminate();
        return 77;
    }

    std::printf("[1] horizontal minification must low-pass\n");
    const bool a = checkMinify(fx);
    std::printf("[2] no phosphor history on a (re)allocation frame\n");
    const bool b = checkFirstFramePersistence(fx);

    glfwDestroyWindow(win);
    glfwTerminate();
    if (!a || !b) { std::printf("crt_glass_resample: FAILED\n"); return 1; }
    std::printf("crt_glass_resample: OK\n");
    return 0;
}
