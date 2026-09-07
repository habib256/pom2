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

// MainWindow_Screen — the emulated screen: uploading the framebuffer to a GL
// texture, drawing it (aspect, integer scaling, the CRT/NTSC pipelines, the
// 3D voxel view) and saving a screenshot of it.
//
// This is the one place in the frontend that touches GL directly outside the
// effect stacks. `drawScreenImage` is also where `screenHovered_` is
// captured, which the Mouse Card grab policy reads — see the note on
// `mouseGrabContext` in MainWindow_Input.cpp for why hover and not rect
// containment.

#include "MainWindow.h"

#include "Apple2Display.h"
#include "AtomicFileReplace.h"
#include "CrtEffectStack.h"
#include "EmulationController.h"
#include "IconsFontAwesome6.h"
#include "Logger.h"
#include "Memory.h"
#include "NtscPostProcessor.h"
#include "Pom2GL.h"
#include "Pom2Theme.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "Voxel3DRenderer.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

void MainWindow::saveScreenshot()
{
    // Snapshot the framebuffer under BOTH locks. stateMutex keeps the
    // renderer from resizing the buffer mid-copy; demodMutex is what makes
    // `pixels()` safe — on ColorCompositeOE it lazily runs
    // finishPendingCpuDemod() → renderCompositeOeCpu(), a ~1-2 ms pass that
    // WRITES frame80, and the AI control server's /screen.ppm handler runs
    // the identical demod on its own thread holding only demodMutex. Lock
    // order stateMutex → demodMutex, never the other way (Apple2Display.h).
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        std::lock_guard<std::mutex> demodLk(display->demodMutex());
        w = display->width();
        h = display->height();
        const uint32_t* src = display->pixels();
        pixels.assign(src, src + w * h);
    }

    // Where: `userDataDir()/screenshots`, not the process working directory.
    // F9 used to drop screenshot_NNN.ppm wherever POM2 happened to be started
    // — a read-only bundle directory in a packaged build, and someone else's
    // source tree in a dev one.
    //
    // Which name: SCAN the directory for the highest existing index instead
    // of walking a `static int` up from 0. The counter never went back down,
    // so once 1000 files existed every further capture overwrote _999
    // forever, and a fresh session with 300 existing shots re-scanned from
    // zero on every press.
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = pom2::userDataDir() / "screenshots";
    fs::create_directories(dir, ec);
    if (ec) {
        tapeStatusMessage = "Screenshot: cannot create " + dir.string();
        tapeStatusUntil   = lastFrameTime + 3.0;
        pom2::log().warn("Screenshot", tapeStatusMessage);
        return;
    }

    long next = 0;
    {
        std::error_code iterEc;
        for (const auto& entry : fs::directory_iterator(dir, iterEc)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("screenshot_", 0) != 0) continue;
            if (entry.path().extension() != ".ppm") continue;
            if (name.size() <= 11 + 4) continue;      // no digits between
            const std::string digits = name.substr(11,
                name.size() - 11 - 4);   // strip prefix and ".ppm"
            if (digits.empty() ||
                digits.find_first_not_of("0123456789") != std::string::npos)
                continue;
            const long idx = std::strtol(digits.c_str(), nullptr, 10);
            if (idx >= next) next = idx + 1;
        }
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "screenshot_%03ld.ppm", next);
    const fs::path out = dir / buf;

    // PPM "P6" — binary RGB, 1 row per scanline. Apple2Display's pixels
    // are 0xAABBGGRR (RGBA little-endian); strip alpha and swizzle to RGB.
    // Serialised into memory, then committed through the same durable
    // temp + rename path as every other POM2 write-back: a screenshot
    // interrupted mid-write used to leave a truncated .ppm behind.
    std::string ppm = "P6\n" + std::to_string(w) + " " + std::to_string(h) +
                      "\n255\n";
    const size_t header = ppm.size();
    ppm.resize(header + static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (size_t i = 0; i < pixels.size(); ++i) {
        const uint32_t p = pixels[i];
        ppm[header + i * 3 + 0] = static_cast<char>( p        & 0xFF);
        ppm[header + i * 3 + 1] = static_cast<char>((p >>  8) & 0xFF);
        ppm[header + i * 3 + 2] = static_cast<char>((p >> 16) & 0xFF);
    }
    if (!pom2::writeFileAtomic(out, ppm.data(), ppm.size(), ec)) {
        tapeStatusMessage = "Screenshot: cannot write " + out.string();
        tapeStatusUntil   = lastFrameTime + 3.0;
        pom2::log().warn("Screenshot", tapeStatusMessage);
        return;
    }

    pom2::log().info("Screenshot", "wrote " + out.string() +
                     " (" + std::to_string(w) + "x" + std::to_string(h) + ")");
    tapeStatusMessage = "Screenshot: " + out.string();
    tapeStatusUntil   = lastFrameTime + 3.0;
}

