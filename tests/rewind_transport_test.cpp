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

// Rewind transport test (Phase 3 — controller integration).
//
// Exercises EmulationController's rewind transport against a REAL worker
// thread, pinning the pieces the UI relies on:
//   1. begin-scrub parks the worker (so a UI restore can't be overrun by an
//      in-flight Running frame);
//   2. while parked, no new frames are captured (the timeline is frozen);
//   3. seek restores exact historical state (cycle counter == frame stamp);
//   4. seekToCycle lands on the right frame;
//   5. resume truncates the abandoned future and runs again;
//   6. a resume that did NOT come through rewindEndAndResume (the toolbar
//      Play button, Machine > Run, the `machine.run` palette command, the
//      kiosk menu all call setMode(Running) directly) still ends the scrub
//      and still leaves the ring strictly increasing.
//
// The guest is a 3-byte `JMP $0800` self-loop so the CPU advances
// deterministically forever (no jam, distinct cycle stamp per frame).

#include "Block512Backing.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "RewindBuffer.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

size_t ringSize(EmulationController& ctrl)
{
    std::lock_guard<std::mutex> lk(ctrl.stateMutex());
    return ctrl.rewind().size();
}

uint64_t frameCycle(EmulationController& ctrl, size_t i)
{
    std::lock_guard<std::mutex> lk(ctrl.stateMutex());
    return ctrl.rewind().infoAt(i).cycle;
}

uint64_t liveCycle(EmulationController& ctrl)
{
    std::lock_guard<std::mutex> lk(ctrl.stateMutex());
    return ctrl.memory().getCycleCounter();
}

