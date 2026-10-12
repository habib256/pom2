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

// Mouse Card pointer-capture policy — pins src/MouseGrab.h.
//
// The grab is the one host-input mode that can leave a user with no visible
// cursor and no obvious way back, so what this pins is exactly the set of
// rules that keep it escapable and honest:
//
//   • the release chord and the release button really do match (and match
//     nothing else) — a typo here strands the pointer;
//   • the capturing click is distinguishable from a normal click, because
//     the caller must SWALLOW the former (the guest cursor is not under the
//     host pointer, so forwarding it would click at an arbitrary spot);
//   • a button RELEASE always reaches the card, from anywhere, so the guest
//     can never be left holding a button down;
//   • capture bypasses the hover gate on motion (that gate is the
//     whole reason to capture: it is what makes the guest cursor unable to
//     reach its own clamp edges) while a non-captured pointer keeps the
//     pre-grab contract;
//   • an open dropdown / popup / docked panel drawn OVER the screen owns
//     its own clicks — `screenHovered` is a z-order aware hover, not a rect
//     containment test (see testUiOverlayOwnsItsClicks);
//   • absolute closed-loop sync is off while captured — under
//     GLFW_CURSOR_DISABLED the reported position is virtual and maps to no
//     point on screen.
//
// MouseGrab.h stays GLFW-free so this test links no windowing stack; the
// mirrored GLFW token values are re-asserted here, and MainWindow.cpp
// static_asserts them against the real <GLFW/glfw3.h>.

#include "MouseGrab.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

namespace mg = pom2::mousegrab;

