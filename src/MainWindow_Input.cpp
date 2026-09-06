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

// MainWindow_Input — every host input on its way to the emulated machine:
// keyboard, clipboard and file paste, the Mouse Card pointer grab, mouse
// motion and buttons, the joystick/paddle poll, and file drops.
//
// Two things here are policy rather than plumbing, and both have a documented
// reason:
//
//   * The pointer grab. `MouseGrab.h` holds the policy and stays GLFW-free so
//     a headless test can link it; the token asserts below are what keep its
//     mirrored constants honest against the real GLFW ones.
//   * The keyboard latch. Left Alt and Right Alt each have TWO sources (the
//     host key and the on-screen //e keyboard), held apart and OR'd in
//     AppleKeyLatch.h — a source that assigned the wire directly released the
//     other one. Pinned by `apple_key_latch`.
//
// The keyboard latch and the paste queue own `Memory::kbMutex`, which is why
// they are among the few things allowed to reach memory without `stateMutex`.

#include "MainWindow.h"

#include "AppleKeyLatch.h"
#include "EmulationController.h"
#include "JoystickInput.h"
#include "Keyboard_ImGui.h"
#include "Logger.h"
#include "Memory.h"
#include "MouseCoordinator.h"
#include "MouseGrab.h"
#include "FourPlayCard.h"
#include "PaddleInputs.h"
#include "Pom2Theme.h"
#include "ResourcePaths.h"
#include "Settings.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ─── Mouse Card input routing ───────────────────────────────────────────

// `MouseGrab.h` stays GLFW-free so a headless test can link it. Prove its
// mirrored tokens still match the real ones here, where <GLFW/glfw3.h> is
// in scope — an upstream renumbering becomes a compile error, not a chord
// that silently stops working.
static_assert(pom2::mousegrab::kKeyG         == GLFW_KEY_G);
static_assert(pom2::mousegrab::kModControl   == GLFW_MOD_CONTROL);
static_assert(pom2::mousegrab::kModAlt       == GLFW_MOD_ALT);
static_assert(pom2::mousegrab::kButtonLeft   == GLFW_MOUSE_BUTTON_LEFT);
static_assert(pom2::mousegrab::kButtonMiddle == GLFW_MOUSE_BUTTON_MIDDLE);

void MainWindow::injectAscii(uint8_t apple2Code)
{
    // Not under lockState(), on purpose: queueKey takes `Memory::kbMutex`,
    // the finer-grained lock that lets the UI and the AI server inject keys
    // without contending with the worker on every keystroke. Same for
    // pasteText / pendingPasteSize / cancelPaste and the kiosk key path;
    // the Open/Solid-Apple setters are plain atomics. Those are the only
    // Memory entry points in this file that may be reached unlocked.
    controller->memory().queueKey(apple2Code);
}

void MainWindow::onChar(unsigned int codepoint)
{
    // While the kiosk menu is up, its keyboard fallbacks (K toggles the key
    // band, etc.) are read via ImGui::IsKeyPressed — the same keystroke must
    // not also land in the $C000 latch.
    if (kioskMenuOpen_) return;
    // In kiosk, K is reserved (Select fallback): the OPEN direction leaks
    // otherwise — this callback fires while the menu is still closed, then
    // updateKioskMenu opens the non-pausing Keys band the same frame, so the
    // running game would receive a live 'k' on every open.
    if (kiosk_ && (codepoint == 'k' || codepoint == 'K')) return;
    // Apple II accepts the full ASCII range (uppercase and lowercase). We
    // forward the codepoint as-is — Applesoft and the Monitor pick whichever
    // case the user typed.
    if (codepoint >= 0x20 && codepoint < 0x80) {
        injectAscii(static_cast<uint8_t>(codepoint));
    }
}

namespace {

// The letter the user's KEYBOARD LAYOUT prints on this key, uppercased, or 0
// when the key produces no single ASCII letter.
//
// GLFW key codes are POSITIONS on a US QWERTY board, not characters: on a
// French AZERTY keyboard GLFW_KEY_A is the key capped 'Q', GLFW_KEY_Q is
// 'A', GLFW_KEY_W is 'Z', GLFW_KEY_Z is 'W' — and the AZERTY 'M' key is
// GLFW_KEY_SEMICOLON, outside the A..Z range entirely. Deriving Ctrl-letter
// from the position therefore handed an AZERTY user Ctrl-Q when they pressed
// the key marked A, and nothing at all for M. (Ctrl-C, the one every
// Applesoft user needs, sits on the same physical key in both layouts, which
// is how this survived.)
//
// `glfwGetKeyName(key, scancode)` returns the layout's character for the key,
// independent of modifiers — that is what the `scancode` parameter of the
// callback is for. GLFW requires it on the main thread, which is where GLFW
// dispatches its own callbacks, so onKey satisfies that by construction. It
// returns null for non-printable keys and can return a multi-byte name on a
// layout whose cap is not ASCII, so the US positional map stays as the
// fallback: no layout gets FEWER Ctrl-letters than it had.
char layoutLetter(int key, int scancode)
{
    if (const char* name = glfwGetKeyName(key, scancode);
        name && name[0] && name[1] == '\0') {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return c;
    }
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
        return static_cast<char>('A' + (key - GLFW_KEY_A));
    return 0;
}

}  // namespace

