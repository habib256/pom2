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

#include "CliDispatcher.h"
#include "Settings.h"
#include "TnfsMedia.h"
#include "Pom2Build.h"
#include "IconsFontAwesome6.h"
#include "Logger.h"
#include "ThreadGuard.h"
#include "MainWindow.h"
#include "Pom2Theme.h"
#include "Version.h"
// MainWindow.h now forward-declares EmulationController and Apple2Display
// to keep its include cone lean. main.cpp dereferences both via
// MainWindow::emul() / displayRef() so it needs the full types.
#include "Apple2Display.h"
#include "EmulationController.h"
#include "CassetteDevice.h"
#include "ChildProcess.h"
#include "Disk35Image.h"
#include "PersistentFs.h"
#include "ResourcePaths.h"
#include "SystemProfile.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <thread>

#ifndef __EMSCRIPTEN__
namespace {
volatile std::sig_atomic_t gShutdownRequested = 0;
void requestShutdown(int) { gShutdownRequested = 1; }
}
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>   // emscripten_set_wheel_callback (browser zoom)
#endif

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static void glfw_char_callback(GLFWwindow* w, unsigned int codepoint)
{
    ImGui_ImplGlfw_CharCallback(w, codepoint);
    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        // Skip the codepoint when ImGui is capturing keyboard (a text
        // input field, etc.). Otherwise the Apple II eats every keystroke
        // even when the user is editing a control widget.
        if (!ImGui::GetIO().WantCaptureKeyboard) mw->onChar(codepoint);
    }
}

static void glfw_key_callback(GLFWwindow* w, int key, int sc, int action, int mods)
{
    ImGui_ImplGlfw_KeyCallback(w, key, sc, action, mods);

    // Alt-F4 = quit, handled by POM2 itself rather than relying on the
    // window manager. This matters in exclusive full-screen kiosk, which
    // has no menu / toolbar / close button and where some WMs don't
    // intercept the combo. Requesting close here feeds the normal
    // clean-shutdown path (the main loop watches glfwWindowShouldClose),
    // so pending saves / tape dumps still run. Routed before the
    // MainWindow forward and independent of ImGui keyboard capture.
    if (key == GLFW_KEY_F4 && action == GLFW_PRESS && (mods & GLFW_MOD_ALT)) {
        glfwSetWindowShouldClose(w, GLFW_TRUE);
        return;
    }

    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        // Caps-lock is reported in `mods` on every key event (we asked for
        // GLFW_LOCK_KEY_MODS in main), so latch it here — before the
        // ImGui-capture gate below, which would otherwise miss the toggle
        // whenever a text field has focus. The status bar surfaces it: a
        // stuck caps-lock is a classic "why won't this game take my input"
        // trap on the Apple II.
        mw->setHostCapsLock((mods & GLFW_MOD_CAPS_LOCK) != 0);

        // F11 (soft reset) and F12 (hard reset) are routed unconditionally
        // so the user can recover even when an ImGui widget has captured
        // the keyboard focus. F9 (screenshot) is routed the same way so
        // a screenshot can be triggered from a focused control widget.
        // Left/Right Alt = Open-Apple/Solid-Apple — routed unconditionally
        // so the IIe/IIc/IIc+ firmware can observe consistent press/release
        // edges via $C061/$C062 regardless of ImGui focus state.
        // Ctrl+Shift+P (command palette) joins the unconditional set so the
        // palette is reachable even from a focused text field — same rationale
        // as F11/F12: the user must always have a way out.
        // F10 and Ctrl+Alt+F (GUI ↔ kiosk — two bindings for one action,
        // because F10 is swallowed by the window manager on several
        // desktops) join the unconditional set for the same reason as
        // F11/F12: entering kiosk from a focused widget must work, and
        // leaving it must ALWAYS work.
        // Ctrl+Alt+G (release the captured mouse) is unconditional for the
        // strongest form of that reason: while the pointer is captured the
        // user cannot click their way to any other control.
        const bool isGlobalKey = (key == GLFW_KEY_F11 || key == GLFW_KEY_F12 ||
                                  key == GLFW_KEY_F9 || key == GLFW_KEY_F10 ||
                                  key == GLFW_KEY_LEFT_ALT ||
                                  key == GLFW_KEY_RIGHT_ALT ||
                                  (key == GLFW_KEY_P &&
                                   (mods & GLFW_MOD_CONTROL) &&
                                   (mods & GLFW_MOD_SHIFT)) ||
                                  (key == GLFW_KEY_G &&
                                   (mods & GLFW_MOD_CONTROL) &&
                                   (mods & GLFW_MOD_ALT)) ||
                                  (key == GLFW_KEY_F &&
                                   (mods & GLFW_MOD_CONTROL) &&
                                   (mods & GLFW_MOD_ALT)));
        if (!ImGui::GetIO().WantCaptureKeyboard || isGlobalKey) {
            mw->onKey(key, sc, action, mods);
        }
    }
}

static void glfw_cursor_pos_callback(GLFWwindow* w, double x, double y)
{
    // Forward to ImGui first so it tracks the cursor for hover detection.
    ImGui_ImplGlfw_CursorPosCallback(w, x, y);
    // Then route to the Apple II Mouse Card. MainWindow gates on
    // whether the cursor is inside the Apple II Screen widget rect —
    // outside, this is a no-op and ImGui handles the cursor normally.
    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        mw->onMouseMove(x, y);
    }
}

static void glfw_mouse_button_callback(GLFWwindow* w, int button, int action, int mods)
{
    ImGui_ImplGlfw_MouseButtonCallback(w, button, action, mods);
    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        // Always forward to MainWindow — it does its own
        // cursor-in-screen-rect check (same gate as the cursor-position
        // callback). The `WantCaptureMouse` gate is too coarse: the
        // Apple II Screen is itself an ImGui window, so clicks inside
        // the screen widget would otherwise be swallowed by ImGui.
        mw->onMouseButton(button, action);
    }
}