namespace {

// A Context with a Mouse Card plugged and the cursor over the screen —
// the state every "does this fire?" case starts from.
mg::Context hovering()
{
    mg::Context c;
    c.cardPlugged  = true;
    c.screenHovered = true;
    return c;
}

void testToggleChord()
{
    // GLFW folds left and right Alt into one modifier bit, so one test
    // covers both; extra modifiers must not break the escape hatch.
    assert(mg::isToggleChord(mg::kKeyG, mg::kModControl | mg::kModAlt));
    assert(mg::isToggleChord(mg::kKeyG,
                             mg::kModControl | mg::kModAlt | 0x0001 /*shift*/));
    assert(mg::isToggleChord(mg::kKeyG,
                             mg::kModControl | mg::kModAlt | 0x0008 /*super*/));

    // Neither half alone: plain Ctrl-G must keep reaching the Apple II
    // keyboard latch as $07 (BELL), and Alt-G is Open-Apple + G.
    assert(!mg::isToggleChord(mg::kKeyG, mg::kModControl));
    assert(!mg::isToggleChord(mg::kKeyG, mg::kModAlt));
    assert(!mg::isToggleChord(mg::kKeyG, 0));
    // Not some neighbouring key with the right modifiers.
    assert(!mg::isToggleChord(mg::kKeyG + 1, mg::kModControl | mg::kModAlt));
    assert(!mg::isToggleChord(mg::kKeyG - 1, mg::kModControl | mg::kModAlt));
}

void testToggleButton()
{
    // The middle button is the only button that carries the toggle. The
    // LEFT button is the point of this test: it must never appear here, so
    // no left click can capture the pointer.
    assert(mg::isToggleButton(mg::kButtonMiddle));
    assert(!mg::isToggleButton(mg::kButtonLeft));
    assert(!mg::isToggleButton(1));   // right
    assert(!mg::isToggleButton(3));   // thumb buttons and beyond
}

void testToggleGrab()
{
    // Capture: needs a card to capture for. Hover is NOT required — the
    // gesture is explicit, and refusing it a few pixels off the widget
    // would read as the toggle being broken.
    mg::Context c = hovering();
    assert(mg::shouldToggleGrab(c));
    mg::Context offWidget = c; offWidget.screenHovered = false;
    assert(mg::shouldToggleGrab(offWidget));

    // No card: nothing to capture for.
    mg::Context noCard = c; noCard.cardPlugged = false;
    assert(!mg::shouldToggleGrab(noCard));

    // The voxel view owns middle-drag (pan), so a middle click must not arm
    // a grab there. Ctrl+Alt+G still can — it cannot be confused with a drag.
    mg::Context voxel = c; voxel.voxelView = true;
    assert(!mg::shouldToggleGrab(voxel));

    // RELEASE is never refused, whatever the state. An escape hatch that
    // any condition can block is not an escape hatch: each of these would
    // otherwise strand a captured pointer with the OS cursor hidden.
    mg::Context grabbed = c;            grabbed.grabbed = true;
    assert(mg::shouldToggleGrab(grabbed));
    mg::Context grabbedNoCard = grabbed; grabbedNoCard.cardPlugged = false;
    assert(mg::shouldToggleGrab(grabbedNoCard));
    mg::Context grabbedVoxel = grabbed;  grabbedVoxel.voxelView = true;
    assert(mg::shouldToggleGrab(grabbedVoxel));
    mg::Context grabbedOff = grabbed;    grabbedOff.screenHovered = false;
    assert(mg::shouldToggleGrab(grabbedOff));
}

// A left press must reach the CARD, never the capture. This is the
// regression the removal of click-to-grab exists to pin: with the old
// policy the first left press over the screen was swallowed to arm a grab,
// so the guest never saw it.
void testLeftClickNeverCaptures()
{
    // The left button carries no toggle, in any state — the policy has no
    // path at all from a left press to a capture.
    assert(!mg::isToggleButton(mg::kButtonLeft));

    // Hovering, card plugged, not grabbed — the exact state the old policy
    // captured in, where the press used to be swallowed. It reaches the
    // card now.
    mg::Context c = hovering();
    assert(mg::shouldRouteButton(c, mg::kButtonLeft, /*press=*/true));
}

void testMotionRouting()
{
    mg::Context c = hovering();
    assert(mg::shouldRouteMotion(c));

    // Uncaptured and off the widget: ImGui's, so the user can still reach
    // the menus and panels.
    mg::Context outside = c; outside.screenHovered = false;
    assert(!mg::shouldRouteMotion(outside));

    // Captured: the widget rect stops meaning anything — this is the whole
    // point of the mode.
    mg::Context grabbedOutside = outside; grabbedOutside.grabbed = true;
    assert(mg::shouldRouteMotion(grabbedOutside));

    // ... but never with no card to feed.
    mg::Context noCard = grabbedOutside; noCard.cardPlugged = false;
    assert(!mg::shouldRouteMotion(noCard));
}

void testButtonRouting()
{
    mg::Context c = hovering();
    assert(mg::shouldRouteButton(c, mg::kButtonLeft, /*press=*/true));

    // Only the primary button is wired (PB7 of the MCU).
    assert(!mg::shouldRouteButton(c, mg::kButtonMiddle, true));
    assert(!mg::shouldRouteButton(c, 1, true));
    assert(!mg::shouldRouteButton(c, 1, false));

    // A press outside the widget belongs to ImGui...
    mg::Context outside = c; outside.screenHovered = false;
    assert(!mg::shouldRouteButton(outside, mg::kButtonLeft, true));
    // ...but its RELEASE still passes through: pressed inside, released
    // outside must not leave the button stuck down in the guest.
    assert(mg::shouldRouteButton(outside, mg::kButtonLeft, false));

    // Captured: every press is the guest's, wherever the virtual cursor is.
    mg::Context grabbedOutside = outside; grabbedOutside.grabbed = true;
    assert(mg::shouldRouteButton(grabbedOutside, mg::kButtonLeft, true));

    // No card, nothing routed — not even a release.
    mg::Context noCard = c; noCard.cardPlugged = false;
    assert(!mg::shouldRouteButton(noCard, mg::kButtonLeft, true));
    assert(!mg::shouldRouteButton(noCard, mg::kButtonLeft, false));
}

// Regression: a dropdown menu, popup or docked panel drawn over the Apple II
// screen must own every click aimed at it.
//
// The bug this pins was NOT in the policy below — it was in how MainWindow
// filled the Context. `screenHovered` used to be computed as raw rect
// containment ("is the cursor between screenRectMin and screenRectMax?"),
// and a menu drawn on top of the screen is geometrically *inside* that rect.
// So opening the Machine menu and picking an item that happened to overlap
// the screen fired the menu item AND handed the press to the Mouse Card —
// and, with click-to-grab on, captured the pointer behind the still-open
// menu, hiding the cursor with no visible cause.
//
// The contract is therefore on the *field*: `screenHovered` means "ImGui
// says the pointer is on the screen widget", which is false whenever
// anything of the UI is on top. Everything below is what that buys, and it
// is only reachable if the caller honours the field's documented meaning —
// which is why `Context::screenHovered` is documented as a hover, why the
// name no longer says "inside", and why MainWindow.cpp feeds it from
// `ImGui::IsItemHovered()` next to the screen Image.
void testUiOverlayOwnsItsClicks()
{
    // The user opens a menu that overlaps the screen and clicks an item.
    // Same cursor coordinates as `hovering()` — only the ownership differs.
    mg::Context menuOverScreen = hovering();
    menuOverScreen.screenHovered = false;

    // The press is ImGui's: it must not reach the card. (It cannot capture
    // the pointer either, but that is now true of every left press
    // everywhere — see testLeftClickNeverCaptures.)
    assert(!mg::shouldRouteButton(menuOverScreen, mg::kButtonLeft, true));
    // Moving along the menu items is the UI's too — otherwise the guest
    // cursor tracks the user's menu navigation.
    assert(!mg::shouldRouteMotion(menuOverScreen));

    // The release still passes through, exactly as for any other off-widget
    // release: a button pressed on the screen and released after a menu
    // opened over it must not stick down in the guest.
    assert(mg::shouldRouteButton(menuOverScreen, mg::kButtonLeft, false));

    // Already captured, the UI has no pointer at all (GLFW_CURSOR_DISABLED),
    // so nothing can be drawn "over" anything as far as the guest cares —
    // the grab keeps winning and the escape hatch stays the chord.
    mg::Context grabbed = menuOverScreen; grabbed.grabbed = true;
    assert(mg::shouldRouteButton(grabbed, mg::kButtonLeft, true));
    assert(mg::shouldRouteMotion(grabbed));
}

void testAbsoluteSyncGate()
{
    mg::Context c = hovering();
    assert(mg::allowAbsoluteSync(c));
    mg::Context grabbed = c; grabbed.grabbed = true;
    assert(!mg::allowAbsoluteSync(grabbed));
}

void testGlfwTokenMirrors()
{
    // MouseGrab.h cannot include <GLFW/glfw3.h> (this test links no GLFW),
    // so the values are mirrored there. MainWindow.cpp static_asserts them
    // against the real header; this catches an edit to the mirrors alone.
    static_assert(mg::kKeyG         == 71);
    static_assert(mg::kModControl   == 0x0002);
    static_assert(mg::kModAlt       == 0x0004);
    static_assert(mg::kButtonLeft   == 0);
    static_assert(mg::kButtonMiddle == 2);
}

void testFullscreenChord()
{
    for (int key : {mg::kKeyEnter, mg::kKeyKpEnter}) {
        assert(mg::isFullscreenChord(key, mg::kModAlt));
        assert(mg::isFullscreenChord(key, mg::kModAlt | 0x0001));
        assert(!mg::isFullscreenChord(key, 0));
        assert(!mg::isFullscreenChord(key, mg::kModControl));
        assert(!mg::isFullscreenChord(key, mg::kModAlt | mg::kModControl));
        assert(!mg::isFullscreenChord(key, mg::kModAlt | mg::kModSuper));
    }
    assert(!mg::isFullscreenChord(mg::kKeyG, mg::kModAlt));
}

} // namespace

int main()
{
    testToggleChord();
    testFullscreenChord();
    testToggleButton();
    testToggleGrab();
    testLeftClickNeverCaptures();
    testMotionRouting();
    testButtonRouting();
    testUiOverlayOwnsItsClicks();
    testAbsoluteSyncGate();
    testGlfwTokenMirrors();
    std::printf("OK\n");
    return 0;
}
