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

#pragma once

// Host-pointer capture ("mouse grab") policy for the Apple II Mouse Card.
//
// One concern: *deciding* what a host input event means while the pointer is
// (or isn't) captured. Actually flipping GLFW's cursor mode, hiding the OS
// cursor and feeding the card lives in MainWindow — this header only answers
// the questions, so the decisions stay pinnable by a headless test that links
// no windowing stack.
//
// Why grab at all? The Mouse Card (both variants) is a purely *relative*
// quadrature device: MainWindow converts host-pixel deltas into Apple mouse
// units and the guest firmware clamps them at its own window edges. Without a
// grab, the host pointer hits the edge of the Apple II Screen widget (or of
// the desktop) long before the guest cursor reaches the edge of its clamp
// window, and every further delta is lost — the two cursors desynchronise and
// the guest cursor can become unable to reach a menu bar or a scroll gutter.
// Capturing the pointer (GLFW_CURSOR_DISABLED) removes the boundary entirely:
// GLFW reports an unbounded virtual position, so deltas keep flowing in every
// direction for as long as the user keeps moving.
//
// Capture is entered and left by deliberate gestures, the ones
// every VM viewer and PC emulator already trains into the user's fingers:
// **Ctrl+Alt+G**, or a click on the **middle button (wheel)**. Both TOGGLE.
// **Alt+Enter** enters full screen with capture, then returns to the windowed
// interface with a free pointer, including when there is no Mouse Card.
//
// A left click deliberately does NOT capture. It used to, which is the
// classic "click the screen to give the guest your mouse" contract, but it
// makes an ordinary click silently change what every later click means — and
// the capturing click has to be swallowed to avoid firing the guest's button
// at an arbitrary spot, so the user's first click just vanishes. Left clicks
// now always mean what they look like: a click for the guest when the screen
// is hovered (or captured), an ImGui click otherwise.
//
// A useful consequence: capture can only be ENTERED by the same gesture that
// LEAVES it, so anyone who is captured already knows how to get out. That is
// what lets the on-screen caption go away — the reminder lives in the status
// bar, and discoverability no longer has to be solved before a stray click.

namespace pom2 {
namespace mousegrab {

// Mirrors of the GLFW tokens this policy compares against. Kept as literals
// so the header stays GLFW-free (tests link it without GLFW/OpenGL);
// MainWindow.cpp static_asserts every one of them against <GLFW/glfw3.h>, so
// a token change upstream is a compile error rather than a silent desync.
inline constexpr int kKeyG         = 71;      // GLFW_KEY_G
inline constexpr int kKeyEnter     = 257;     // GLFW_KEY_ENTER
inline constexpr int kKeyKpEnter   = 335;     // GLFW_KEY_KP_ENTER
inline constexpr int kModControl   = 0x0002;  // GLFW_MOD_CONTROL
inline constexpr int kModAlt       = 0x0004;  // GLFW_MOD_ALT
inline constexpr int kModSuper     = 0x0008;  // GLFW_MOD_SUPER
inline constexpr int kButtonLeft   = 0;       // GLFW_MOUSE_BUTTON_LEFT
inline constexpr int kButtonMiddle = 2;       // GLFW_MOUSE_BUTTON_MIDDLE

/// Ctrl+Alt+G — grab/release toggle. Matched on either Alt (GLFW folds
/// left and right Alt into the same GLFW_MOD_ALT bit) and regardless of
/// Shift/Super, so a stray modifier can never strand a captured pointer.
/// Must be tested BEFORE the Ctrl-letter path that injects $01..$1A into
/// the Apple II keyboard latch, or the chord would also type Ctrl-G ($07).
inline bool isToggleChord(int key, int mods)
{
    return key == kKeyG && (mods & kModControl) != 0 && (mods & kModAlt) != 0;
}

/// Alt+Enter toggles between full screen with capture and a free windowed UI.
/// The caller consumes every edge so no Return reaches the guest.
/// Only PRESS changes the mode.
inline bool isFullscreenChord(int key, int mods)
{
    return (key == kKeyEnter || key == kKeyKpEnter) && (mods & kModAlt) &&
           !(mods & (kModControl | kModSuper));
}

/// Middle button (wheel click) — the button that carries the toggle.
inline bool isToggleButton(int button) { return button == kButtonMiddle; }

/// Everything the policy needs to know about the current host/UI state.
struct Context {
    bool cardPlugged  = false;  ///< MouseCard or MouseCardAppleWin on the bus
    bool grabbed      = false;  ///< host pointer currently captured
    /// Host cursor over the Apple II Screen widget **and nothing of the UI's
    /// on top of it**. This MUST come from a z-order-aware hover test
    /// (`ImGui::IsItemHovered()` on the screen image), never from a raw
    /// "is the cursor between screenRectMin and screenRectMax" containment
    /// test: a dropdown menu, a popup or a docked panel drawn over the
    /// screen is geometrically *inside* that rect, so a rect test hands the
    /// guest every click the user aims at the menu sitting on top of it.
    bool screenHovered = false;
    bool voxelView    = false;  ///< 3D voxel view owns the drag gestures
};

/// What a middle click should do. Releasing is ALWAYS allowed — an escape
/// hatch that can be blocked by state is not an escape hatch, so nothing
/// here (no card, no hover, voxel view up) may refuse to give the pointer
/// back. Capturing is narrower: it needs a card to capture *for*, and is
/// suppressed under the 3D voxel view, where middle-drag pans the camera and
/// arming a grab would swallow the gesture. Ctrl+Alt+G still captures there,
/// because that one cannot be confused with a drag.
///
/// Screen hover is deliberately NOT required: the user is asking for the
/// pointer by an explicit gesture, and refusing because it happened a few
/// pixels off the widget would read as the toggle being broken.
inline bool shouldToggleGrab(const Context& c)
{
    if (c.grabbed) return true;              // release: never refused
    return c.cardPlugged && !c.voxelView;    // capture: needs a target
}

/// Motion routing. Grabbed: every delta is the guest's, wherever GLFW's
/// virtual cursor has wandered to — hover stops meaning anything once the
/// pointer is captured. Not grabbed: the pre-grab contract, i.e. only motion
/// actually hovering the screen widget, so the host can still drive ImGui
/// panels — including the ones drawn *over* the screen.
inline bool shouldRouteMotion(const Context& c)
{
    return c.cardPlugged && (c.grabbed || c.screenHovered);
}

/// Button routing to the card. Only the primary button is wired (PB7 of the
/// MCU). A release ALWAYS passes through even from off the widget, so a
/// button pressed over the screen and released elsewhere still clears on
/// the card instead of sticking down in the guest.
inline bool shouldRouteButton(const Context& c, int button, bool press)
{
    if (button != kButtonLeft || !c.cardPlugged) return false;
    if (!press)                                  return true;
    return c.grabbed || c.screenHovered;
}

/// The AppleWin HLE card's absolute closed-loop sync projects the host
/// cursor's position-in-widget onto the firmware clamp window. Under a grab
/// GLFW reports an unbounded virtual position that no longer corresponds to
/// any point on screen, so that projection is meaningless — a grabbed pointer
/// always drives the relative path.
inline bool allowAbsoluteSync(const Context& c) { return !c.grabbed; }

} // namespace mousegrab
} // namespace pom2
