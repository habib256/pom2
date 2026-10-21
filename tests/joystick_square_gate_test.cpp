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

// Pins the Apple II square-gate joystick model in JoystickInput.
//
// A modern analog stick is bounded by a ROUND gate: a full diagonal only
// reaches (~0.707, ~0.707), so both paddles top out near 217/255 at once and
// the four extreme corners (full X *and* full Y together) are physically
// unreachable. The original Apple II stick rode a SQUARE gate, so the corners
// (255/255) were reachable — which some games (Wings of Fury's take-off)
// require. JoystickInput::applySquareGate() expands the inscribed circle back
// out to the full square.
//
// This test exercises the two pure static helpers (applySquareGate +
// axisToPaddle01), which together encode the corner-reachability behaviour,
// plus the full stickToPaddles() composition (invert → rescaled radial
// deadzone → axis-snap notch → gate → mapping) that paddleValue() routes
// through.

#include "JoystickInput.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

bool near(float a, float b, float eps = 1e-3f)
{
    return std::fabs(a - b) <= eps;
}

// Convenience: run the gate and return the mapped paddle pair, mirroring the
// order paddleValue() applies them (gate → axisToPaddle01).
void gateToPaddle(float x, float y, uint8_t& px, uint8_t& py)
{
    JoystickInput::applySquareGate(x, y);
    px = JoystickInput::axisToPaddle01(x);
    py = JoystickInput::axisToPaddle01(y);
}

void testAxisDirectionsUntouched()
{
    // A stick pushed straight along an axis is already on the square edge:
    // the gate must be a no-op (scale factor 1), or it would distort the
    // cardinal directions.
    for (auto p : {std::pair<float,float>{ 1.0f,  0.0f},
                   {-1.0f,  0.0f},
                   { 0.0f,  1.0f},
                   { 0.0f, -1.0f}}) {
        float x = p.first, y = p.second;
        JoystickInput::applySquareGate(x, y);
        assert(near(x, p.first));
        assert(near(y, p.second));
    }
    std::printf("  ok: pure-axis directions pass through unchanged\n");
}

void testFullDiagonalsReachCorners()
{
    // The whole point: a full diagonal on a round gate (~0.707, ~0.707)
    // must expand to the true corner (1, 1) → paddle 255/255.
    const float d = 0.70710678f;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            float x = sx * d, y = sy * d;
            JoystickInput::applySquareGate(x, y);
            assert(near(x, static_cast<float>(sx)));
            assert(near(y, static_cast<float>(sy)));

            uint8_t px, py;
            gateToPaddle(sx * d, sy * d, px, py);
            assert(px == (sx > 0 ? 255 : 0));
            assert(py == (sy > 0 ? 255 : 0));
        }
    }
    std::printf("  ok: full diagonals reach the corners (255/255 etc.)\n");
}

void testRoundGateWouldFallShort()
{
    // Sanity: WITHOUT the gate, that same full diagonal only reaches ~217,
    // never 255 — this is the modern-pad limitation we are correcting.
    const float d = 0.70710678f;
    uint8_t p = JoystickInput::axisToPaddle01(d);
    assert(p > 210 && p < 224);   // ~217
    assert(p != 255);
    std::printf("  ok: ungated diagonal falls short at ~%u (not 255)\n", p);
}

void testPartialTiltScalesProportionally()
{
    // Half-magnitude diagonal (mag 0.5) → the largest component becomes the
    // magnitude (0.5), preserving the 45° direction while expanding travel.
    float x = 0.35355339f, y = 0.35355339f;   // mag = 0.5
    JoystickInput::applySquareGate(x, y);
    assert(near(x, 0.5f));
    assert(near(y, 0.5f));
    std::printf("  ok: partial diagonal scales proportionally (0.5/0.5)\n");
}

void testCenterIsStable()
{
    float x = 0.0f, y = 0.0f;
    JoystickInput::applySquareGate(x, y);   // must not divide by zero
    assert(near(x, 0.0f) && near(y, 0.0f));
    assert(JoystickInput::axisToPaddle01(0.0f) == 128);
    std::printf("  ok: center is a stable no-op (128/128, no div-by-zero)\n");
}

void testMapEndpoints()
{
    assert(JoystickInput::axisToPaddle01(-1.0f) == 0);
    assert(JoystickInput::axisToPaddle01( 0.0f) == 128);
    assert(JoystickInput::axisToPaddle01( 1.0f) == 255);
    // Out-of-range (a stick briefly reporting > 1) clamps, never wraps.
    assert(JoystickInput::axisToPaddle01( 2.0f) == 255);
    assert(JoystickInput::axisToPaddle01(-2.0f) == 0);
    std::printf("  ok: axis→paddle endpoints and clamping\n");
}