void MainWindow::onKey(int key, int scancode, int action, int mods)
{
    // Open-Apple / Solid-Apple are read by the IIe/IIc/IIc+ firmware via
    // $C061/$C062 bit 7 (MAME `apple2e.cpp:2157-2169`) — the firmware itself
    // decides cold-reboot vs self-test on Ctrl+Reset. We just source the
    // bits; observe both press and release so the firmware sees the key
    // released after Reset like on real hardware.
    // Through `pushAppleKeys()`, never straight to Memory: the on-screen
    // keyboard presses the same two wires and would otherwise clear this.
    if (key == GLFW_KEY_LEFT_ALT) {
        appleKeys_.hostOpen = (action != GLFW_RELEASE);
        pushAppleKeys();
        return;
    }
    if (key == GLFW_KEY_RIGHT_ALT) {
        appleKeys_.hostSolid = (action != GLFW_RELEASE);
        pushAppleKeys();
        return;
    }

    // Ctrl+Alt+G toggles the Mouse Card pointer capture. Placed above every
    // other branch — including the kiosk-menu gate below — for the same
    // reason F10/F11/F12 are routed unconditionally: a captured pointer with
    // no reachable way out is a trap. PRESS only, so holding the chord can't
    // flip capture ~30×/s on auto-repeat. Note the Left-Alt half also sets
    // Open-Apple (handled above, and cleared when the user lifts it) — the
    // guest sees a modifier press it would have seen anyway.
    // Tested before the Ctrl-letter path further down, which would otherwise
    // also inject Ctrl-G ($07) into the keyboard latch.
    if (pom2::mousegrab::isToggleChord(key, mods)) {
        if (action == GLFW_PRESS) toggleMouseGrab();
        return;
    }

    // Ctrl+Alt+F — the second GUI ⇄ kiosk toggle, alongside F10. Sits with
    // Ctrl+Alt+G above every other branch for the same two reasons: leaving
    // kiosk must ALWAYS work, and the chord has to be tested before the
    // Ctrl-letter path further down or it would also inject Ctrl-F ($06)
    // into the keyboard latch. Matched on either Alt and regardless of
    // Shift/Super (GLFW folds both Alts into GLFW_MOD_ALT), so a stray
    // modifier can never strand a full-screen session. PRESS only: on
    // GLFW_REPEAT a held chord would flip full-screen ⇄ windowed ~30×/s,
    // each flip doing a window-monitor change AND a synchronous
    // settings->save() to disk.
    //
    // Why a second binding at all: F10 is claimed by the window manager on
    // several desktops (GNOME/KDE open the focused window's menu with it),
    // where it never reaches GLFW. A chord in the same family as Ctrl+Alt+G
    // is reachable everywhere.
    if (key == GLFW_KEY_F && (mods & GLFW_MOD_CONTROL) && (mods & GLFW_MOD_ALT)) {
        if (action == GLFW_PRESS) toggleKioskMode();
        return;
    }

    // Kiosk menu open: its arrows/Enter/Esc fallbacks are polled with
    // ImGui::IsKeyPressed and the menu window never captures the keyboard,
    // so everything below would double-deliver — Enter on the key band
    // would send the cell AND inject $0D, Esc would close the menu AND
    // type $1B into the game on resume.
    if (kioskMenuOpen_) {
        // F10 still leaves kiosk with the in-kiosk menu open — the user
        // must always have a way back to the GUI. (Ctrl+Alt+F is handled
        // above, so it works here too.)
        if (key == GLFW_KEY_F10 && action == GLFW_PRESS) toggleKioskMode();
        return;
    }
    // K reserved in kiosk (see onChar) — also blocks Ctrl-K's $0B, since
    // eSelect fires on the K key regardless of modifiers.
    if (kiosk_ && key == GLFW_KEY_K) return;

    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    // Both host shortcuts below match on the LAYOUT's letter, like the
    // Ctrl-letter path at the bottom of this function: the user presses the
    // key marked V (resp. P) on their own keyboard, wherever GLFW thinks
    // that position is.
    const char ctrlLetter = ctrl ? layoutLetter(key, scancode) : 0;

    // Ctrl-V intercepts the host shortcut: paste system clipboard into
    // the Apple II keyboard buffer rather than injecting raw $16. The
    // Apple II's own Ctrl-V (rarely used) is reached from the clickable
    // //e keyboard panel: latch CONTROL, click V. NOT from the Edit menu —
    // that menu only carries the host clipboard actions (paste from
    // clipboard / from file / cancel). Ctrl-Shift-V is unmapped and free if
    // a future version wants a direct chord.
    if (ctrlLetter == 'V') {
        pasteFromClipboard();
        return;
    }

    // Ctrl+Shift+P opens the command palette. Shift is what keeps it off the
    // Apple II's Ctrl-P ($10) — which CP/M under the SoftCard uses for printer
    // echo, so plain Ctrl-P must keep reaching the guest.
    if (ctrlLetter == 'P' && (mods & GLFW_MOD_SHIFT)) {
        openCommandPalette();
        return;
    }

    switch (key) {
        case GLFW_KEY_ENTER:        // fallthrough — main + numpad Enter both
        case GLFW_KEY_KP_ENTER:     injectAscii(0x0D); break;
        case GLFW_KEY_BACKSPACE:    injectAscii(0x08); break;
        case GLFW_KEY_LEFT:         injectAscii(0x08); break;
        case GLFW_KEY_RIGHT:        injectAscii(0x15); break;
        case GLFW_KEY_UP:           injectAscii(0x0B); break;
        case GLFW_KEY_DOWN:         injectAscii(0x0A); break;
        case GLFW_KEY_ESCAPE:       injectAscii(0x1B); break;
        case GLFW_KEY_TAB:          injectAscii(0x09); break;
        // PRESS only, like F10 below: `saveScreenshot()` takes stateMutex
        // AND the demodulator's lock, runs a CPU-side demod of the frame and
        // writes one PNG per call. On GLFW_REPEAT a leaned-on F9 fired that
        // ~30×/s, spraying files and stalling the machine.
        case GLFW_KEY_F9:
            if (action == GLFW_PRESS) saveScreenshot();
            break;
        // F10 = GUI <-> kiosk (Ctrl+Alt+F does the same, handled above).
        // "Full screen" in the GUI IS kiosk mode:
        // exclusive full-screen with the chrome-free render path. The
        // machine keeps running across the switch (no snapshot needed —
        // kiosk touches only windowing / rendering / settings-writes).
        // PRESS only: this switch also runs for GLFW_REPEAT, and holding
        // F10 would otherwise flip full-screen ⇄ windowed ~30×/s, each
        // entry doing a window-monitor change AND a synchronous
        // settings->save() to disk.
        case GLFW_KEY_F10:
            if (action == GLFW_PRESS) toggleKioskMode();
            break;
        // Both resets are PRESS only for the same reason as F10/F9: a held
        // key repeats, and re-vectoring the CPU through $FFFC ~30×/s means
        // the machine can never get past its reset handler — the user sees a
        // frozen screen for as long as the key is down, and (on F12) a wiped
        // register file each time.
        case GLFW_KEY_F11:
            if (action == GLFW_PRESS) controller->softReset();
            break;
        case GLFW_KEY_F12:
            if (action == GLFW_PRESS) controller->hardReset();
            break;
        default:
            // Ctrl-A..Ctrl-Z generate $01..$1A — these matter for Applesoft
            // (Ctrl-C breaks out of a running program, Ctrl-G beeps, etc.).
            // The letter comes from the LAYOUT, not the key position: see
            // layoutLetter() above.
            if (ctrl) {
                if (const char c = layoutLetter(key, scancode))
                    injectAscii(static_cast<uint8_t>(c - 'A' + 1));
            }
            break;
    }
}