static void glfw_window_focus_callback(GLFWwindow* w, int focused)
{
    // Chain ImGui's own handler first (same pattern as the key / cursor /
    // button callbacks above): installing ours replaces the one
    // ImGui_ImplGlfw_InitForOpenGL registered.
    ImGui_ImplGlfw_WindowFocusCallback(w, focused);
    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        // Drop a Mouse Card pointer capture on the way out: a grab that
        // survived Alt-Tab would keep eating the desktop's pointer with no
        // visible owner and no window to press Ctrl+Alt+G in.
        mw->onWindowFocus(focused != 0);
    }
}

static void glfw_drop_callback(GLFWwindow* w, int count, const char** paths)
{
    // Honour the README's "drop a .woz/.dsk on the window" promise: route
    // the dropped image(s) through MainWindow, which classifies each one
    // (Disk II / SmartPort 3.5" / ProDOS HDV) and boots it.
    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        mw->onFileDrop(count, paths);
    }
}

#ifdef __EMSCRIPTEN__
namespace {
/// The window the page's lifecycle events persist. Set once the frame loop is
/// about to start; null before that, and never cleared — the browser build
/// never tears its MainWindow down (see MainWindow_Session.cpp).
MainWindow* gPersistTarget = nullptr;
/// Seconds between two heartbeat persists. The desktop writes this state once,
/// at exit; the browser has no exit, so it writes it on a clock. Long enough
/// that an idle tab is doing nothing measurable, short enough that a visitor
/// who changes something and closes the tab a moment later keeps it.
constexpr double kBrowserPersistSeconds = 10.0;
} // namespace

/// Called from wasm/shell.html on `visibilitychange` → hidden and on
/// `pagehide`: the last moments a browser reliably gives a page. Writes the
/// session and asks for an immediate flush — both best-effort, which is why
/// the heartbeat above exists rather than relying on this.
extern "C" EMSCRIPTEN_KEEPALIVE void pom2_persist_now()
{
    if (!gPersistTarget) return;
    gPersistTarget->persistSession();
    pom2::flushPersistentStateNow();
}
#endif

int main(int argc, char* argv[])
{
    if (const int broker = pom2::ChildProcess::runConsoleSignalBrokerIfRequested(
            argc, argv); broker >= 0)
        return broker;
    pom2::log().info("POM2", POM2_VERSION_STRING " - Apple II Emulator (Dear ImGui)");

    bool helpRequested = false;
    auto plan = pom2::parseCli(argc, argv, helpRequested);
    if (helpRequested) return 0;
    if (!plan)         return 1;

#ifdef __EMSCRIPTEN__
    // WASM default experience: //c+ profile booting Total Replay from the
    // floppyemu/ bundle. //c+ comes up at 4× (defaultCyclesPerFrame =
    // 68180), which is what feels right in a browser — the plain //c
    // lands at 1× and disk-turbo is off by default. The browser has no
    // CLI, so we inject these as if the user had typed `--preset iic+
    // <hdv>`. Gate on a TRULY default plan (no preset AND no disk path)
    // so `?preset=ii+&nodisk=1` (shell.html) actually drops to BASIC —
    // pre-gate, the preset injection respected the URL but the disk
    // injection didn't, so II+ landed with a 32 MiB HDV it can't mount
    // instead of `]`.
    const bool wasmPlanWasDefault =
        (plan->preset == pom2::CliPreset::Default) &&
        plan->bootDiskPath.empty();
    if (wasmPlanWasDefault) {
        plan->preset       = pom2::CliPreset::AppleIIcPlus;
        plan->bootDiskPath = "floppyemu/Total Replay v6.1.hdv";
    }
#endif

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return -1;

#if POM2_GL_ES
    // GLES 3.0 tier — WebGL2 in the browser, Mesa V3D on a Raspberry Pi.
    //
    // WebGL2 ≈ OpenGL ES 3.0. ImGui's OpenGL3 backend selects shader
    // source variant from the GLSL version string — desktop "#version
    // 150" produces shaders WebGL2 can't compile, so ImGui silently
    // draws nothing (CPU + audio keep running → black canvas symptom).
    // CMake forces -sUSE_WEBGL2 / MIN=MAX=2 so the actual context is
    // always WebGL2 regardless of these hints; we still set them so
    // GLFW's Emscripten port doesn't fight us. GLFW_ALPHA_BITS=0 → the
    // canvas is opaque, otherwise the page background bleeds through
    // wherever we don't draw.
    const char* glsl_version = "#version 300 es";
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_ALPHA_BITS, 0);
#  ifndef __EMSCRIPTEN__
    // NATIVE GLES (Raspberry Pi & co) must go through EGL. GLX can only hand
    // out a GLES context when the X server advertises
    // GLX_EXT_create_context_es2_profile, and V3D — the Pi's driver, the whole
    // reason this tier exists — does not. Without this hint GLFW picks GLX by
    // default on Linux and context creation fails, which looks exactly like the
    // desktop-GL failure this tier was added to avoid. Emscripten's GLFW port
    // has no such knob (and needs none), hence the guard.
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
#  endif
#else
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
#endif

    // Native keeps the curated desktop layout: Apple II Screen on the
    // left plus room for the unified Disk Library on the right. WASM
    // starts chrome-light, so its canvas width tracks the Apple II Screen
    // window instead of reserving an empty side column.
    const char* kWindowTitle = "POM2 " POM2_VERSION_STRING " - Apple II Emulator";
    constexpr int kDefaultWindowWidth =
#ifdef __EMSCRIPTEN__
        1125;
#else
        1568;
#endif
    constexpr int kDefaultWindowHeight =
#ifdef __EMSCRIPTEN__
        825;
#else
        850;