// ── stickToPaddles(): the full composition paddleValue() runs ────────────

// Default binding: dz = 0.10, no invert, square gate on.
JoystickInput::Binding bindingWith(bool gate = true, float dz = 0.10f)
{
    JoystickInput::Binding b;
    b.deadzone   = dz;
    b.squareGate = gate;
    return b;
}

void testCompositionCentersAndCorners()
{
    const JoystickInput::Binding b = bindingWith();
    uint8_t px, py;

    JoystickInput::stickToPaddles(0.0f, 0.0f, b, px, py);
    assert(px == 128 && py == 128);

    // Inside the dead zone → still centered.
    JoystickInput::stickToPaddles(0.05f, 0.05f, b, px, py);
    assert(px == 128 && py == 128);

    // NaN axes (hotplug glitch) → centered, no UB.
    JoystickInput::stickToPaddles(NAN, NAN, b, px, py);
    assert(px == 128 && py == 128);

    // Full-tilt diagonals still reach the exact corners: the rescaled
    // deadzone maps mag=1 to 1 and the notch never fires when both
    // components are comparable, so the Wings-of-Fury guarantee holds.
    const float d = 0.70710678f;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            JoystickInput::stickToPaddles(sx * d, sy * d, b, px, py);
            assert(px == (sx > 0 ? 255 : 0));
            assert(py == (sy > 0 ? 255 : 0));
        }

    // Full single-axis push still hits the rail.
    JoystickInput::stickToPaddles(1.0f, 0.0f, b, px, py);
    assert(px == 255 && py == 128);
    std::printf("  ok: composition — center, deadzone, NaN, corners, rails\n");
}

void testDeadzoneEdgeIsContinuous()
{
    // The old hard cutoff jumped ~12 counts the instant the stick left the
    // dead zone; the rescaled radial deadzone must be continuous: just past
    // the threshold reads within a couple of counts of center.
    const JoystickInput::Binding b = bindingWith();
    uint8_t px, py;

    JoystickInput::stickToPaddles(0.099f, 0.0f, b, px, py);   // just inside
    assert(px == 128 && py == 128);

    JoystickInput::stickToPaddles(0.12f, 0.0f, b, px, py);    // just outside
    assert(px >= 128 && px <= 132);
    assert(py == 128);
    std::printf("  ok: no step across the deadzone edge (128 → %u)\n", px);
}

void testAxisSnapSuppressesCrossDrift()
{
    // 5 % Y drift during a full X push must NOT creep the Y paddle (it used
    // to read ≈134 and drift games) — the notch snaps it back to center.
    const JoystickInput::Binding b = bindingWith();
    uint8_t px, py;
    JoystickInput::stickToPaddles(1.0f, 0.05f, b, px, py);
    assert(px == 255 && py == 128);

    // …but a real diagonal is untouched: comparable components never notch.
    JoystickInput::stickToPaddles(0.6f, 0.5f, b, px, py);
    assert(px > 128 && py > 128);
    std::printf("  ok: axis-snap kills cross-axis drift, spares diagonals\n");
}

void testGateOffPath()
{
    // squareGate=false: a full diagonal falls short (~217) exactly like the
    // raw round-gate hardware — pins that the toggle actually bypasses.
    const JoystickInput::Binding b = bindingWith(/*gate=*/false);
    const float d = 0.70710678f;
    uint8_t px, py;
    JoystickInput::stickToPaddles(d, d, b, px, py);
    assert(px > 210 && px < 224 && px != 255);
    assert(py > 210 && py < 224 && py != 255);
    std::printf("  ok: gate-off diagonal falls short at ~%u (not 255)\n", px);
}

void testInvert()
{
    JoystickInput::Binding b = bindingWith();
    b.invert[1] = true;
    uint8_t px, py;
    JoystickInput::stickToPaddles(0.0f, -1.0f, b, px, py);
    assert(px == 128 && py == 255);
    JoystickInput::stickToPaddles(0.0f, 1.0f, b, px, py);
    assert(px == 128 && py == 0);
    std::printf("  ok: per-axis invert flips before the pipeline\n");
}