void MainWindow::pasteFromClipboard()
{
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) {
        tapeStatusMessage = "Paste: clipboard is empty";
        tapeStatusUntil   = lastFrameTime + 3.0;
        return;
    }
    std::string text = clip;
    if (pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller->memory().pasteText(text);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars queued from clipboard", queued);
    tapeStatusMessage = buf;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

void MainWindow::pasteFromFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) {
        tapeStatusMessage = "Paste: cannot open " + path;
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    std::string text;
    text.resize(Memory::kPasteMaxChars);
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(f.gcount()));
    const bool truncated = f.peek() != std::char_traits<char>::eof();
    if (pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller->memory().pasteText(text);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars%s from %s", queued,
                  truncated ? " (file truncated)" : "", path.c_str());
    tapeStatusMessage = buf;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

pom2::mousegrab::Context MainWindow::mouseGrabContext() const
{
    pom2::mousegrab::Context c;
    c.cardPlugged = mouseCoordinator_->capture().plugged();
    c.grabbed     = mouseGrabbed_;
    c.voxelView   = show(pom2::PanelId::Voxel);
    // Hover, NOT rect containment. `screenHovered_` is ImGui's own z-order
    // aware verdict, captured next to the screen Image (renderScreenWindow).
    // A raw "is the cursor between screenRectMin and screenRectMax" test
    // cannot see what is drawn on top: an open dropdown, a popup or a panel
    // docked over the screen all sit *inside* that rect, so every click the
    // user aimed at the menu also reached the Mouse Card — and, worse, armed
    // `shouldGrabOnPress` into capturing the pointer behind the menu.
    // The rect itself is still the right tool for the *coordinate* mapping
    // in onMouseMove; it is only wrong as an ownership test.
    c.screenHovered = screenHovered_;
    return c;
}

