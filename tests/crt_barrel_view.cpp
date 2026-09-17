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

// Offscreen harness: compile the CrtEffectStack shader and render a barrel +
// scanline + shadow-mask test, to confirm the GLSL compiles and the moiré
// fix works. Writes PPMs for both barrel-on and barrel-off.
//
// It also CHECKS one property that has no ctest pin (the shader needs a GL
// context, which CI has none of): the shadow-mask pitch must be a property of
// the glass, not of the source resolution. Feeding the same picture as a
// 280-wide framebuffer (Apple2Display's legacy `frame`: 40-col text, 40-col
// HGR, lo-res) and as a 560-wide one (`frame80`: 80-col, DHGR, Chat Mauve, OE
// demod) must put the same number of triads on the screen. Exit code 3 if not.
//
// And it checks the analog RGB bandwidth pre-pass (NtscParams::
// rgbBandwidthMHz): a ~5 MHz cable must kill dot-rate detail on a 560-wide
// framebuffer (sampled at 14.318 MHz, so its Nyquist is 7.16 MHz), leave a
// 280-wide one alone (Nyquist 3.58 MHz — the cutoff is above it, so the pass
// is skipped), and never shift the brightness of a flat field. Exit code 4.
#include "CrtEffectStack.h"
#include "NtscPostProcessor.h"
#include "ProbeOutDir.h"

#include <GLFW/glfw3.h>
// Pom2GL.h, not a bare <GL/gl.h>: the latter does not exist on macOS (and is
// frozen at GL 1.1 on Windows), so this harness only ever built on Linux.
#include "Pom2GL.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <vector>

static void writePPM(const char* path, const std::vector<uint8_t>& rgba, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    // GL texture origin is bottom-left; flip vertically for PPM (top-left).
    for (int y = h - 1; y >= 0; --y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            f.put(char(p[0])); f.put(char(p[1])); f.put(char(p[2]));
        }
}

static bool loadPPM(const char* path, std::vector<uint8_t>& rgba, int& w, int& h) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string magic; f >> magic; if (magic != "P6") return false;
    int maxv; f >> w >> h >> maxv; f.get();   // single whitespace after maxv
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    f.read(reinterpret_cast<char*>(rgb.data()), rgb.size());
    rgba.assign(size_t(w) * h * 4, 255);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        rgba[i*4+0] = rgb[i*3+0]; rgba[i*4+1] = rgb[i*3+1]; rgba[i*4+2] = rgb[i*3+2];
    }
    return true;
}

// Every glass layer off: what comes out is the source, resampled 1:1 (dst ==
// src keeps sampleSrc on the plain texture() path). Isolates whatever single
// stage a check wants to look at.
static pom2::NtscParams neutralParams()
{
    pom2::NtscParams p;
    p.brightness = 0.0f; p.contrast = 1.0f; p.saturation = 1.0f; p.hue = 0.0f;
    p.sharpness = 0.5f;  p.persistence = 0.0f; p.phosphorGamma = 1.0f;
    p.scanlines = 0.0f;  p.barrel = 0.0f; p.centerLighting = 1.0f;
    p.luminanceGain = 1.0f;
    p.shadowMask = pom2::NtscParams::ShadowMask::Off;
    p.shadowMaskStrength = 0.0f;
    return p;
}