void testFourPaddleRouting()
{
    JoystickInput::DeviceState device;
    device.present = true;
    device.axis = {0.0f, 0.0f, 0.70710678f, -0.70710678f};
    JoystickInput::Binding binding;
    assert(JoystickInput::paddleValue(device, binding, 0) == 128);
    assert(JoystickInput::paddleValue(device, binding, 1) == 128);
    assert(JoystickInput::paddleValue(device, binding, 2) == 255);
    assert(JoystickInput::paddleValue(device, binding, 3) == 0);
    binding.invert[2] = binding.invert[3] = true;
    assert(JoystickInput::paddleValue(device, binding, 2) == 0);
    assert(JoystickInput::paddleValue(device, binding, 3) == 255);
    device.axis = {1.0f, -1.0f, 0.01f, -0.01f};
    assert(JoystickInput::paddleValue(device, binding, 0) == 255);
    assert(JoystickInput::paddleValue(device, binding, 1) == 0);
    assert(JoystickInput::paddleValue(device, binding, 2) == 128);
    assert(JoystickInput::paddleValue(device, binding, 3) == 128);
    assert(JoystickInput::paddleValue(device, binding, -1) == 128);
    assert(JoystickInput::paddleValue(device, binding, 4) == 128);
    device.present = false;
    for (int i = 0; i < 4; ++i)
        assert(JoystickInput::paddleValue(device, binding, i) == 128);
    std::printf("  ok: independent sticks, right inversion/deadzone and disconnect\n");
}

// Paddle sources: the default puts the Apple II joystick (PDL 0/1) on the
// first stick and PDL 2/3 on the second; each paddle can read none, any
// stick axis, L2/R2 or a raw host axis. An unmapped pad's sticks are the raw
// axes that rest centred — an axis at -1 when the pad appears is a trigger.
void testPaddleSources()
{
    JoystickInput::Binding def;
    assert(def.source[0] == JoystickInput::SrcStick1X &&
           def.source[1] == JoystickInput::SrcStick1Y &&
           def.source[2] == JoystickInput::SrcStick2X &&
           def.source[3] == JoystickInput::SrcStick2Y);

    // Unmapped pad: raw 0/1 = stick 1, raw 2 = a trigger (rests at -1),
    // raw 3/4 = stick 2, raw 5 = the second trigger.
    JoystickInput::DeviceState d;
    d.present = true;
    d.rawCount = 6;
    d.raw = {0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f};
    for (int i = 0; i < 6; ++i) d.restsLow[i] = d.raw[i] < -0.75f;
    d.seeded = true;
    d.raw[3] = 1.0f;                    // push stick 2 right
    d.raw[2] = 1.0f;                    // squeeze L2
    JoystickInput::resolveRawSticks(d);
    assert(d.axis[2] == 1.0f && "stick 2 X is raw 3, not the trigger at raw 2");
    assert(d.trigger[0] == 1.0f && d.trigger[1] == -1.0f);

    JoystickInput::Binding b;
    assert(JoystickInput::paddleValue(d, b, 2) == 255);
    b.source[2] = JoystickInput::SrcTriggerL;
    assert(JoystickInput::paddleValue(d, b, 2) == 255);
    b.source[2] = JoystickInput::SrcNone;
    assert(JoystickInput::paddleValue(d, b, 2) == 128);
    b.source[0] = JoystickInput::SrcRaw0 + 3;          // a raw axis, directly
    assert(JoystickInput::paddleValue(d, b, 0) == 255);
    b.source[0] = JoystickInput::SrcRaw0 + 7;          // beyond rawCount
    assert(JoystickInput::paddleValue(d, b, 0) == 128);
    assert(JoystickInput::validSource(JoystickInput::SrcRaw0 + 7) &&
           !JoystickInput::validSource(JoystickInput::SrcRaw0 + 8) &&
           !JoystickInput::validSource(7));
    std::printf("  ok: paddle sources (default sticks, triggers, raw, none)\n");
}

} // namespace

int main()
{
    std::printf("joystick_square_gate:\n");
    testAxisDirectionsUntouched();
    testFullDiagonalsReachCorners();
    testRoundGateWouldFallShort();
    testPartialTiltScalesProportionally();
    testCenterIsStable();
    testMapEndpoints();
    testCompositionCentersAndCorners();
    testDeadzoneEdgeIsContinuous();
    testAxisSnapSuppressesCrossDrift();
    testGateOffPath();
    testInvert();
    testFourPaddleRouting();
    testPaddleSources();
    std::printf("joystick_square_gate: all assertions passed\n");
    return 0;
}