void MainWindow::uploadScreenTexture()
{
    if (screenTexture == 0) {
        glGenTextures(1, &screenTexture);
        glBindTexture(GL_TEXTURE_2D, screenTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Initial allocation matches the 280-wide buffer; the real
        // dimensions are set after the first display->render() below.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     Apple2Display::kWidth, Apple2Display::kHeight,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        screenTextureWidth  = Apple2Display::kWidth;
        screenTextureHeight = Apple2Display::kHeight;
    }

    // Mirror the live demod knobs into the display BEFORE render() so the
    // OE-CPU demod (and the OE-GPU mixed-frame CPU demod band) track the
    // CRT-Settings sliders exactly like the GPU shader — hue / sharpness /
    // PAL / textSharp used to be GPU-only and silently dead on the CPU path.
    Apple2Display::OeDemodParams dp;
    if (ntscFx) {
        const pom2::NtscParams& np = ntscFx->getParams();
        dp.hue       = np.hue;
        dp.sharpness = np.sharpness;
        dp.palMode   = np.palMode;
        dp.textSharp = np.textSharp;
    }

    // demodMutex covers render + demod + upload: the AI control server's
    // /screen handler runs the same render/demod/pixels phases on its own
    // thread, and the two used to race over frame/frame80/signalBuf with
    // no shared lock at all. stateMutex covers only render() (the
    // guest-RAM snapshot) so the CPU worker still isn't stalled by the
    // ~1-2 ms demod. Lock order: stateMutex → demodMutex, never nested
    // the other way.
    std::unique_lock<std::mutex> demodLk(display->demodMutex(),
                                         std::defer_lock);
    {
        // Render under stateMutex so we get a consistent snapshot of RAM
        // (otherwise the CPU may be mid-frame with the text screen half
        // updated, producing tearing).
        auto st = controller->lockState();
        demodLk.lock();
        // Published UNDER demodMutex, not before it: the AI control server's
        // /screen handler runs the identical OE-CPU demod on its own thread
        // and reads these knobs while it walks the scanlines, so an unlocked
        // write could change hue / textSharp between two bands of the SAME
        // captured frame. Still ahead of render(), which is what the
        // ordering above is about.
        display->setOeDemodParams(dp);
        display->render(st.memory());
    }
    display->finishPendingCpuDemod();

    const int w = display->width();
    const int h = display->height();
    glBindTexture(GL_TEXTURE_2D, screenTexture);
    if (w != screenTextureWidth || h != screenTextureHeight) {
        // 80-col toggled — reallocate. glTexImage2D releases the previous
        // storage, so we don't leak GL memory across mode switches.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, display->pixels());
        screenTextureWidth  = w;
        screenTextureHeight = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, display->pixels());
    }
}

void MainWindow::renderScreenWindow()
{
    // Default startup layout. Native keeps room for the Disk Library column;
    // WASM starts with only menu + toolbar + Apple II Screen + status bar.
    // `FirstUseEver` only applies on a fresh install — once the user
    // moves / resizes the window their imgui.ini takes over.
#ifdef __EMSCRIPTEN__
    ImGui::SetNextWindowPos (ImVec2(5,    56),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1115, 760), ImGuiCond_FirstUseEver);
#else
    ImGui::SetNextWindowPos (ImVec2(5,    90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1115, 745), ImGuiCond_FirstUseEver);