// Push `src` (sw x sh, RGBA) through the stack 1:1 with only the bandwidth
// knob engaged, and report the peak-to-peak swing of the green channel along
// a middle row (central 80%, away from the edge fade) plus that row's mean.
struct RowStats { int span; int mean; };
static RowStats bandwidthRow(pom2::CrtEffectStack& stack,
                             const std::vector<uint8_t>& src, int sw, int sh,
                             float bandwidthMHz)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, src.data());

    pom2::NtscParams p = neutralParams();
    p.rgbBandwidthMHz = bandwidthMHz;
    stack.setParams(p);
    GLuint t = stack.process(tex, sw, sh, sw, sh);
    t        = stack.process(tex, sw, sh, sw, sh);
    glDeleteTextures(1, &tex);
    if (!t) return {-1, -1};

    std::vector<uint8_t> out(size_t(sw) * sh * 4);
    glBindTexture(GL_TEXTURE_2D, t);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());

    const int y = sh / 2, x0 = sw / 10, x1 = sw - sw / 10;
    int lo = 255, hi = 0; long sum = 0;
    for (int x = x0; x < x1; ++x) {
        const int g = out[(size_t(y) * sw + x) * 4 + 1];
        lo = std::min(lo, g); hi = std::max(hi, g); sum += g;
    }
    return { hi - lo, int(sum / (x1 - x0)) };
}

// Render a flat grey field of `sw` source columns through the mask and count
// how many triads land on the screen, by counting sign changes of (R - B)
// along a middle row. Scanlines/barrel/persistence off so the only horizontal
// modulation is the mask itself; the outer 10% is skipped (edgeMask fade).
static int countTriads(pom2::CrtEffectStack& stack, int sw, int sh,
                       int dstW, int dstH)
{
    std::vector<uint8_t> flat(size_t(sw) * sh * 4, 255);
    for (size_t i = 0; i < size_t(sw) * sh; ++i) {
        flat[i*4+0] = 180; flat[i*4+1] = 180; flat[i*4+2] = 180;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, flat.data());

    pom2::NtscParams p;
    p.persistence = 0.0f;
    p.scanlines   = 0.0f;
    p.barrel      = 0.0f;
    p.shadowMask  = pom2::NtscParams::ShadowMask::ApertureGrille;
    p.shadowMaskStrength = 1.0f;
    stack.setParams(p);
    GLuint t = stack.process(tex, sw, sh, dstW, dstH);
    t        = stack.process(tex, sw, sh, dstW, dstH);
    glDeleteTextures(1, &tex);
    if (!t) return -1;

    std::vector<uint8_t> out(size_t(dstW) * dstH * 4);
    glBindTexture(GL_TEXTURE_2D, t);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());

    const int y  = dstH / 2;
    const int x0 = dstW / 10, x1 = dstW - dstW / 10;
    int changes = 0, prev = 0;
    for (int x = x0; x < x1; ++x) {
        const uint8_t* q = &out[(size_t(y) * dstW + x) * 4];
        const int d = int(q[0]) - int(q[2]);       // R - B
        const int sign = (d > 0) ? 1 : (d < 0 ? -1 : 0);
        if (sign != 0) {
            if (prev != 0 && sign != prev) ++changes;
            prev = sign;
        }
    }
    return changes;
}