#endif
    GLFWwindow* window = nullptr;
    if (plan->kiosk) {
        // Exclusive full-screen on the primary monitor at its current
        // video mode (no resolution change beyond what the mode dictates).
        // Copying the mode's bit depths + refresh into the hints is the
        // GLFW-recommended way to request a "windowed full screen" that
        // doesn't force a mode switch.
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
        if (mon && vm) {
            glfwWindowHint(GLFW_RED_BITS,     vm->redBits);
            glfwWindowHint(GLFW_GREEN_BITS,   vm->greenBits);
            glfwWindowHint(GLFW_BLUE_BITS,    vm->blueBits);
            glfwWindowHint(GLFW_REFRESH_RATE, vm->refreshRate);
            window = glfwCreateWindow(vm->width, vm->height, kWindowTitle,
                                      mon, nullptr);
            pom2::log().info("CLI", "--kiosk: full-screen " +
                std::to_string(vm->width) + "x" + std::to_string(vm->height));
        }
        if (!window) {
            pom2::log().warn("CLI",
                "--kiosk: no primary monitor / video mode — falling back "
                "to a windowed canvas");
        }
    }
    if (!window) {
        window = glfwCreateWindow(kDefaultWindowWidth, kDefaultWindowHeight,
                                  kWindowTitle, nullptr, nullptr);
    }
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Docking. POM2 ships ~33 free-floating panels; without docking they pile
    // up on top of each other and of the Apple II screen, and the user spends
    // the session dragging windows out of the way. `MainWindow::render()`
    // hosts a DockSpace over the viewport work area and seeds a curated
    // default layout the first time (see applyDockLayout).
    //
    // Multi-viewport is deliberately NOT enabled: it would move panels into
    // separate OS windows, which means per-viewport GL contexts and a
    // different render loop. Docking alone is the part that pays here.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Dock by dragging a tab/title bar as usual — no modifier. The screen
    // window's own NoMove flag keeps it from being dragged out by accident.
    io.ConfigDockingWithShift = false;
    // Leave `ConfigDpiScaleFonts` off: it would silently overwrite
    // `style.FontScaleDpi`, which Pom2Theme owns (monitor scale × user zoom).
    // Two writers on one field is how the zoom control would start fighting
    // the monitor scale.
    io.ConfigDpiScaleFonts     = false;
    io.ConfigDpiScaleViewports = false;
    // Note: ImGui's global `ConfigWindowsMoveFromTitleBarOnly` would be
    // the obvious knob to make the Apple II Screen content-area click-
    // through, but the user wants only THAT window restricted (others
    // keep the comfort of drag-from-anywhere). We instead apply
    // `ImGuiWindowFlags_NoMove` to the Apple II Screen and roll our
    // own title-bar drag inside `MainWindow::renderScreenWindow`.
    // Persist ImGui window positions / sizes / docking state to
    // ~/.config/POM2/imgui.ini (mirrors Settings::resolveStorePath).
    // First launch sees no file → `FirstUseEver` defaults from
    // `renderScreenWindow`/`renderDiskPanelWindow`/`renderHdvPanelWindow`
    // pick the curated layout (Screen top-left, HDV bottom-left, Disk II
    // right column). Subsequent launches restore whatever the user
    // dragged into place. Falls back to the cwd `imgui.ini` if $HOME
    // isn't set (matches Settings's fallback path).
    // Kiosk keeps a deterministic startup layout and must not restore or
    // overwrite the user's desktop window placement. WASM used to be lumped
    // in with it for the same reason — but "deterministic" there meant "the
    // browser build cannot remember anything", which is the bug, not the
    // policy. It now persists like the desktop, into the IDBFS mount; a
    // visitor who drags the panels into a mess still has View → Layout to
    // get back to a curated one.
    static std::string iniPath;
    if (plan->kiosk) {
        io.IniFilename = nullptr;
    } else {
        namespace fs = std::filesystem;
        // Same directory as state.cfg, from the same function — the copy of
        // the platform dance that used to live here drifted from Settings's
        // under Emscripten, which is how the browser build ended up writing
        // its layout to a filesystem that does not survive a reload.
        if (const fs::path dir = pom2::userConfigDir(); !dir.empty()) {
            iniPath = (dir / "imgui.ini").string();
            // Up to v0.8.5 this file lived at ~/.config/POM2/imgui.ini on
            // every Unix (macOS included) and in the launch directory on
            // Windows, while state.cfg already used the per-platform
            // directory. Moving it under userConfigDir() would have left
            // every existing user's dock layout behind, with nothing to
            // re-seed it (ui_dock_seeded stays true in state.cfg) — every
            // panel floating on the first launch after upgrade. Carry the
            // old file over, once, when the new place is empty.
            std::error_code ec;
            if (!fs::exists(iniPath, ec)) {
                std::vector<fs::path> legacy;
#if defined(_WIN32)
                legacy.emplace_back("imgui.ini");
#else
                if (const char* home = std::getenv("HOME"); home && *home)
                    legacy.emplace_back(fs::path(home) / ".config" / "POM2" / "imgui.ini");
#endif
                for (const fs::path& old : legacy) {
                    if (old.string() == iniPath || !fs::exists(old, ec)) continue;
                    if (fs::copy_file(old, iniPath, ec) && !ec) {
                        pom2::log().info("UI", "Migrated ImGui layout from " +
                                         old.string() + " to " + iniPath);
                        break;
                    }
                }
            }
        } else if (const char* home = std::getenv("HOME"); home && *home) {
            iniPath = (fs::path(home) / ".pom2_imgui.ini").string();
        } else {
            iniPath = "imgui.ini";
        }
#ifdef __EMSCRIPTEN__
        // Manual ini handling in the browser. Not a style preference: with
        // `IniFilename` set, ImGui writes the file from inside NewFrame and
        // tells nobody, and a write to the IDBFS mount that nobody hears
        // about is a write that never reaches IndexedDB (PersistentFs.h).
        // Driving load and save ourselves gives us the one thing the
        // automatic path withholds — the moment the file changed.
        io.IniFilename = nullptr;
        {
            std::error_code ec;
            if (std::filesystem::exists(iniPath, ec) && !ec)
                ImGui::LoadIniSettingsFromDisk(iniPath.c_str());
        }
#else
        io.IniFilename = iniPath.c_str();
#endif
    }
    // POM2's own style, replacing bare StyleColorsDark(). The headline change
    // is that every window background is opaque — the stock dark theme's 0.94
    // alpha made panels unreadable over a running HGR game.
    //
    // Applied here at 1:1 so the font-loading code below sees a valid style;
    // the real scale (monitor DPI × the user's persisted zoom) is applied by
    // `mainWindow.setDpiScale()` once the GLFW backend is initialised — the
    // content-scale query below needs it.
    pom2::applyTheme(pom2::UiAccent::Amber, 1.0f, 1.0f);

    // Report the caps-lock *lock* state (not the key press) in the modifier
    // bits of every key event, so the status bar can warn about it. Without
    // this GLFW only ever sets GLFW_MOD_CAPS_LOCK while the key is held.
    // Guarded: the mode is GLFW 3.3+, and Emscripten's GLFW port does not
    // define it — the browser never reports lock state anyway, so the badge
    // simply stays hidden there.
