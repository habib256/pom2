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

// Disk-turbo speed override composes with the base CPU budget.
//
// `MainWindow::updateAutoTurbo` cranks the machine to ~60x while a drive
// streams. It used to do that by stashing `getCyclesPerFrame()` in a
// MainWindow member and writing 1 000 000, then writing the member back when
// the drive went idle. Anything that changed the machine's speed DURING the
// burst was therefore silently reverted on exit:
//
//   * a profile switch mid-load (applyProfile writes the new profile's
//     defaultCyclesPerFrame — 20313 on PAL, 68180 on a //c+) came back as
//     whatever was current when the motor spun up;
//   * `setVideoStandard` likewise;
//   * a speed-menu / AI `/speed` change during a load never took effect at
//     all — the exit overwrote it.
//
// The override now lives in the controller: `setCyclesPerFrame` records the
// BASE, `setTurboOverride` / `clearTurboOverride` layer on top, and
// `getCyclesPerFrame` reports the EFFECTIVE budget the worker burns (which is
// what the toolbar's speed label and the AI `/speed` reply have always read).

#include "EmulationController.h"

#include <cassert>
#include <cstdio>

int main()
{
    EmulationController ctrl;

    // Base is the effective value while no override is engaged.
    ctrl.setCyclesPerFrame(17045);
    assert(ctrl.getCyclesPerFrame() == 17045);
    assert(ctrl.getBaseCyclesPerFrame() == 17045);
    assert(!ctrl.turboOverrideActive());

    // Engage turbo: the effective budget jumps, the base is remembered.
    ctrl.setTurboOverride(1'000'000);
    assert(ctrl.turboOverrideActive());
    assert(ctrl.getCyclesPerFrame() == 1'000'000);
    assert(ctrl.getBaseCyclesPerFrame() == 17045);

    // A speed change DURING the burst (speed menu, AI /speed, CLI --speed)
    // updates the base without disturbing the override.
    ctrl.setCyclesPerFrame(34090);              // 2x
    assert(ctrl.getCyclesPerFrame() == 1'000'000 &&
           "turbo still owns the effective budget");
    assert(ctrl.getBaseCyclesPerFrame() == 34090);

    // A profile switch during the burst goes through the same setter.
    ctrl.setCyclesPerFrame(20313);              // PAL default
    assert(ctrl.getCyclesPerFrame() == 1'000'000);

    // Turbo lifts → the machine lands on what was last asked for, NOT on the
    // value captured when the motor spun up. This is the assertion that
    // fails on the old MainWindow-member shape (it restored 17045).
    ctrl.clearTurboOverride();
    assert(!ctrl.turboOverrideActive());
    assert(ctrl.getCyclesPerFrame() == 20313 &&
           "turbo exit must restore the CURRENT base, not a stale capture");

    // Idempotence: a second clear (updateAutoTurbo's `turboEligible == false`
    // branch can reach it) must not move anything.
    ctrl.clearTurboOverride();
    assert(ctrl.getCyclesPerFrame() == 20313);

    // A //c+ style 4x base survives a turbo round trip untouched.
    ctrl.setCyclesPerFrame(68180);
    ctrl.setTurboOverride(1'000'000);
    ctrl.clearTurboOverride();
    assert(ctrl.getCyclesPerFrame() == 68180);

    std::puts("OK disk_turbo_speed (override composes with the base budget)");
    return 0;
}