#endif

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    // `NoMove` so ImGui's default "drag-from-anywhere" doesn't eat
    // click-and-drag gestures inside the screen (Mouse Card games like
    // A2Desktop need them to reach the guest). `NoCollapse` so a
    // double-click on the title bar doesn't accidentally collapse the
    // window — a single concern at a time. We restore the title-bar
    // drag manually below.
    const ImGuiWindowFlags screenFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Apple II Screen", nullptr, screenFlags)) {
        // ── Manual title-bar drag ─────────────────────────────────────
        // Compute the title bar rect from the public Begin geometry
        // (window pos + size + frame height). When the user clicks
        // inside that strip we latch `screenDraggingByTitleBar`; on
        // every subsequent frame we apply `io.MouseDelta` until the
        // button is released. `IsAnyItemActive()` guards against
        // claiming a click that another widget already consumed (e.g.
        // any future title-bar button).
        //
        // Skipped entirely while DOCKED: a docked window has no title bar of
        // its own (it's a tab in the host node) and its position is owned by
        // the dock node. Left enabled, the rect we compute lands on the dock
        // node's tab bar and `SetWindowPos` fights the node every frame —
        // the screen jitters and the tab won't drag out.
        if (!ImGui::IsWindowDocked()) {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            const float  th = ImGui::GetFrameHeight();
            const ImVec2 m  = ImGui::GetIO().MousePos;
            const bool overTitleBar =
                (m.x >= wp.x && m.x <= wp.x + ws.x &&
                 m.y >= wp.y && m.y <= wp.y + th);
            // Do not start dragging when another foreground window covers
            // this title-bar rectangle. ImGui's normal move handling already
            // gives that front window priority; the Apple II Screen's manual
            // title drag must obey the same z-order rule.
            const bool screenWindowHovered = ImGui::IsWindowHovered();
            if (screenWindowHovered && overTitleBar &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::IsAnyItemActive()) {
                screenDraggingByTitleBar = true;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                screenDraggingByTitleBar = false;
            }
            if (screenDraggingByTitleBar) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                if (d.x != 0.0f || d.y != 0.0f) {
                    ImGui::SetWindowPos(ImVec2(wp.x + d.x, wp.y + d.y));
                }
            }
        }

        drawScreenImage();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void MainWindow::drawScreenImage()
{
    uploadScreenTexture();

    // OpenEmulator-style composite path: when the user selected
    // ColorCompositeOE AND Apple2Display produced a 14.318 MHz signal
    // this frame, lazily spin up the NTSC shader and run a single pass
    // that consumes the signal texture and writes an RGBA output we
    // hand to ImGui::Image. The first call compiles the shader; if
    // anything fails (driver lacking GL 3.x, shader compile error, …)
    // we fall back to the regular `screenTexture` for the rest of the
    // session — no crashes, no flicker, just the existing LUT view.
    unsigned int presentTex = screenTexture;
    // The 3D voxel view must sample the decoded COLOUR image, NEVER the CRT
    // glass — scanlines / shadow-mask / barrel warp would bake into the cube
    // grid. Track that tap point separately: it follows the colour pipeline
    // (NTSC demod, OE or otherwise) but stops *before* CrtEffectStack. For all
    // non-OE-GPU modes the colour image already lives in `screenTexture`; the
    // OE-GPU branch below redirects it to the demod output.
    unsigned int voxelSrcTex = screenTexture;
    // Sharp-text override: when the user wants legible text under the
    // composite mode, skip the shader for TEXT scanlines and let the
    // crisp RGB framebuffer go straight to ImGui. Full-screen text uses
    // textSharp; mixed HGR/lo-res/DHGR keeps the demod on the graphics
    // band only — the bottom 4 text rows are patched in frame80 as
    // white/black after demod (mixedCompositeUsesFramebuffer).
    // Use the soft-switch snapshot the render() above actually consumed —
    // re-polling Memory::getDisplayState() here raced the CPU worker (it may
    // have advanced past the rendered frame between the two), flashing one
    // LUT-fallback frame on a text↔graphics switch.
    //
    // Every one of these is written by Apple2Display::render(), and the AI
    // control server's /screen handler runs render() on ITS OWN thread. Read
    // one at a time outside the lock they had no consistency at all: the
    // branch below could take `lastRenderState()` from the UI's frame and
    // `signalProduced()` / `mixedCompositeUsesFramebuffer()` from the
    // capture's, and present a signal texture the flags said was not there.
    // One acquisition, all four values, then work with the locals.
    Memory::DisplayState displayState{};
    bool signalReady    = false;
    bool mixedFbPresent = false;
    Apple2Display::HiResMode hiResMode = Apple2Display::HiResMode::ColorNTSC;
    {
        std::lock_guard<std::mutex> demodLk(display->demodMutex());
        displayState   = display->lastRenderState();
        signalReady    = display->signalProduced();
        mixedFbPresent = display->mixedCompositeUsesFramebuffer();
        hiResMode      = display->getHiResMode();
    }
    // The framebuffer dimensions come from the TEXTURE we just uploaded, not
    // from a fresh display->width() — the capture thread can flip 40/80
    // columns between the upload and here, and the CRT stack would then be
    // told its source is 560 wide while `screenTexture` holds 280.
    const int fbW = screenTextureWidth;
    const int fbH = screenTextureHeight;
    const bool oeGpuMode = hiResMode == Apple2Display::HiResMode::ColorCompositeOE;
    const bool oeCpuMode = hiResMode == Apple2Display::HiResMode::ColorCompositeOECpu;
    const bool oeMode    = oeGpuMode;
    const bool oeFamily  = oeGpuMode || oeCpuMode;
    const bool wantSharpText = ntscFx && ntscFx->getParams().textSharp
                            && displayState.textMode;

    // Compute the on-screen target size up-front so the CRT effect pass can
    // render at native output resolution. That is what lets the scanline /
    // shadow-mask patterns be sampled finely enough to analytically
    // anti-alias (no barrel-warp moiré) and lets ImGui blit the result 1:1
    // (no second resample beat). Same three aspect modes used for the final
    // blit below, which reuses `avail` / `size`.
    const float W = static_cast<float>(Apple2Display::kWidth);
    const float H = static_cast<float>(Apple2Display::kHeight);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 size;
    switch (aspectMode) {
        case AspectMode::Crt43: {
            float h = std::min(avail.y, avail.x * 3.0f / 4.0f);
            h = std::max(h, H);
            size = ImVec2(h * 4.0f / 3.0f, h);
            break;
        }
        case AspectMode::Integer: {
            float s = std::max(1.0f, std::floor(std::min(avail.x / W, avail.y / H)));
            size = ImVec2(W * s, H * s);
            break;
        }
        case AspectMode::Square:
        default: {
            float s = std::max(1.0f, std::min(avail.x / W, avail.y / H));
            size = ImVec2(W * s, H * s);
            break;
        }
    }
    const int dstW = std::max(1, static_cast<int>(size.x + 0.5f));
    const int dstH = std::max(1, static_cast<int>(size.y + 0.5f));

    if (oeMode && signalReady && !wantSharpText && !mixedFbPresent) {
        if (!ntscFx) ntscFx = std::make_unique<pom2::NtscPostProcessor>();
        if (!ntscFx->available() && !ntscFx->initialize()) {
            // initialize() already logged the failure. Stop trying so
            // we don't spam the log every frame.
        }
        if (ntscFx->available()) {
            // Phase 4 final: NtscPostProcessor is now demod-only (colour
            // recovery). The CRT glass (scanlines / mask / barrel /
            // persistence / BCS) lives in the shared CrtEffectStack, so OE
            // chains into it just like every other mode — one effects
            // implementation. Run the stack ALWAYS for OE (not gated by the
            // opt-in toggle) so the look the user configured is preserved.
            unsigned int demod = 0;
            {
                // demodMutex: `signal()` hands the shader a raw pointer into
                // signalBuf, which the AI control server's /screen handler
                // rewrites via display->render() on its own thread. Without
                // the lock the upload can read a half-rewritten field (one
                // torn frame). uploadScreenTexture() released the lock
                // before returning, so this is a fresh acquisition, not a
                // nesting; scoped tight so the CRT stack below runs unlocked.
                std::lock_guard<std::mutex> demodLk(display->demodMutex());
                demod = ntscFx->process(
                    display->signal(),
                    display->signalWidth(),
                    display->signalHeight(),
                    display->signalPhaseOffset());
            }
            if (demod != 0) {
                presentTex = demod;
                voxelSrcTex = demod;   // colour image, pre-CRT-glass (3D tap)
                // CRT glass only when the master toggle is on (top of the CRT
                // Settings window); otherwise present the raw demod output.
                if (crtEffectsEnabled) {
                    if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
                    if (!crtFx->available()) crtFx->initialize();
                    if (crtFx->available()) {
                        // The OE demod shader already applied hue + chroma-
                        // bandwidth sharpness; zero hue and neutralise sharpness
                        // here so the shared stack doesn't rotate the chroma or
                        // sharpen a second time (0.5 = the stack's neutral pass).
                        pom2::NtscParams crtP = ntscFx->getParams();
                        crtP.hue       = 0.0f;
                        crtP.sharpness = 0.5f;
                        crtFx->setParams(crtP);
                        const unsigned int out = crtFx->process(
                            demod, ntscFx->outputWidth(), ntscFx->outputHeight(),
                            dstW, dstH);
                        if (out != 0) presentTex = out;
                    }
                }
            }
        }
    }

    // OE CPU demod writes frame80 → screenTexture; the OE-GPU fallbacks
    // (mixed graphics+text frame, sharp text, shader unavailable, demod
    // failure) also still present screenTexture at this point. Give ALL of
    // them the same CRT glass — gating on oeCpuMode alone made scanlines /
    // mask / persistence vanish on exactly the mixed frames (score bands,
    // menus, BASIC) and pop back on full-screen graphics, with a stale-
    // persistence ghost on re-entry. `presentTex == screenTexture` is
    // precisely "no OE branch above produced a processed texture".
    if (oeFamily && crtEffectsEnabled && presentTex == screenTexture) {
        if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
        if (!crtFx->available()) crtFx->initialize();
        if (crtFx->available()) {
            // Neutralise the demod-stage knobs, same as the GPU branch above:
            // the OE-CPU demod (and the mixed-frame CPU demod band) now
            // applies hue + chroma-bandwidth sharpness itself via
            // setOeDemodParams, so the stack must not rotate the chroma or
            // sharpen a second time. (The only frames reaching here without
            // a demod are crisp B/W text — hue is a no-op on grays — and the
            // shader-unavailable LUT fallback, a documented degraded path.)
            pom2::NtscParams crtP = ntscFx ? ntscFx->getParams() : pom2::NtscParams{};
            crtP.hue       = 0.0f;
            crtP.sharpness = 0.5f;
            crtFx->setParams(crtP);
            const unsigned int out = crtFx->process(
                presentTex, fbW, fbH, dstW, dstH);
            if (out != 0) presentTex = out;
        }
    }

    // Universal CRT effect stack (Phase 3): for every NON-OE colour mode,
    // run the framebuffer through the shared scanline / mask / barrel /
    // persistence / BCS pass so those effects work on Color NTSC, Mono,
    // Chat Mauve and AppleWin too. OE GPU and OE CPU share one CRT branch
    // (demod hue/sharpness applied by their demod stage — neutralised there).
    if (!oeFamily && crtEffectsEnabled) {
        if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
        if (!crtFx->available()) crtFx->initialize();
        if (crtFx->available()) {
            // One CRT Settings panel drives both processors: mirror the
            // NtscParams the user edits there (the demod-only knobs are
            // ignored by the effect stack).
            if (ntscFx) crtFx->setParams(ntscFx->getParams());
            const unsigned int out = crtFx->process(
                presentTex, fbW, fbH, dstW, dstH);
            if (out != 0) presentTex = out;
        }
    }

    // 3D voxel view: rebuild the decoded COLOUR image (`voxelSrcTex`, the tap
    // taken before CrtEffectStack — so the cubes never inherit scanlines / mask
    // / barrel) as an upright 4:3 slab of equal-depth cubes (MicroM8 "Voxel
    // Cube"), viewed by the orbit camera. Renders at the on-screen size so the
    // aspect is exact; replaces the flat blit (CRT glass computed above is
    // discarded when the 3D view wins). Falls back to the flat texture if the
    // GL renderer can't initialise.
    if (show(pom2::PanelId::Voxel)) {
        if (!voxel3d_) voxel3d_ = std::make_unique<pom2::Voxel3DRenderer>();
        // One voxel per live Apple II pixel (280 or 560 × 192) so the cube grid
        // captures the full image — half-res sampling visibly lost detail.
        voxel3d_->gridW = std::max(1, fbW);
        voxel3d_->gridH = std::max(1, fbH);
#if defined(__EMSCRIPTEN__)
        // Perf guard: halve the 560-wide DHGR/80-col geometry on the browser so
        // the cube count stays near the comfortable HGR 280×192 (~54k); the FBO
        // supersample is likewise capped in Voxel3DRenderer::process.
        if (voxel3d_->gridW > 280) voxel3d_->gridW = 280;
#endif
        const int vw = std::max(16, static_cast<int>(size.x));
        const int vh = std::max(16, static_cast<int>(size.y));
        const float aspect = static_cast<float>(vw) / static_cast<float>(vh);
        const unsigned int out =
            voxel3d_->process(voxelSrcTex, vw, vh, voxelCam_.viewProj(aspect));
        if (out != 0) presentTex = out;
    }

    // Scale to the content region, then centre (letterbox on a wider/taller
    // region, e.g. a kiosk viewport). `avail` / `size` were computed up-front
    // (above) so the CRT effect pass could render at this exact resolution.
    // The 280×192 active area drove a 4:3 CRT, so its pixels are not square —
    // three presentation modes:
    //   Square  — 1:1 logical pixels (280:192 ≈ 1.46); crisp; never < 1×.
    //   Crt43   — stretch the active area to a true 4:3 frame (real-monitor
    //             shape); fills the region, letterboxed.
    //   Integer — Square snapped to an integer multiple (no fractional-scale
    //             shimmer); never < 1×.
    ImVec2 cur = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(
        cur.x + std::max(0.0f, (avail.x - size.x) * 0.5f),
        cur.y + std::max(0.0f, (avail.y - size.y) * 0.5f)));

    ImGui::Image(static_cast<ImTextureID>(presentTex), size);
    // Capture the screen widget's screen-space rect so the GLFW
    // cursor-pos callback (Phase 5) can map a host position onto Apple
    // pixels. The rect answers "*where* in the screen", never "is the
    // screen the one being pointed at" — see screenHovered_ below.
    screenRectMin = ImGui::GetItemRectMin();
    screenRectMax = ImGui::GetItemRectMax();
    // ...and ImGui's own z-order aware verdict on whether the pointer is
    // actually on the screen widget, which is what decides *ownership* of a
    // click (mouseGrabContext). Unlike the rect, this is false while a
    // dropdown, popup or panel is drawn over the screen, so a click aimed at
    // an open menu no longer doubles as a click into the guest — nor as the
    // capturing press that would steal the pointer behind that menu.
    // Recomputed every frame; renderFrame clears it first so a collapsed or
    // hidden screen window cannot leave a stale `true` behind.
    const bool screenHovered = ImGui::IsItemHovered();
    screenHovered_ = screenHovered;

    // 3D voxel view camera: left-drag orbits, middle-drag strafes (pan),
    // wheel zooms (MicroM8-style). All reference the Image item above
    // (IsItemHovered), so this must stay right after it. Mutates the
    // persistent `voxelCam_` the renderer reads.
    if (show(pom2::PanelId::Voxel) && screenHovered) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            voxelCam_.azimuth   += d.x * 0.008f;
            voxelCam_.elevation += d.y * 0.008f;
            const float lim = 1.5f;   // ~86°: stay off the lookAt up-vector poles
            voxelCam_.elevation = std::clamp(voxelCam_.elevation, -lim, lim);
        }
        // Middle-drag = pan/strafe. Scale to world-units-per-pixel at the
        // target plane so the grab tracks the cursor 1:1, and grab the scene
        // (drag right → scene follows right → camera slides left).
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            const float k = 2.0f * voxelCam_.distance *
                            std::tan(voxelCam_.fovY * 0.5f) /
                            std::max(1.0f, size.y);
            voxelCam_.pan(-d.x * k, d.y * k);
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            voxelCam_.distance =
                std::clamp(voxelCam_.distance * std::pow(0.9f, wheel), 0.6f, 20.0f);
    }

}
