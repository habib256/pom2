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

// Two-finger pinch zoom policy — pins src/PinchZoom.h.
//
// What a host reports becomes one log-scale delta per frame, handed to the
// one view under the fingers and to nobody else:
//
//   • the platform sign is kept (spread = zoom in), and the setting
//     `pinch_zoom_inverted` flips it;
//   • every host unit converts to the same scale: a Ctrl+wheel notch is
//     ×1.25, a macOS magnification m is ×(1+m), a distance ratio is itself,
//     a browser pixel pinch follows the browser's own exp(-dy/100);
//   • a NaN, an infinity or a huge burst cannot leap the view (×4 a frame);
//   • a frame's events accumulate, the last focal point wins, and a gesture
//     no view took is dropped at the next frame;
//   • the target is the window under the focal point or one of its
//     ancestors, AND the view's rect must contain the point; one taker only;
//   • a touch pinch's request to drop the first finger's press survives the
//     frame boundary and is taken once;
//   • a discrete ladder steps once per ×1.25, carries the remainder, and a
//     reversal does not inherit the other direction's remainder.

#include "PinchZoom.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace pz = pom2::pinch;

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

const pz::Rect kView{100.0f, 100.0f, 500.0f, 400.0f};
constexpr std::uint64_t kWin = 42, kChild = 43, kOther = 7;

void testConversions()
{
    CHECK(near(pz::wheelToLog(1.0f), std::log(1.25f)));
    CHECK(near(pz::wheelToLog(-2.0f), -2.0f * std::log(1.25f)));
    CHECK(near(pz::magnificationToLog(0.1), std::log(1.1f)));
    CHECK(near(pz::magnificationToLog(-0.1), std::log(0.9f)));
    CHECK(near(pz::ratioToLog(2.0), std::log(2.0f)));
    CHECK(pz::ratioToLog(0.0) == 0.0f);                 // a degenerate distance
    CHECK(pz::ratioToLog(-1.0) == 0.0f);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(pz::ratioToLog(nan) == 0.0f);
    CHECK(pz::magnificationToLog(nan) == 0.0f);
    CHECK(pz::magnificationToLog(inf) == 0.0f);
    CHECK(pz::wheelToLog(std::numeric_limits<float>::infinity()) == 0.0f);
    // Clamped to ×4 either way.
    CHECK(near(pz::ratioToLog(1000.0), pz::kMaxLog));
    CHECK(near(pz::magnificationToLog(-0.99), -pz::kMaxLog));
    CHECK(near(pz::wheelToLog(100.0f), pz::kMaxLog));
    // Browser: pixel pinches follow exp(-dy/100); a 100 px "notch" is capped
    // to 25 px; lines are thirds of a notch; pages are notches.
    CHECK(near(pz::domCtrlWheelToLog(-5.0, 0), 0.05f));
    CHECK(near(pz::domCtrlWheelToLog(5.0, 0), -0.05f));
    CHECK(near(pz::domCtrlWheelToLog(100.0, 0), -0.25f));
    CHECK(near(pz::domCtrlWheelToLog(-3.0, 1), std::log(1.25f)));
    CHECK(near(pz::domCtrlWheelToLog(1.0, 2), -std::log(1.25f)));
    CHECK(pz::domCtrlWheelToLog(nan, 0) == 0.0f);
    // Only Ctrl turns a wheel into a zoom.
    CHECK(pz::wheelIsZoom(true, 1.0f));
    CHECK(!pz::wheelIsZoom(false, 1.0f));
    CHECK(!pz::wheelIsZoom(true, 0.0f));
}

void testSignAndInversion()
{
    pz::Router r;
    r.push(pz::ratioToLog(2.0));                         // fingers apart
    r.beginFrame({200.0f, 200.0f});
    r.setHitChain({kWin});
    pz::Gesture g;
    CHECK(r.take(kWin, kView, g));
    CHECK(g.logScale > 0.0f);                            // platform convention: in
    CHECK(near(g.factor(), 2.0f));

    r.setInverted(true);
    r.push(pz::ratioToLog(2.0));
    r.beginFrame({200.0f, 200.0f});
    r.setHitChain({kWin});
    CHECK(r.take(kWin, kView, g));
    CHECK(near(g.factor(), 0.5f));                       // the opt-in reverses it
    CHECK(r.inverted());
}