#ifdef GLFW_LOCK_KEY_MODS
    glfwSetInputMode(window, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
#endif

    // Base UI font — Proggy Clean (ImGui's default) is a bitmap that
    // only covers ASCII, so anything past U+007F (em-dash "—", en-dash
    // "–", curly quotes, accented letters used in POM2's localised
    // strings, ellipsis "…") renders as '?'. Try to load a real TTF
    // with Latin-1 Supplement + a handful of General Punctuation
    // glyphs so the menus / status text / tooltips read properly.
    //
    // Probe order: bundled font (none today), then a few common
    // distribution paths for DejaVu Sans (Debian/Ubuntu, Fedora,
    // Arch). Last resort = AddFontDefault → users see '?' for the
    // missing glyphs but the rest of the UI still works.
    //
    // The explicit SizePixels matters: since ImGui added the
    // ImFontFlags_ImplicitRefSize check, merging FontAwesome (which
    // requests 14.0f below) into a destination that uses implicit
    // ref size hard-asserts at AddFont. Both branches here pick an
    // explicit size for that reason.
    {
        namespace fs = std::filesystem;
        const char* baseCandidates[] = {
            // POM2-bundled (drop a TTF into fonts/ to override).
            "fonts/DejaVuSans.ttf",
            "../fonts/DejaVuSans.ttf",
            "../../fonts/DejaVuSans.ttf",
            // System locations seen in the wild.
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/Library/Fonts/Arial Unicode.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
        };
        // findResource resolves the bundled "fonts/…" entries against the
        // executable-relative / FHS roots too (so an installed binary finds
        // its TTF), and returns the absolute system paths verbatim when
        // they exist. See ResourcePaths.h.
        std::string basePath;
        for (const char* c : baseCandidates) {
            std::string r = pom2::findResource(c);
            if (!r.empty()) { basePath = r; break; }
        }
        // Latin-1 Supplement + selected General Punctuation. Listed
        // pair-by-pair (ImGui ranges are inclusive [from, to] pairs
        // terminated with 0). Covers: ASCII printables + accented
        // letters (é, à, ç, ü, ö, …), en/em dash, curly single + double
        // quotes, ellipsis, non-breaking space.
        static const ImWchar baseRanges[] = {
            0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
            0x2013, 0x2014,   // – (en dash) — (em dash)
            0x2018, 0x201D,   // ' ' " "  (curly quotes)
            0x2022, 0x2022,   // • (bullet)
            0x2026, 0x2026,   // … (ellipsis)
            0x20AC, 0x20AC,   // € (euro sign — used in localised strings)
            0,
        };
        if (!basePath.empty()) {
            ImFontConfig baseCfg;
            baseCfg.SizePixels    = 14.0f;
            baseCfg.OversampleH   = 2;
            baseCfg.OversampleV   = 2;
            baseCfg.PixelSnapH    = false;
            if (!io.Fonts->AddFontFromFileTTF(basePath.c_str(), 14.0f,
                                              &baseCfg, baseRanges)) {
                // TTF load failure → fall back to default so the UI
                // still comes up.
                ImFontConfig defCfg;
                defCfg.SizePixels = 13.0f;
                io.Fonts->AddFontDefault(&defCfg);
            }
        } else {
            std::fprintf(stderr,
                "POM2: no Unicode TTF found (tried DejaVu Sans + system "
                "paths) — em-dashes, curly quotes and accented chars "
                "will render as '?'. Drop DejaVuSans.ttf into fonts/ to "
                "fix.\n");
            ImFontConfig defCfg;
            defCfg.SizePixels = 13.0f;
            io.Fonts->AddFontDefault(&defCfg);
        }
    }
    {
        const char* candidates[] = {
            "fonts/fa-solid-900.ttf",
            "../fonts/fa-solid-900.ttf",
            "../../fonts/fa-solid-900.ttf",
        };
        std::string fontPath;
        for (const char* c : candidates) {
            std::string r = pom2::findResource(c);
            if (!r.empty()) { fontPath = r; break; }
        }
        if (!fontPath.empty()) {
            ImFontConfig iconsConfig;
            iconsConfig.MergeMode        = true;
            iconsConfig.PixelSnapH       = true;
            iconsConfig.GlyphMinAdvanceX = 15.0f;
            static const ImWchar iconsRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
            if (!io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 14.0f,
                                              &iconsConfig, iconsRanges)) {
                std::fprintf(stderr,
                    "POM2: failed to load %s — deck icons will render as '?'\n",
                    fontPath.c_str());
            }
        } else {
            std::fprintf(stderr,
                "POM2: fa-solid-900.ttf not found in fonts/ — deck icons will render as '?'\n");
        }
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __EMSCRIPTEN__
    // The Emscripten GLFW port doesn't feed wheel events to ImGui, so
    // io.MouseWheel stayed 0 in the browser (3D voxel zoom did nothing).
    // Register a DOM wheel handler that feeds ImGui directly, with the same
    // delta scaling as ImGui's own backend handler. We do this surgically
    // rather than ImGui_ImplGlfw_InstallEmscriptenCallbacks() so its canvas-
    // resize / fullscreen hooks don't fight wasm/shell.html's own sizing.
    emscripten_set_wheel_callback("#canvas", nullptr, false,
        [](int, const EmscriptenWheelEvent* e, void*) -> EM_BOOL {
            const float m = (e->deltaMode == DOM_DELTA_PIXEL) ? 1.0f / 100.0f
                          : (e->deltaMode == DOM_DELTA_LINE)  ? 1.0f / 3.0f
                          : 80.0f;
            ImGui::GetIO().AddMouseWheelEvent(
                static_cast<float>(e->deltaX) * -m,
                static_cast<float>(e->deltaY) * -m);
            return EM_TRUE;
        });
#endif
    ImGui_ImplOpenGL3_Init(glsl_version);

    // ── A boot disk that lives on a TNFS server ──────────────────────────
    // `POM2 tnfs://host/path/image.po` pulls the image into a local cache and
    // then becomes an ordinary positional disk — so classifyDiskForSlot picks
    // the drive, the two-phase mount moves it in off the state mutex, and
    // writes land on the local copy. TNFS is served read-only here, and a
    // local copy is the only honest place for a write to go.
    //
    // Done HERE, before the window loop rather than inside the boot
    // countdown: the fetch is seconds of blocking network I/O, and the boot
    // countdown runs between frames on the UI thread.
    //
    // The `tnfs://` scheme is REQUIRED for the positional. parseTnfsUrl also
    // accepts a bare host/path — convenient in a text field — but a bare
    // `disks/foo.po` is a relative filename, and the positional argument
    // cannot be allowed to guess between the two.
    {
        std::string lower = plan->bootDiskPath;
        for (char& ch : lower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower.rfind("tnfs://", 0) == 0) {
            pom2::Settings settingsForPaths;
            const std::filesystem::path store(settingsForPaths.getStorePath());
            const std::filesystem::path cache =
                (store.has_parent_path() ? store.parent_path()
                                         : std::filesystem::path("."))
                / "tnfs_cache";
            const auto got = pom2::tnfsFetchImage(plan->bootDiskPath, cache.string());
            if (got.ok) {
                plan->bootDiskPath = got.localPath;
            } else {
                // Same shape as a positional disk that will not mount: say so
                // and carry on to a usable machine, rather than exiting and
                // leaving the user with no way to look at anything.
                pom2::log().error("CLI", "TNFS: " + got.error);
                plan->bootDiskPath.clear();
            }
        }
    }

    // Heap-owned, not a plain local, so its lifetime can END BEFORE the GL /
    // GLFW / ImGui teardown at the bottom of main(). ~MainWindow deletes GPU
    // objects — the About and keyboard textures, the paint/sprite editors'
    // textures, and (through its members) Voxel3DRenderer's FBO + program.
    // As a local it was destroyed after glfwTerminate(), i.e. every one of
    // those glDelete* calls hit a context that no longer existed: undefined
    // behaviour, and a driver-dependent crash at exit. See the `reset()`
    // next to captureWindowGeometryNow() below.
    auto mainWindowOwner = std::make_unique<MainWindow>(plan->forceIIPlus);
    MainWindow& mainWindow = *mainWindowOwner;
    if (plan->forceIIPlus) {
        pom2::log().info("CLI", "--ii-plus: ignoring apple2e.rom, booting as II+");
    }
    // Kiosk flag FIRST: setGlfwWindow restores the persisted windowed
    // geometry, and doing that to the exclusive full-screen window this
    // path already created would trigger a video MODE SWITCH to the old
    // window's size (GLFW: glfwSetWindowSize on a full-screen window
    // changes its desired video mode). The flag makes setGlfwWindow skip
    // the restore.
    mainWindow.setKioskMode(plan->kiosk);
    // Hand the GLFW window to MainWindow BEFORE any applyProfile() call so
    // the profile-driven title update (step 13 in applyProfile) sees a
    // valid handle even when --preset triggers the switch.
    // FujiNet relay, armed BEFORE the machine boots so the autostart slot
    // scan finds a FujiNet on its first pass rather than falling through to
    // the Disk II and needing a manual reboot.
    if (plan->fujiNet != pom2::CliPlan::FujiNetTransport::None) {
        std::string err;
        const bool serial = plan->fujiNet == pom2::CliPlan::FujiNetTransport::Serial;
        const bool presetHasNoSlots =
            plan->preset == pom2::CliPreset::AppleIIc ||
            plan->preset == pom2::CliPreset::AppleIIcPlus ||
            plan->preset == pom2::CliPreset::AppleIIcPAL;
        // `slot` is in/out: without an explicit --fujinet-slot it may be
        // relocated to the first free slot, and the log line must name the one
        // actually used. MainWindow remembers the request, so a `--preset`
        // rebuild below re-plugs the card instead of destroying it.
        int slot = plan->fujiNetSlot;
        if (presetHasNoSlots) {
            pom2::log().error("CLI", "--fujinet: the selected //c-class "
                                      "profile has no physical expansion slots");
        } else if (mainWindow.plugFujiNetFromCli(slot, plan->fujiNetSlotExplicit,
                                          serial, plan->fujiNetSerialPath,
                                          plan->fujiNetPort, err)) {
            pom2::log().info("CLI", "FujiNet card in slot " +
                                        std::to_string(slot) +
                                        (serial ? " (serial)" : " (TCP :" +
                                            std::to_string(plan->fujiNetPort) + ")"));
        } else {
            pom2::log().error("CLI", "--fujinet: " + err);
        }
    }

    mainWindow.setGlfwWindow(window);
    // Re-applies the theme with the settings-restored accent + user zoom on
    // top of the monitor scale. The ctor can't do it: it loads `state.cfg`
    // but has no way to know the display's content scale.
    //
    // Use the BACKEND's helper, not `glfwGetWindowContentScale` directly.
    // They differ exactly where it matters: on macOS, Wayland, Emscripten and
    // Android the framebuffer is already larger than the window, so ImGui's
    // DisplayFramebufferScale path handles HiDPI and the helper returns 1.0f.
    // Querying GLFW ourselves would report 2.0 there and scale the UI twice.
    // It also preserves the 0.0 that virtual/accessibility monitors report
    // (imgui #7902) — `setDpiScale` clamps that back to 1.
    mainWindow.setDpiScale(ImGui_ImplGlfw_GetContentScaleForWindow(window));
    // (setKioskMode already ran above, before setGlfwWindow — see there.)
#ifdef __EMSCRIPTEN__
    mainWindow.setBrowserResetBootImage(plan->bootDiskPath);
#endif
    // CLI --preset selection (must come AFTER MainWindow's legacy boot so
    // it overrides via the full cold-reset path applyProfile uses).
    if (plan->preset != pom2::CliPreset::Default) {
        pom2::SystemProfile sp = pom2::SystemProfile::AppleIIPlus;
        switch (plan->preset) {
            case pom2::CliPreset::AppleII:      sp = pom2::SystemProfile::AppleII;      break;
            case pom2::CliPreset::AppleIIPlus:  sp = pom2::SystemProfile::AppleIIPlus;  break;
            case pom2::CliPreset::AppleIIeUnenhanced:
                                                sp = pom2::SystemProfile::AppleIIeUnenhanced; break;
            case pom2::CliPreset::AppleIIe:     sp = pom2::SystemProfile::AppleIIe;     break;
            case pom2::CliPreset::AppleIIc:     sp = pom2::SystemProfile::AppleIIc;     break;
            case pom2::CliPreset::AppleIIcPlus: sp = pom2::SystemProfile::AppleIIcPlus; break;
            case pom2::CliPreset::AppleIIePAL:  sp = pom2::SystemProfile::AppleIIePAL;  break;
            case pom2::CliPreset::AppleIIcPAL:  sp = pom2::SystemProfile::AppleIIcPAL;  break;
            case pom2::CliPreset::AppleIIeUnenhancedPAL:
                                sp = pom2::SystemProfile::AppleIIeUnenhancedPAL; break;
            case pom2::CliPreset::Default: break;
        }
        mainWindow.applyProfile(sp);
    }
    glfwSetWindowUserPointer(window, &mainWindow);
    glfwSetCharCallback(window, glfw_char_callback);
    glfwSetKeyCallback (window, glfw_key_callback);
    glfwSetCursorPosCallback  (window, glfw_cursor_pos_callback);
    glfwSetMouseButtonCallback(window, glfw_mouse_button_callback);
    glfwSetWindowFocusCallback(window, glfw_window_focus_callback);
    glfwSetDropCallback       (window, glfw_drop_callback);

    // ─── Phase B: apply boot-time overrides on the live emulator ─────────
    if (plan->cpuMax) {
        mainWindow.emul().setCyclesPerFrame(1'000'000);
        pom2::log().info("CLI", "--cpu-max: emulator running flat-out");
    }
    if (plan->executionSpeed) {
        mainWindow.emul().setCyclesPerFrame(*plan->executionSpeed);
    }
    if (plan->aiControl) {
        std::string err;
        if (!mainWindow.startAiControlFromCli(
                static_cast<unsigned short>(plan->aiControlPort), err)) {
            pom2::log().error("CLI", "--ai-control: " + err);
        }
    }
    if (plan->displayMode != pom2::CliDisplayMode::NoHint) {
        Apple2Display::HiResMode m = Apple2Display::HiResMode::ColorNTSC;
        const char* label = "ntsc";
        switch (plan->displayMode) {
            case pom2::CliDisplayMode::ColorNTSC:
                m = Apple2Display::HiResMode::ColorNTSC;    label = "ntsc";        break;
            case pom2::CliDisplayMode::ChatMauveRGB:
                m = Apple2Display::HiResMode::ChatMauveRGB; label = "chatmauve";   break;
            case pom2::CliDisplayMode::MonoWhite:
                m = Apple2Display::HiResMode::MonoWhite;    label = "mono-white";  break;
            case pom2::CliDisplayMode::MonoGreen:
                m = Apple2Display::HiResMode::MonoGreen;    label = "mono-green";  break;
            case pom2::CliDisplayMode::MonoAmber:
                m = Apple2Display::HiResMode::MonoAmber;    label = "mono-amber";  break;
            case pom2::CliDisplayMode::NoHint: break;
        }
        mainWindow.displayRef().setHiResMode(m);
        pom2::log().info("CLI", std::string("--display ") + label);
    }
    if (plan->rgbCardInvertBit7.has_value()) {
        const bool v = *plan->rgbCardInvertBit7;
        const bool applied = mainWindow.setChatMauveInvertBit7(v);
        pom2::log().info("CLI",
            std::string("--rgb-card-invert-bit7=") + (v ? "on" : "off")
            + (applied ? "" : " (no card plugged at boot)"));
    }
    if (!plan->initialTapePath.empty()) {
        if (mainWindow.emul().loadTape(plan->initialTapePath)) {
            pom2::log().info("CLI", "--tape loaded: " + plan->initialTapePath);
            if (plan->initialTapeAutoPlay) mainWindow.emul().playTape();
        } else {
            pom2::log().warn("CLI", "--tape failed: " +
                mainWindow.emul().cassette().getLastError());
        }
    }
    auto mount35Cli = [&](int idx, const std::string& path, const char* flag) {
        if (path.empty()) return;
        // Documented contract: --35-disk1/2 drive the //c+ on-board Sony 3.5"
        // hub, which only exists on the //c+ profile. On any other profile
        // mount35() would silently write into a hub the machine can't read,
        // so warn and ignore (CliDispatcher.h). Slot SmartPort 3.5" cards on
        // //e/II+ are mounted through the GUI/Library, not this flag.
        if (mainWindow.currentProfile() != pom2::SystemProfile::AppleIIcPlus) {
            pom2::log().warn("CLI", std::string(flag) +
                " ignored: 3.5\" disks require the //c+ profile (--preset iic+)");
            return;
        }
        if (mainWindow.emul().mount35(idx, path)) {
            pom2::log().info("CLI", std::string(flag) + " mounted: " + path);
        } else {
            const auto& img = (idx == 0)
                ? mainWindow.emul().disk35Internal()
                : mainWindow.emul().disk35External();
            pom2::log().warn("CLI",
                std::string(flag) + " failed: " + img.lastError());
        }
    };
    mount35Cli(0, plan->disk35Internal, "--35-disk1");
    mount35Cli(1, plan->disk35External, "--35-disk2");

    // ─── Phase C deferred actions: kick off in a background thread that
    // sleeps briefly (let the worker thread + first render frame land)
    // then runs every action in order. The thread is TRACKED (not
    // detached) so we can join before MainWindow goes out of scope —
    // otherwise a fast-quit user could close the window in <250 ms and
    // the lambda would dereference a destroyed EmulationController. The
    // `deferredCancelled` flag lets fast-quit skip the actions entirely
    // (sleep loop polls it every 10 ms) so shutdown stays snappy.
    std::atomic<bool>  deferredCancelled{false};
    // Ordering gate vs the positional-disk boot below. The deferred
    // actions (--run / --paste / --step …) are meant to act on the BOOTED
    // machine, but the two used to be paced independently: this thread
    // slept a fixed 250 ms while the boot fired on a frame countdown, so
    // the order flipped with the monitor's refresh rate (30 frames is
    // ~500 ms at 60 Hz but ~208 ms at 144 Hz — the actions then ran
    // against a machine the boot was about to reset out from under them).
    // Pre-set when there is no positional disk, so the no-disk path keeps
    // its old timing exactly.
    std::atomic<bool>  bootDiskSettled{plan->bootDiskPath.empty()};
#ifndef __EMSCRIPTEN__
    std::thread        deferredThread;
    if (!plan->deferredActions.empty()) {
        deferredThread = std::thread(
            [actions = plan->deferredActions,
             emu     = &mainWindow.emul(),
             booted  = &bootDiskSettled,
             cancel  = &deferredCancelled] {
                for (int i = 0; i < 25; ++i) {
                    if (cancel->load(std::memory_order_acquire)) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                // Then wait for the positional-disk boot to land (bounded:
                // ~5 s, well past the 30-frame countdown at any refresh
                // rate, so a failed/absent boot can't wedge the actions).
                for (int i = 0; i < 500 &&
                                !booted->load(std::memory_order_acquire); ++i) {
                    if (cancel->load(std::memory_order_acquire)) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (cancel->load(std::memory_order_acquire)) return;
                // Guard the worker: an uncaught exception escaping a
                // std::thread callable calls std::terminate(). Deferred
                // actions touch user-named files, so an I/O or alloc failure
                // must not crash the whole emulator. Same barrier every other
                // POM2 thread wears — ThreadGuard.h.
                pom2::runGuarded("CLI", [&] {
                    pom2::runDeferredActions(actions, *emu);
                });
            });
    }
#endif

    // Optional auto-boot path for capturing traces / repro headless-ish.
    // POM2_AUTO_BOOT_HDV=<N>  → after N seconds (default 1), call
    //                          bootHdvImage() on the main UI thread.
    // POM2_AUTO_QUIT=<N>      → after N seconds, glfwSetWindowShouldClose
    //                          so the binary exits cleanly without state.cfg
    //                          stomping (state IS saved on clean exit).
    std::atomic<bool> autoBootRequested{false};
    std::atomic<bool> autoQuitRequested{false};
#ifndef __EMSCRIPTEN__
    std::thread       autoBootThread;
    std::atomic<bool> autoBootCancelled{false};
    {
        const char* abEnv = std::getenv("POM2_AUTO_BOOT_HDV");
        const char* aqEnv = std::getenv("POM2_AUTO_QUIT");
        if (abEnv || aqEnv) {
            const int abDelay = abEnv ? std::max(0, std::atoi(abEnv)) : -1;
            const int aqDelay = aqEnv ? std::max(0, std::atoi(aqEnv)) : -1;
            // Both delays count from PROGRAM START — the documented
            // contract is "after N seconds", so the quit timer waits the
            // REMAINDER after the boot timer fired, not aqDelay on top of
            // it. Sliced 100 ms sleeps + a cancel flag (same pattern as
            // deferredThread) so closing the window early doesn't stall
            // shutdown in join() for the rest of the delays.
            autoBootThread = pom2::guardedThread("CLI",
                                            [&, abDelay, aqDelay]() {
                auto sleepSeconds = [&](int sec) {
                    for (int t = 0; t < sec * 10 && !autoBootCancelled; ++t)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(100));
                };
                int elapsed = 0;
                if (abDelay >= 0) {
                    const int d = abDelay > 0 ? abDelay : 1;
                    sleepSeconds(d);
                    if (autoBootCancelled) return;
                    autoBootRequested.store(true);
                    elapsed = d;
                }
                if (aqDelay >= 0) {
                    if (aqDelay > elapsed) sleepSeconds(aqDelay - elapsed);
                    if (autoBootCancelled) return;
                    autoQuitRequested.store(true);
                }
            });
        }
    }
#endif

    // Positional disk image → mount + boot once the worker thread and the
    // first few frames have settled (slot cards plugged, CPU running). A
    // small frame countdown keeps this on the UI thread between frames, so
    // the SlotBus mutation in insertAndBootImage() doesn't race the worker.
    // Works in both GUI and --kiosk mode (bare `POM2 disk` boots in GUI).
    int cliBootCountdown = (plan->bootDiskPath.empty() && plan->prodosFolderPath.empty()) ? -1 : 30;

    // Loop iteration packaged as a function so the native path can stay a
    // plain `while`, and the WASM path can hand it to
    // `emscripten_set_main_loop_arg` (which forbids a blocking loop on
    // the main thread — the browser owns the frame schedule there).
    struct FrameCtx {
        GLFWwindow*         window;
        MainWindow*         mainWindow;
        std::string         bootDiskPath;
        std::string         prodosFolderPath;
        int                 cliBootCountdown;
        std::atomic<bool>*  autoBootRequested;
        std::atomic<bool>*  autoQuitRequested;
        /// Released once the positional-disk boot has run, so the Phase-C
        /// deferred actions always observe the booted machine regardless
        /// of the host refresh rate. Null when there is no boot disk.
        std::atomic<bool>*  bootDiskSettled;
#ifdef __EMSCRIPTEN__
        bool                firstFrameReadySignaled;
        /// Where ImGui's layout is written, or null in kiosk mode. Held as
        /// a raw pointer into the `iniPath` static above, which outlives the
        /// loop (main() never returns under simulate_infinite_loop).
        const char*         imguiIniPath;
        double              lastPersistSeconds;
#endif
    } frameCtx{
        window, &mainWindow, plan->bootDiskPath, plan->prodosFolderPath, cliBootCountdown,
        &autoBootRequested, &autoQuitRequested, &bootDiskSettled
#ifdef __EMSCRIPTEN__
        , false
        , plan->kiosk ? nullptr : iniPath.c_str()
        , 0.0
#endif
    };
    #ifdef __EMSCRIPTEN__
    // The browser's "the user is leaving" signals are JS events, and the only
    // C++ that can answer them is a function with external linkage. main()
    // never returns here (simulate_infinite_loop), so the pointer stays valid
    // for the life of the tab.
    gPersistTarget = &mainWindow;
#endif
    auto iterate = [](void* userdata) {
        auto& c = *static_cast<FrameCtx*>(userdata);
        glfwPollEvents();
#ifdef __EMSCRIPTEN__
        // Single-threaded build: drive the CPU from the render loop
        // since there's no worker thread on this host. Run one frame's
        // worth of cycles before drawing so input → CPU → display
        // pipeline still updates every frame.
        c.mainWindow->emul().tickFrame();
#endif

        if (c.cliBootCountdown > 0 && --c.cliBootCountdown == 0) {
            std::string err;
            if (!c.prodosFolderPath.empty() &&
                !c.mainWindow->mountProDOSFolder(c.prodosFolderPath, err)) {
                pom2::log().warn("CLI", "ProDOS folder mount failed: " + err);
            }
            err.clear();
            if (c.bootDiskPath.empty()) {
                pom2::log().info("CLI", "ProDOS host folder mounted without boot disk");
            } else if (c.mainWindow->insertAndBootImage(c.bootDiskPath, err)) {
                pom2::log().info("CLI", "booted disk: " + c.bootDiskPath);
            } else {
                pom2::log().warn("CLI", "disk boot failed: " + err);
            }
            // Release the Phase-C deferred actions — success or failure,
            // the machine's boot state is now settled (see bootDiskSettled).
            if (c.bootDiskSettled)
                c.bootDiskSettled->store(true, std::memory_order_release);
        }

        if (c.autoBootRequested->exchange(false)) {
            c.mainWindow->bootHdvImage();
        }
        if (c.autoQuitRequested->load()) {
            glfwSetWindowShouldClose(c.window, GLFW_TRUE);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        c.mainWindow->render();

        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(c.window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(c.window);
#ifdef __EMSCRIPTEN__
        // The layout, then everything durable. ImGui raises the flag at most
        // once every `IniSavingRate` seconds (5 by default), so this writes
        // rarely; the pump then coalesces it with any settings write into one
        // IndexedDB round-trip.
        if (ImGui::GetIO().WantSaveIniSettings && c.imguiIniPath) {
            ImGui::SaveIniSettingsToDisk(c.imguiIniPath);
            ImGui::GetIO().WantSaveIniSettings = false;
            pom2::markPersistentStateDirty();
        }
        // Heartbeat. Everything the desktop writes in ~MainWindow() has no
        // moment to run in the browser, so it runs on a clock instead; a
        // save whose content is unchanged costs a map comparison and stops
        // there (Settings::save).
        {
            // The browser's own clock rather than glfwGetTime(): this loop is
            // driven by requestAnimationFrame, and the wall-clock question
            // "has it been ten seconds" has nothing to do with GLFW.
            const double now = emscripten_get_now() / 1000.0;
            if (now - c.lastPersistSeconds >= kBrowserPersistSeconds) {
                c.lastPersistSeconds = now;
                c.mainWindow->persistSession();
            }
        }
        pom2::pumpPersistentState();
        if (!c.firstFrameReadySignaled) {
            c.firstFrameReadySignaled = true;
            emscripten_run_script(
                "if (globalThis.pom2FirstFrameReady) "
                "globalThis.pom2FirstFrameReady();");
        }
#endif
    };

#ifdef __EMSCRIPTEN__
    // `1` = simulate_infinite_loop → the runtime throws to terminate
    // main() so the captured FrameCtx and surrounding locals stay alive
    // for the lifetime of the browser tab. fps=0 → browser-driven RAF
    // cadence; WebGL swap takes care of vsync.
    // Captureless lambda decays to `void(*)(void*)` (em_arg_callback_func).
    emscripten_set_main_loop_arg(iterate, &frameCtx, 0, 1);
#else
    std::signal(SIGINT, requestShutdown);
    std::signal(SIGTERM, requestShutdown);
    while (!glfwWindowShouldClose(window)) {
        if (gShutdownRequested)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        iterate(&frameCtx);
    }
#endif

    // Stop the deferred-actions worker before any destructors run.
    // Signal cancellation so a thread still in its 250 ms wakeup window
    // exits promptly instead of running actions on the about-to-be-
    // destroyed emulator.
    deferredCancelled.store(true, std::memory_order_release);
#ifndef __EMSCRIPTEN__
    autoBootCancelled.store(true, std::memory_order_release);
    if (deferredThread.joinable()) deferredThread.join();
    if (autoBootThread.joinable()) autoBootThread.join();
#endif

    // Record where the window ended up WHILE GLFW is still alive.
    // The capture cannot live in ~MainWindow: glfwGetWindowPos/Size bail on
    // the un-init check, zero their out-params, and the geometry write is
    // silently dropped. (~MainWindow no longer runs after glfwTerminate() —
    // `mainWindowOwner.reset()` a few lines below destroys it while the GL
    // context, the GLFW window and the ImGui context are all still alive —
    // but the capture stays here anyway: it is the one call that must happen
    // before ANY teardown, and moving it back into the destructor would put
    // it after the panels' own GL cleanup for no gain.)
    mainWindow.captureWindowGeometryNow();

    // Destroy the window object HERE, while the GL context, the GLFW window
    // and the ImGui context are all still alive — ~MainWindow issues
    // glDeleteTextures / glDeleteProgram (About + keyboard photos, the paint
    // and sprite editors, Voxel3DRenderer's FBO and shader). Running that
    // after glfwTerminate(), as the old plain-local lifetime did, is a call
    // into a torn-down context. The GLFW callbacks below all go through
    // glfwGetWindowUserPointer, so clear it first: no event dispatched
    // during glfwDestroyWindow can reach the freed object.
    glfwSetWindowUserPointer(window, nullptr);
    mainWindowOwner.reset();

    // ~FujiNetCard hands its helper teardown to a detached thread that would
    // die with the process; wake it and wait for the SIGKILL sweep, or a
    // SIGTERM-trapping helper outlives POM2 holding the loopback port.
    pom2::ChildProcess::drainDetached();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