void MainWindow::setMouseGrab(bool on)
{
    if (on == mouseGrabbed_) return;
    if (on && !mouseCoordinator_->capture().plugged()) {
        // Capturing the pointer with nothing to feed would strand the user
        // in a hidden-cursor mode for no gain.
        tapeStatusMessage = "Mouse capture: no Mouse Card plugged "
                            "(Slot Configuration → mouse / mouseaw)";
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    mouseGrabbed_ = on;

    if (window) {
        // GLFW_CURSOR_DISABLED hides the OS cursor AND unbounds it: the
        // reported position keeps accumulating past the window edges, which
        // is exactly the infinite-delta source a relative quadrature mouse
        // wants. GLFW restores the pre-grab cursor position on the way out.
        glfwSetInputMode(window, GLFW_CURSOR,
                         on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
#ifndef __EMSCRIPTEN__
        // Raw (unaccelerated, unscaled) motion while captured — the desktop's
        // pointer-acceleration curve is tuned for a screen-sized target area,
        // and it makes the guest cursor's speed depend on how fast the user
        // flicks. Only meaningful under GLFW_CURSOR_DISABLED. The browser has
        // no equivalent knob (pointer lock already delivers raw movementX/Y).
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
                             on ? GLFW_TRUE : GLFW_FALSE);
        }
#endif
    }

    // Take the mouse away from ImGui for the duration: under a captured
    // pointer io.MousePos tracks the virtual cursor, which would hover and
    // click panels the user cannot see. The GLFW backend already skips its
    // own cursor-shape updates while GLFW_CURSOR_DISABLED is set
    // (imgui_impl_glfw.cpp `ImGui_ImplGlfw_UpdateMouseCursor`), so the two
    // do not fight over the input mode.
    ImGuiIO& io = ImGui::GetIO();
    if (on) io.ConfigFlags |=  ImGuiConfigFlags_NoMouse;
    else    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

    // Both edges warp the reported cursor position (into the virtual space on
    // entry, back to the real one on exit). Re-seed the delta baseline so the
    // first event after the transition doesn't inject that jump as motion,
    // and drop sub-pixel residue accumulated under the other regime.
    mouseInited    = false;
    mouseSubAppleX = 0.0;
    mouseSubAppleY = 0.0;

    // Never leave the guest holding a button it can no longer release.
    if (!on && mouseButtonHeld) {
        mouseButtonHeld = false;
        (void)mouseCoordinator_->routeHost(mouseAppleX, mouseAppleY, false);
    }

    tapeStatusMessage = on
        ? "Mouse captured — Ctrl+Alt+G or middle click to release"
        : "Mouse released";
    tapeStatusUntil     = lastFrameTime + (on ? 4.0 : 2.0);
    // The bar-side "how to get out" hint. 4 s was tuned for a caption
    // painted over the emulated screen, where it had to get out of the way
    // fast; in the status bar it costs only bar width, and the person who
    // needs it is the one still working out where their pointer went. Long
    // enough to notice, read and act on without it becoming furniture.
    mouseGrabHintUntil_ = on ? lastFrameTime + 30.0 : 0.0;
}

void MainWindow::toggleMouseGrab() { setMouseGrab(!mouseGrabbed_); }

void MainWindow::onWindowFocus(bool focused)
{
    if (!focused) setMouseGrab(false);
}