int main(int argc, char** argv) {
    // No context = no test, said as a SKIP (77) — since 2026-09-17 this runs
    // under ctest, and in CI under Xvfb + Mesa's software rasteriser.
    if (!glfwInit()) { std::printf("SKIP: glfwInit failed (no display)\n"); return 77; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "crt", nullptr, nullptr);
    if (!win) { std::printf("SKIP: no GL 3.2 core context\n"); glfwTerminate(); return 77; }
    glfwMakeContextCurrent(win);

    // ── Source pattern (560×192): a bright field with colour blocks and a
    //    fine 1px checker region — exactly the kind of content whose
    //    scanlines/mask moiré under barrel curvature.
    int sw = 560, sh = 192;
    std::vector<uint8_t> src;
    if (argc > 1 && loadPPM(argv[1], src, sw, sh)) {
        std::printf("loaded source %s (%dx%d)\n", argv[1], sw, sh);
    } else {
    src.assign(size_t(sw) * sh * 4, 0);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            uint8_t r = 220, g = 220, b = 220;          // bright grey field
            if (y < 24)               { r = 230; g = 40;  b = 200; } // magenta bar (top — barrel curve)
            else if (y >= sh - 24)    { r = 40;  g = 220; b = 60;  } // green bar (bottom)
            else if (x < 40)          { r = 40;  g = 80;  b = 230; } // blue edge (left curve)
            else if (x >= sw - 40)    { r = 230; g = 180; b = 30;  } // amber edge (right)
            else if (((x ^ y) & 1))   { r = g = b = 255; }           // 1px checker → worst-case
            else                      { r = g = b = 30;  }
            uint8_t* p = &src[(size_t(y) * sw + x) * 4];
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
    }
    GLuint srcTex = 0;
    glGenTextures(1, &srcTex);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, src.data());

    pom2::CrtEffectStack stack;
    if (!stack.initialize()) {
        std::fprintf(stderr, "SHADER COMPILE FAILED: %s\n", stack.lastError().c_str());
        return 2;
    }
    std::printf("shader compiled OK\n");

    const int dstW = 840, dstH = 768;   // typical on-screen size
    std::vector<uint8_t> out(size_t(dstW) * dstH * 4);

    auto run = [&](float barrel, const char* path) {
        pom2::NtscParams p;
        p.brightness = 0.0f; p.contrast = 1.0f; p.saturation = 1.0f;
        p.persistence = 0.0f;            // isolate geometry/scanlines/mask
        p.scanlines = 0.35f;
        p.barrel = barrel;
        p.shadowMask = pom2::NtscParams::ShadowMask::ApertureGrille;
        p.shadowMaskStrength = 0.5f;
        stack.setParams(p);
        // Two frames so persistence ping-pong settles (persistence=0 anyway).
        GLuint t = stack.process(srcTex, sw, sh, dstW, dstH);
        t        = stack.process(srcTex, sw, sh, dstW, dstH);
        if (!t) { std::fprintf(stderr, "process returned 0\n"); return; }
        glBindTexture(GL_TEXTURE_2D, t);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        writePPM(path, out, dstW, dstH);
        std::printf("wrote %s (%dx%d) barrel=%.2f\n", path, dstW, dstH, barrel);
    };

    // Under the probe directory, never /tmp directly (CLAUDE.md § probes).
    const std::string outDir = pom2test::probeOutDir();
    const std::string onPath  = outDir + "/crt_barrel_on.ppm";
    const std::string offPath = outDir + "/crt_barrel_off.ppm";
    run(0.25f, onPath.c_str());
    run(0.0f,  offPath.c_str());

    // ── Mask pitch is glass, not signal ───────────────────────────────────
    const int mW = 1680, mH = 768;      // wide enough that a triad spans ~4.5px
    const int t280 = countTriads(stack, 280, 192, mW, mH);
    const int t560 = countTriads(stack, 560, 192, mW, mH);
    std::printf("mask sign-changes: src280=%d src560=%d\n", t280, t560);
    int rc = 0;
    if (t280 < 0 || t560 < 0) {
        std::fprintf(stderr, "mask pitch check: process() returned 0\n");
        rc = 3;
    } else if (t280 != t560) {
        std::fprintf(stderr,
                     "MASK PITCH TIED TO SOURCE RESOLUTION: %d vs %d "
                     "(ratio %.2f) — the mask must not change with the "
                     "video mode\n", t280, t560,
                     double(t560) / double(t280 ? t280 : 1));
        rc = 3;
    } else {
        std::printf("mask pitch OK — same triad count at 280 and 560\n");
    }

    // ── Analog RGB bandwidth: a cable, not a resolution ───────────────────
    // Vertical 1-pixel stripes = content sitting exactly at the grid's
    // Nyquist, the worst case a bandwidth limit is there to soften.
    auto stripes = [](int w, int h) {
        std::vector<uint8_t> v(size_t(w) * h * 4, 255);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t c = (x & 1) ? 235 : 20;
                uint8_t* q = &v[(size_t(y) * w + x) * 4];
                q[0] = q[1] = q[2] = c;
            }
        return v;
    };
    const auto s560 = stripes(560, 192);
    const auto s280 = stripes(280, 192);
    const RowStats off560 = bandwidthRow(stack, s560, 560, 192, 0.0f);
    const RowStats bw560  = bandwidthRow(stack, s560, 560, 192, 5.0f);
    const RowStats off280 = bandwidthRow(stack, s280, 280, 192, 0.0f);
    const RowStats bw280  = bandwidthRow(stack, s280, 280, 192, 5.0f);
    std::printf("bandwidth dot-rate swing: 560 off=%d 5MHz=%d | "
                "280 off=%d 5MHz=%d\n",
                off560.span, bw560.span, off280.span, bw280.span);

    // A flat field must come out flat and at the same level — the kernel is
    // normalised by its own weight sum precisely so dragging the slider does
    // not walk the brightness.
    std::vector<uint8_t> flat(size_t(560) * 192 * 4, 255);
    for (size_t i = 0; i < size_t(560) * 192; ++i) {
        flat[i*4+0] = 180; flat[i*4+1] = 180; flat[i*4+2] = 180;
    }
    const RowStats flatOff = bandwidthRow(stack, flat, 560, 192, 0.0f);
    const RowStats flatBw  = bandwidthRow(stack, flat, 560, 192, 5.0f);
    std::printf("bandwidth flat field: off mean=%d span=%d | "
                "5MHz mean=%d span=%d\n",
                flatOff.mean, flatOff.span, flatBw.mean, flatBw.span);

    if (off560.span < 0 || bw560.span < 0 || off280.span < 0 ||
        bw280.span < 0 || flatBw.span < 0) {
        std::fprintf(stderr, "bandwidth check: process() returned 0\n");
        rc = 4;
    } else if (bw560.span > off560.span / 4) {
        std::fprintf(stderr, "5 MHz did not band-limit the 560-wide grid: "
                             "swing %d -> %d\n", off560.span, bw560.span);
        rc = 4;
    } else if (bw280.span != off280.span) {
        std::fprintf(stderr, "5 MHz is above the 280-wide grid's Nyquist "
                             "(3.58 MHz) and must be a no-op there: "
                             "swing %d -> %d\n", off280.span, bw280.span);
        rc = 4;
    } else if (flatBw.span > 1 || std::abs(flatBw.mean - flatOff.mean) > 1) {
        std::fprintf(stderr, "bandwidth filter moved a flat field: "
                             "mean %d -> %d, span %d\n",
                     flatOff.mean, flatBw.mean, flatBw.span);
        rc = 4;
    } else {
        std::printf("bandwidth OK — kills dot-rate detail at 560, no-op at "
                    "280, flat field untouched\n");
    }

    // Informational, not asserted: the shape of the knob. Two contents on the
    // 560-wide grid — 1-px stripes (7.16 MHz, what a true 560-dot DHGR /
    // COL280 picture carries) and 2-px stripes (3.58 MHz, what 280-dot HGR
    // doubled into frame80 carries) — swept across the slider's useful range.
    // This is what "5 MHz softens DHGR but barely touches HGR" looks like in
    // numbers, and where to look when retuning the default.
    {
        std::vector<uint8_t> wide(size_t(560) * 192 * 4, 255);
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 560; ++x) {
                const uint8_t c = ((x >> 1) & 1) ? 235 : 20;
                uint8_t* q = &wide[(size_t(y) * 560 + x) * 4];
                q[0] = q[1] = q[2] = c;
            }
        std::printf("bandwidth sweep on the 560 grid (swing out of ~215):\n");
        for (float f : {3.0f, 4.0f, 5.0f, 6.0f, 7.0f}) {
            const RowStats dot = bandwidthRow(stack, s560, 560, 192, f);
            const RowStats dbl = bandwidthRow(stack, wide, 560, 192, f);
            std::printf("  %.0f MHz : 7.16 MHz content %3d | "
                        "3.58 MHz content %3d\n", double(f), dot.span, dbl.span);
        }
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    return rc;
}