void testAccumulationAndFocal()
{
    pz::Router r;
    r.push(pz::wheelToLog(1.0f));
    r.push(pz::wheelToLog(1.0f));
    r.pushAt(pz::ratioToLog(1.0), {300.0f, 300.0f});    // ratio 1: no zoom, no focal
    r.pushAt(pz::ratioToLog(1.1), {250.0f, 260.0f});
    r.beginFrame({120.0f, 130.0f});
    CHECK(r.pending());
    CHECK(near(r.focal().x, 250.0f) && near(r.focal().y, 260.0f));
    r.setHitChain({kWin});
    pz::Gesture g;
    CHECK(r.take(kWin, kView, g));
    CHECK(near(g.factor(), 1.25f * 1.25f * 1.1f, 1e-3f));

    // No focal reported: the pointer stands in.
    r.push(pz::wheelToLog(1.0f));
    r.beginFrame({120.0f, 130.0f});
    CHECK(near(r.focal().x, 120.0f) && near(r.focal().y, 130.0f));

    // A burst is bounded to ×4 for the frame.
    for (int i = 0; i < 50; ++i) r.push(pz::wheelToLog(1.0f));
    r.beginFrame({200.0f, 200.0f});
    r.setHitChain({kWin});
    CHECK(r.take(kWin, kView, g));
    CHECK(near(g.logScale, pz::kMaxLog));

    // Nothing queued: nothing pending.
    r.beginFrame({200.0f, 200.0f});
    CHECK(!r.pending());
}

void testTargeting()
{
    pz::Router r;
    pz::Gesture g;

    // The window under the fingers is a CHILD of the view's window: taken.
    r.push(pz::wheelToLog(1.0f));
    r.beginFrame({200.0f, 200.0f});
    r.setHitChain({kChild, kWin});
    CHECK(!r.take(kOther, kView, g));                    // another panel: never
    CHECK(r.take(kWin, kView, g));
    CHECK(!r.take(kWin, kView, g));                      // one taker a frame

    // Right window, but the point is outside the view's rect (its toolbar).
    r.push(pz::wheelToLog(1.0f));
    r.beginFrame({50.0f, 50.0f});
    r.setHitChain({kWin});
    CHECK(!r.take(kWin, kView, g));

    // Inside the rect, but another window is in front (no chain to ours).
    r.push(pz::wheelToLog(1.0f));
    r.beginFrame({200.0f, 200.0f});
    r.setHitChain({kOther});
    CHECK(!r.take(kWin, kView, g));

    // Nothing hit at all (a modal elsewhere, the desktop gap).
    r.push(pz::wheelToLog(1.0f));
    r.beginFrame({200.0f, 200.0f});
    CHECK(!r.take(kWin, kView, g));

    // Untaken gestures never leak into the next frame.
    r.beginFrame({200.0f, 200.0f});
    r.setHitChain({kWin});
    CHECK(!r.pending());
    CHECK(!r.take(kWin, kView, g));
}

void testFit()
{
    pz::Router r;
    pz::Gesture g;
    r.pushFit();
    r.beginFrame({200.0f, 200.0f});
    r.setHitChain({kWin});
    CHECK(r.pending());
    CHECK(r.take(kWin, kView, g));
    CHECK(g.fit && g.logScale == 0.0f && g.active());
    r.beginFrame({200.0f, 200.0f});
    CHECK(!r.pending());
}

void testPressCancel()
{
    // A second finger landing asks for the first one's press to be dropped.
    // The request survives the frame boundary (beginFrame drops gestures,
    // not this) and is taken exactly once.
    pz::Router r;
    CHECK(!r.takePressCancel());
    r.cancelPress();
    r.beginFrame({0.0f, 0.0f});
    CHECK(r.takePressCancel());
    CHECK(!r.takePressCancel());
}

void testLadder()
{
    pz::LadderAccumulator a;
    const float notch = pz::kWheelNotchLog;
    CHECK(a.feed(notch) == 1);
    CHECK(a.feed(notch * 0.5f) == 0);
    CHECK(a.feed(notch * 0.6f) == 1);                    // the remainder carried
    CHECK(a.feed(-notch * 0.5f) == 0);                   // reversal: from zero
    CHECK(a.feed(-notch * 0.6f) == -1);
    CHECK(a.feed(notch * 3.2f) == 3);
    CHECK(a.feed(0.0f) == 0);
}

} // namespace

int main()
{
    testConversions();
    testSignAndInversion();
    testAccumulationAndFocal();
    testTargeting();
    testFit();
    testPressCancel();
    testLadder();
    if (failures) {
        std::printf("pinch_zoom_policy: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("pinch_zoom_policy: all checks passed\n");
    return 0;
}