void MainWindow::onMouseMove(double x, double y)
{
    // First call after startup just seeds last-position; no delta yet.
    if (!mouseInited) {
        lastMouseHostX = x;
        lastMouseHostY = y;
        mouseInited = true;
        return;
    }
    const double rawDx = x - lastMouseHostX;
    const double rawDy = y - lastMouseHostY;
    lastMouseHostX = x;
    lastMouseHostY = y;

    // Either MAME-faithful MouseCard or AppleWin HLE MouseCardAppleWin can be
    // plugged (mutually exclusive). MouseCoordinator re-resolves both from the
    // live SlotBus under the machine lock, so the absolute / relative cursor
    // logic below stays variant-agnostic AND cannot write through a card that
    // a slot replacement has already destroyed.
    const pom2::mousegrab::Context grabCtx = mouseGrabContext();
    // Card plugged + (pointer captured, or hovering the screen widget).
    // Uncaptured motion outside the widget belongs to ImGui — see MouseGrab.h.
    if (!pom2::mousegrab::shouldRouteMotion(grabCtx)) return;
    auto pushMouse = [&](uint8_t rx, uint8_t ry, bool btn) {
        (void)mouseCoordinator_->routeHost(rx, ry, btn);
    };
    // Need a valid Apple II Screen widget rect to map host pixels into
    // Apple-cursor coordinates. Bail until renderScreen has populated it.
    const float widgetW = screenRectMax.x - screenRectMin.x;
    const float widgetH = screenRectMax.y - screenRectMin.y;
    if (widgetW <= 0.0f || widgetH <= 0.0f) return;

    // ── Absolute closed-loop cursor sync (AppleWin HLE only) ───────────
    // When the `mouseaw` card is plugged AND the AppleMouse firmware has
    // been turned on (MODE_MOUSE_ON, bit 0 of the latched MODE byte), the
    // card's HLE'd MCU keeps the cursor position in `iX/iY` clamped to
    // the firmware-installed window `[iMinX..iMaxX] × [iMinY..iMaxY]`.
    // We read that authoritative state via the debug snapshot, project
    // the host cursor's position onto the widget rect (saturating clamp
    // outside, so wandering out of the widget pins the Apple cursor at
    // the matching edge instead of letting it drift), and inject the
    // delta needed to drive `iX/iY` toward the projected target. The
    // earlier closed-loop attempt (reverted in commit ccd9a95) failed
    // because it assumed the clamp range equalled the display resolution
    // — that's true for the //e desktop but wrong for e.g. MousePaint's
    // 0..559 horizontal clamp. Using the card-reported clamp window
    // sidesteps that guess entirely.
    // Each push is bounded to ±127 (the MCU's 8-bit signed wrap range);
    // large gaps (first event after re-entry, big window resize) converge
    // over several events.
    bool absoluteHandled = false;
    const auto mouseInventory = mouseCoordinator_->capture();
    if (mouseInventory.appleWinPlugged &&
        pom2::mousegrab::allowAbsoluteSync(grabCtx)) {
        const auto& s = mouseInventory.appleWin;
        const bool mouseOn = s.mouseOn();
        const int rangeX = s.iMaxX - s.iMinX;
        const int rangeY = s.iMaxY - s.iMinY;
        if (mouseOn && rangeX > 0 && rangeY > 0) {
            const double fracX = std::clamp(
                (x - double(screenRectMin.x)) / double(widgetW), 0.0, 1.0);
            const double fracY = std::clamp(
                (y - double(screenRectMin.y)) / double(widgetH), 0.0, 1.0);
            const int targetX = s.iMinX + int(fracX * rangeX + 0.5);
            const int targetY = s.iMinY + int(fracY * rangeY + 0.5);
            int dx = targetX - s.iX;
            int dy = targetY - s.iY;
            if (dx >  127) dx =  127;
            if (dx < -127) dx = -127;
            if (dy >  127) dy =  127;
            if (dy < -127) dy = -127;
            mouseAppleX = static_cast<uint8_t>(mouseAppleX + dx);
            mouseAppleY = static_cast<uint8_t>(mouseAppleY + dy);
            pushMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
            // Drop relative sub-pixel residue so a later fallback (mouse
            // turned off mid-session) doesn't replay stale fractional
            // motion accumulated before sync was active.
            mouseSubAppleX = 0.0;
            mouseSubAppleY = 0.0;
            absoluteHandled = true;
        }
    }
    if (absoluteHandled) return;

    // ── Relative drive (fallback, and the only path while captured) ──
    // Used by the MAME-faithful MouseCard (no iX/iY exposed — firmware
    // lives inside the 68705P3 MCU's internal RAM), by the AppleWin HLE
    // card before the firmware enables MOUSE_ON, and by BOTH whenever the
    // pointer is captured (a grabbed cursor has no meaningful position in
    // the widget, only deltas — see MouseGrab.h). The cursor-inside-widget
    // gate was already applied by `shouldRouteMotion` above.

    // ── Speed mapping (relative drive — the only path) ──────────────
    // The closed-loop absolute sync was an experiment that didn't survive
    // contact with real apps: the cursor's real clamp range lives behind the
    // firmware (the MCU on the //e card, the internal ROM on the //c) and the
    // app's ClampMouse parameters don't reliably land in 6502-readable holes
    // for MGTK-based apps (A2Desktop/MousePaint), so any absolute target was
    // guesswork. The proven proportional drive below — what AppleWin/MAME do
    // — gives no centre-jump and lets the app's own firmware clamp at its
    // own edges naturally.
    // Used when the AppleMouse firmware is off or its clamp window is
    // non-standard (holes out of display range). Convert host-pixel
    // deltas to Apple-cursor units so 1 host pixel of motion = 1 host
    // pixel of cursor motion visually in the widget.
    //   apple_per_host_px = logical_screen_dim / widget_host_dim
    // The widget is ALWAYS drawn at kWidth(280)×kHeight(192) aspect
    // (drawScreenImage), so the X mapping must use kWidth, NOT
    // display->width() — the latter returns 560 in DHGR/80-col, which made
    // X track 2× faster than Y in 80-column mode (where A2Desktop runs).
    // Both axes now share the same logical→widget scale. Sub-pixel motion
    // accumulates across events.
    const double sxRatio = double(Apple2Display::kWidth)  / double(widgetW);
    const double syRatio = double(Apple2Display::kHeight) / double(widgetH);
    mouseSubAppleX += rawDx * sxRatio;
    mouseSubAppleY += rawDy * syRatio;
    int dxApple = static_cast<int>(mouseSubAppleX);
    int dyApple = static_cast<int>(mouseSubAppleY);
    // Clamp BEFORE consuming the sub-pixel accumulator so big jumps
    // (>127 ticks in one event, e.g. cursor teleported across widget)
    // carry the residual forward to the next event instead of being
    // silently dropped. ±127 = MCU's 8-bit signed wrap-correction range.
    if (dxApple >  127) dxApple =  127;
    if (dxApple < -127) dxApple = -127;
    if (dyApple >  127) dyApple =  127;
    if (dyApple < -127) dyApple = -127;
    mouseSubAppleX -= dxApple;
    mouseSubAppleY -= dyApple;

    mouseAppleX = static_cast<uint8_t>(mouseAppleX + dxApple);
    mouseAppleY = static_cast<uint8_t>(mouseAppleY + dyApple);
    pushMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
}