// Poll until the ring holds at least `target` frames, or timeout.
bool waitForFrames(EmulationController& ctrl, size_t target, int timeoutMs)
{
    for (int i = 0; i < timeoutMs; ++i) {
        if (ringSize(ctrl) >= target) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

int main()
{
    EmulationController ctrl;

    // Install a deterministic JMP-self loop at $0800 and point the CPU at it.
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        Memory& mem = ctrl.memory();
        mem.memWrite(0x0800, 0x4C);   // JMP $0800
        mem.memWrite(0x0801, 0x00);
        mem.memWrite(0x0802, 0x08);
        ctrl.cpu().setProgramCounter(0x0800);
    }

    ctrl.rewind().setMaxFrames(120);
    ctrl.rewind().setKeyframeInterval(8);   // force a mix of keyframes + deltas
    ctrl.rewind().setEnabled(true);

    ctrl.start();   // spawns the worker, arms Running

    // (1) Frames accrue while running.
    assert(waitForFrames(ctrl, 12, 4000) && "worker never captured frames");

    // (1) begin-scrub parks the worker.
    assert(ctrl.rewindBeginScrub());
    assert(ctrl.rewindIsParked() && "worker did not park after beginScrub");

    // (2) parked → timeline frozen.
    const size_t frozen = ringSize(ctrl);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    assert(ringSize(ctrl) == frozen && "ring grew while parked (capture not gated by run state)");
    assert(frozen >= 12);

    // (3) seek restores exact state: live cycle counter == the frame's stamp.
    for (size_t k : { size_t(0), frozen / 2, frozen - 1 }) {
        const size_t got = ctrl.rewindSeek(k);
        assert(got == k);
        assert(liveCycle(ctrl) == frameCycle(ctrl, k));
    }

    // (4) seekToCycle lands on the newest frame at-or-before the target.
    {
        const size_t mid = frozen / 2;
        const uint64_t target = frameCycle(ctrl, mid) + 1;   // just after frame `mid`
        const size_t got = ctrl.rewindSeekToCycle(target);
        assert(got == mid);
        assert(liveCycle(ctrl) == frameCycle(ctrl, mid));
    }

    // (5) resume from a mid point truncates the future and runs again.
    const size_t resumeAt = frozen / 2;
    const uint64_t resumeCycle = frameCycle(ctrl, resumeAt);
    ctrl.rewindEndAndResume(resumeAt);
    assert(ctrl.getMode() == EmulationController::Mode::Running);
    // The machine continues from the resumed cycle, so it must climb past it.
    bool advanced = false;
    for (int i = 0; i < 2000; ++i) {
        if (liveCycle(ctrl) > resumeCycle) { advanced = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(advanced && "machine did not resume after rewindEndAndResume");
    // And it keeps recording the new timeline.
    const size_t afterResume = ringSize(ctrl);
    assert(waitForFrames(ctrl, afterResume + 3, 2000) && "capture did not resume");

    // (6) A BARE resume — setMode(Running), no rewindEndAndResume. Four UI
    //     paths do exactly this. Two things used to break: the scrub stayed
    //     flagged live (so the panel's next drag seeked a running machine and
    //     the slider visibly did nothing), and the abandoned future stayed in
    //     the ring, so the frames captured from the rewound point carried
    //     stamps EARLIER than the tail — `indexForCycle` walks the deque
    //     expecting the opposite, so seeks landed far from the cycle asked
    //     for and the span readout lied.
    assert(waitForFrames(ctrl, 24, 4000) && "ring did not refill");
    assert(ctrl.rewindBeginScrub());
    assert(ctrl.rewindScrubbing() && "begin-scrub did not flag the scrub");
    const size_t sizeBefore = ringSize(ctrl);
    const size_t bareAt     = sizeBefore / 3;
    assert(bareAt + 4 < sizeBefore && "need an abandoned future worth dropping");
    assert(ctrl.rewindSeek(bareAt) == bareAt);
    const uint64_t bareCycle = frameCycle(ctrl, bareAt);
    assert(liveCycle(ctrl) == bareCycle);

    ctrl.setMode(EmulationController::Mode::Running);   // the toolbar Play path
    assert(!ctrl.rewindScrubbing() && "a bare resume left the scrub flagged live");

    // The first capture from the rewound point drops the future, so the ring
    // SHRINKS before it grows again — a ring that only ever grows is the bug.
    bool shrank = false;
    for (int i = 0; i < 4000 && !shrank; ++i) {
        if (ringSize(ctrl) < sizeBefore) shrank = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(shrank && "the abandoned future was never dropped");

    // Let the new timeline record a few frames, then check the invariant the
    // seek helpers rely on: stamps strictly increasing, end to end.
    assert(waitForFrames(ctrl, bareAt + 6, 4000) && "new timeline did not record");
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        const pom2::RewindBuffer& rb = ctrl.rewind();
        for (size_t i = 1; i < rb.size(); ++i)
            assert(rb.infoAt(i).cycle > rb.infoAt(i - 1).cycle &&
                   "ring stamps went backwards after a bare resume");
        assert(rb.newestCycle() >= bareCycle);
    }

    ctrl.stop();   // dtor also joins, but be explicit

    // ── WASM path: tickFrame() captures too (no worker thread) ─────────────
    // The browser build drives the CPU via tickFrame() from the render loop
    // instead of the worker. Verify the same frame-boundary capture fires.
    {
        EmulationController wasm;
        {
            std::lock_guard<std::mutex> lk(wasm.stateMutex());
            Memory& mem = wasm.memory();
            mem.memWrite(0x0800, 0x4C);
            mem.memWrite(0x0801, 0x00);
            mem.memWrite(0x0802, 0x08);
            wasm.cpu().setProgramCounter(0x0800);
        }
        wasm.rewind().setEnabled(true);
        wasm.setMode(EmulationController::Mode::Running);
        for (int i = 0; i < 25; ++i) wasm.tickFrame();   // no start(); manual frames
        assert(ringSize(wasm) >= 20 && "tickFrame did not capture rewind frames");
        // And those frames restore.
        const size_t last = ringSize(wasm) - 1;
        const uint64_t c = frameCycle(wasm, last);
        assert(wasm.rewindSeek(last) == last);
        assert(liveCycle(wasm) == c);
    }

    // ── (8) A media write clears the ring (bug hunt #2, items S3/S4/S5) ───
    //
    // The ring never captures the MEDIA of a block device (up to 32 MiB), a
    // 3.5" image (800 KB) or a writable WOZ. Rolling RAM back over a ProDOS
    // SAVE while the volume stayed written cross-links blocks on the next
    // allocation. The policy is that a rewind may never CROSS such a write:
    // the storage leaf bumps `pom2::mediaWriteEpoch()` and the controller
    // drops the history at its next capture point.
    {
        EmulationController m;
        {
            std::lock_guard<std::mutex> lk(m.stateMutex());
            Memory& mem = m.memory();
            mem.memWrite(0x0800, 0x4C);
            mem.memWrite(0x0801, 0x00);
            mem.memWrite(0x0802, 0x08);
            m.cpu().setProgramCounter(0x0800);
        }
        m.rewind().setEnabled(true);
        m.setMode(EmulationController::Mode::Running);
        for (int i = 0; i < 25; ++i) m.tickFrame();
        const size_t before = ringSize(m);
        assert(before >= 20);

        // A guest block write, straight through the leaf that every HDV /
        // CFFA / SmartPort / 3.5" / WOZ write path funnels into.
        pom2::noteMediaWrite();

        m.tickFrame();
        const size_t after = ringSize(m);
        assert(after == 1 &&
               "the rewind ring was not cleared by a media write — a scrub "
               "could still cross it");

        // Steady state: no further clearing while nothing writes.
        for (int i = 0; i < 10; ++i) m.tickFrame();
        assert(ringSize(m) > after &&
               "the ring stopped growing after a media write");
    }

    std::printf("Rewind transport: OK (park + frozen + seek + seekToCycle + "
                "resume + bare-resume + tickFrame + media-write clear)\n");
    return 0;
}