void MainWindow::onMouseButton(int button, int action)
{
    const bool press = (action != 0);   // GLFW_RELEASE = 0, others = press/repeat
    const pom2::mousegrab::Context grabCtx = mouseGrabContext();

    // Middle click TOGGLES the capture, matching what every VM viewer trains
    // into the user's fingers, and it is one of exactly two gestures that
    // can — Ctrl+Alt+G is the other. Checked first and on PRESS only:
    // releasing the wheel button must not toggle back, and while captured
    // ImGui has no mouse, so nothing else wants this event.
    //
    // A left press does NOT capture. That contract is gone on purpose: it
    // made an ordinary click silently change the meaning of every later
    // click, and the capturing press had to be swallowed (the guest cursor
    // is wherever its firmware left it, not under the host pointer), so the
    // user's first click simply vanished. Left presses now always route by
    // shouldRouteButton below and mean what they look like.
    if (pom2::mousegrab::isToggleButton(button)) {
        if (press && pom2::mousegrab::shouldToggleGrab(grabCtx))
            setMouseGrab(!mouseGrabbed_);
        return;
    }

    // Only the primary button is wired to the Apple Mouse Card (PB7 of the
    // MCU). Captured, every press is the guest's; uncaptured, only presses
    // over the Apple II Screen widget are (the rest belong to ImGui —
    // menus, panels). A RELEASE always passes through, so a button pressed
    // inside the screen but released outside still gets cleared on the card.
    if (!pom2::mousegrab::shouldRouteButton(grabCtx, button, press)) return;

    mouseButtonHeld = press;
    (void)mouseCoordinator_->routeHost(mouseAppleX, mouseAppleY,
                                       mouseButtonHeld);
}

void MainWindow::renderPasteFileDialog()
{
    if (showPasteFileDialog) {
        ImGui::OpenPopup("Paste from file");
        showPasteFileDialog = false;
    }
    if (ImGui::BeginPopupModal("Paste from file", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path to a text file (Applesoft listing, etc.)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", pasteDialogPath.c_str());
        if (ImGui::InputText("##PastePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            pasteDialogPath = buf;
        else
            pasteDialogPath = buf;
        ImGui::Checkbox("Auto-uppercase", &pasteAutoUppercase);
        ImGui::Separator();
        if (ImGui::Button("Paste", ImVec2(120, 0))) {
            if (!pasteDialogPath.empty()) pasteFromFile(pasteDialogPath);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void MainWindow::pollJoystickAndPushToMemory()
{
    joystick->poll();
    joystick->autoBindIfUnconfigured();

    // One-shot diagnostic when the bound pad (or its gamepad-mapping status)
    // changes: the kiosk Start-menu only works when gamepad-mapped=yes. If a
    // pad is present but reports "no", GLFW has no standard mapping for it,
    // so use the F1 keyboard fallback (or add an SDL mapping).
    {
        const int  hi = joystick->binding().hostIdx;
        const bool gp = joystick->activeIsGamepad();
        if (hi != loggedJoyHost_ || gp != loggedJoyGamepad_) {
            loggedJoyHost_    = hi;
            loggedJoyGamepad_ = gp;
            if (hi >= 0) {
                pom2::log().info(
                    "Joystick", "bound #" + std::to_string(hi + 1) + " '" +
                    joystick->activeName() + "' gamepad-mapped=" +
                    (gp ? "yes (Start opens kiosk disk menu)"
                        : "no (use F1 for the kiosk disk menu)"));
            } else {
                pom2::log().info("Joystick", "no pad bound");
            }
        }
    }

    // Apple II paddles (4) and push buttons (3). The Memory side already
    // handles the $C064-$C067 RC discharge model and $C061-$C063 push
    // buttons; we just hand it fresh values once per frame. Hold stateMutex
    // while writing: the CPU worker reads paddleValue/paddleButton inside
    // softSwitchAccess under the same lock (during processor.run()), so an
    // unlocked write here is a data race on those non-atomic arrays.
    // While the kiosk disk selector is open the pad drives the menu, not the
    // game: feed the Apple II centered paddles + released buttons so the A/B
    // navigation presses (which share physical buttons with PB0/PB1) don't
    // leak into the running title.
    const bool menuActive = kioskMenuOpen_;
    const JoystickInput::GamepadPlay play = joystick->play();

    // Menu → game isolation across the close. Circle/Cross double as the
    // menu's B/A and the Apple PB0/PB1, and this poll runs BEFORE
    // updateKioskMenu, so `menuActive` lags the close by a frame: the press
    // that dismissed the menu would land in the game as a fire-button hit.
    // Latch a swallow on the open→closed edge and hold it until every shared
    // button (faces + D-pad) is released. Analog paddles stay live — the
    // stick isn't a menu control and carries no edge.
    // Only gamepad-mapped pads need the latch: raw pads can't drive the menu
    // (nav requires a mapping), so a fire button held across a keyboard-
    // driven close is a legitimate game input, not a menu leftover.
    if (kioskMenuWasOpen_ && !menuActive && play.valid) kioskSwallowPad_ = true;
    kioskMenuWasOpen_ = menuActive;
    if (kioskSwallowPad_) {
        const bool anyHeld = play.valid
            ? (play.button0 || play.button1 || play.dpadUp || play.dpadDown ||
               play.dpadLeft || play.dpadRight)
            : (joystick->buttonDown(0) || joystick->buttonDown(1) ||
               joystick->buttonDown(2));
        if (!anyHeld) kioskSwallowPad_ = false;
    }
    const bool suppressGame = menuActive || kioskSwallowPad_;

    {
        auto st = controller->lockState();
        Memory& mem = st.memory();
        for (int i = 0; i < 4; ++i)
            mem.setPaddle(i, menuActive ? 128 : joystick->paddleValue(i));

        if (suppressGame) {
            for (int i = 0; i < 3; ++i) mem.setPaddleButton(i, false);
        } else if (play.valid) {
            // Gamepad-mapped: only Cross/Circle are Apple game-port buttons;
            // the other face buttons are keyboard keys (below), so PB2 is up.
            mem.setPaddleButton(0, play.button0);   // Circle → PB0
            mem.setPaddleButton(1, play.button1);   // Cross  → PB1
            mem.setPaddleButton(2, false);
        } else {
            // Raw pad (unknown layout): legacy buttons 0/1/2 → PB0/1/2.
            for (int i = 0; i < 3; ++i) mem.setPaddleButton(i, joystick->buttonDown(i));
        }
    }

    // 4play: four DIGITAL joysticks, one host pad each. Separate from the
    // block above on purpose — the Apple game port carries paddles 0-3 as
    // analogue values and the card carries four whole sticks, so they are
    // different devices reading different hardware, not two views of one.
    // Host pads 1-4 (GLFW slots 0-3) map to players 1-4 in order; the pad
    // bound to the analogue game port keeps that job as well, which is what
    // a real desk with one stick and one card would do.
    //
    // The card's four bytes are atomics (see FourPlayCard.h), so this needs
    // no lock — only the slot-bus topology read, which is UI-thread-confined
    // and allowed unlocked (CLAUDE.md § Reach the emulated state).
    for (int s2 = 1; s2 <= 7; ++s2) {
        auto* fp = dynamic_cast<pom2::FourPlayCard*>(
            controller->memory().slotBus().peripheral(s2));
        if (!fp) continue;
        for (int pl = 0; pl < pom2::FourPlayCard::kPlayers; ++pl) {
            pom2::FourPlayCard::Player st4;
            const auto& dev = joystick->deviceState(pl);
            if (dev.present && !suppressGame) {
                constexpr float kGate = 0.5f;   // digital card, analogue stick
                st4.left    = dev.axis[0] < -kGate;
                st4.right   = dev.axis[0] >  kGate;
                st4.up      = dev.axis[1] < -kGate;
                st4.down    = dev.axis[1] >  kGate;
                st4.button1 = dev.buttons[0];
                st4.button2 = dev.buttons[1];
                st4.button3 = dev.buttons[2];
            }
            fp->setPlayer(pl, st4);
        }
        break;   // one card is the whole point; a second would fight it
    }

    // Keyboard routing for the digital controls — outside stateMutex, since
    // queueKey has its own keyboard lock. Only in-game (menu closed, swallow
    // drained) and only for a gamepad-mapped pad whose layout we can trust.
    //
    // Its own reference, deliberately: the paddle block above reaches Memory
    // through the state lock, this one must NOT hold that lock (queueKey
    // takes `Memory::kbMutex`, the finer-grained one). Sharing a single
    // reference across the two, as this function used to, is what made the
    // split invisible.
    Memory& mem = controller->memory();
    if (suppressGame || !play.valid) {
        // Drop the auto-repeat history so a direction still held from menu
        // navigation re-arms cleanly (press-then-delay) once released.
        for (bool& h : padArrowHeld_) h = false;
    } else {
        if (play.spaceEdge) mem.queueKey(0x20);   // Square   → SPACE
        if (play.enterEdge) mem.queueKey(0x0D);   // Triangle → RETURN
        // D-pad → Apple II arrow codes (←$08 →$15 ↑$0B ↓$0A) with auto-repeat
        // so a held direction keeps moving, like the //e keyboard.
        const bool    held[4] = { play.dpadUp, play.dpadDown, play.dpadLeft, play.dpadRight };
        const uint8_t code[4] = { 0x0B, 0x0A, 0x08, 0x15 };
        const double  t = ImGui::GetTime();
        for (int i = 0; i < 4; ++i) {
            if (!held[i]) { padArrowHeld_[i] = false; continue; }
            if (!padArrowHeld_[i]) {                       // press: fire once
                mem.queueKey(code[i]);
                padArrowHeld_[i]  = true;
                padArrowNextT_[i] = t + 0.35;              // delay before repeat
            } else if (t >= padArrowNextT_[i]) {           // held: repeat
                mem.queueKey(code[i]);
                padArrowNextT_[i] = t + 0.06;              // ~16/s
            }
        }
    }
}

void MainWindow::pushAppleKeys()
{
    // $C061/$C062 bit 7 is one wire with two things pressing it: the host's
    // Left/Right Alt and the on-screen keyboard's latches. Either alone is
    // enough, so the sources are OR'd rather than assigned — an assignment
    // from one source silently releases the other, which is how the panel
    // used to disable Alt for the whole session (it runs every frame).
    // `openAppleKey`/`solidAppleKey` are atomics, so no lock is needed
    // (CLAUDE.md's unlocked-Memory carve-out).
    controller->memory().setOpenAppleKey (appleKeys_.openApple());
    controller->memory().setSolidAppleKey(appleKeys_.solidApple());
}

void MainWindow::onFileDrop(int count, const char** paths)
{
    if (count <= 0 || !paths) return;
    // Boot the first image whose extension/size we recognise; skip the
    // rest (a single Apple II has one boot path). insertAndBootImage takes
    // the state lock itself and must run on the UI thread — the GLFW drop
    // callback fires inside glfwPollEvents(), which the render loop drives,
    // so we are on that thread here.
    for (int i = 0; i < count; ++i) {
        if (!paths[i]) continue;
        const std::string path = paths[i];
        if (classifyDiskForSlot(path) == DiskSlotClass::Unknown) continue;
        std::string err;
        if (insertAndBootImage(path, err)) {
            tapeStatusMessage = "Dropped + booted: " +
                std::filesystem::path(path).filename().string();
            pom2::log().info("Drop", "booted dropped image: " + path);
        } else {
            tapeStatusMessage = "Drop failed: " + err;
            pom2::log().warn("Drop", "dropped image rejected: " + err);
        }
        tapeStatusUntil = lastFrameTime + 4.0;
        return;
    }
    // Nothing usable in the drop — tell the user rather than silently
    // ignoring it (the most common case is dropping a ROM or a .zip).
    tapeStatusMessage =
        "Dropped file not a disk image "
        "(.dsk/.do/.d13/.po/.nib/.woz/.hdv/.2mg)";
    tapeStatusUntil = lastFrameTime + 4.0;
}
