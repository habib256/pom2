// EmulationController run-control pins — bug hunt #11.
//
//  1. A worker that DIED inside the exception barrier is still joinable; the
//     next start() must reap it and spawn a replacement, and the next stop()
//     must return (it used to spin on the UI thread for ever: the window
//     wedged, every unflushed disk write lost).
//  2. The worker's Step retirement must not clobber a concurrent Run.
//  3. A Stop cancels the queued single-steps (a `--step 20000` interrupted by
//     Stop used to leave its remainder for the next Step press).
//  4. bootFromSlot() from a paused machine un-suspends the audio bus.

#include "AudioDevice.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

class ThrowOnceCard final : public SlotPeripheral {
public:
    std::string_view name() const override { return "throw-once"; }
    uint8_t deviceSelectRead(uint8_t) override {
        if (++n_ == 500) throw std::bad_alloc();   // e.g. a rewind capture
        return 0xFF;
    }
private:
    int n_ = 0;
};

class BootableCard final : public SlotPeripheral {
public:
    std::string_view name() const override { return "bootable-stub"; }
    uint8_t slotRomRead(uint8_t low8) override {
        switch (low8) {
            case 1: return 0x20;   // Apple II JSR-dispatch signature
            case 3: return 0x00;
            case 5: return 0x03;
            default: return 0x60;  // RTS
        }
    }
};

void poke(EmulationController& ctrl, const uint8_t* code, size_t n, uint16_t at = 0x0800)
{
    auto st = ctrl.lockState();
    for (size_t i = 0; i < n; ++i) st.memory().memWrite(static_cast<uint16_t>(at + i), code[i]);
    st.cpu().setProgramCounter(at);
}

uint64_t cycles(EmulationController& ctrl)
{
    auto st = ctrl.lockState();
    return st.memory().getCycleCounter();
}

void testDeadWorkerIsRestartedAndStopReturns()
{
    static const uint8_t kLoop[] = { 0xAD, 0xE0, 0xC0, 0x4C, 0x00, 0x08 };   // LDA $C0E0 / JMP
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        st.memory().slotBus().plug(6, std::make_unique<ThrowOnceCard>());
    }
    poke(ctrl, kLoop, sizeof kLoop);
    ctrl.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the barrier fires
    assert(ctrl.getMode() == EmulationController::Mode::Stopped);
    const uint64_t c0 = cycles(ctrl);
    ctrl.start();                                                  // the user presses Play
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(cycles(ctrl) - c0 > 1000 && "Play after a dead worker did not restart the machine");
    std::atomic<bool> done{false};
    std::thread t([&] { ctrl.stop(); done.store(true); });
    for (int i = 0; i < 30 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!done.load()) {
        std::printf("FAIL: stop() after a dead worker is still blocked after 3 s\n");
        t.detach();
        std::_Exit(1);                                            // fail, do not hang the runner
    }
    t.join();
    std::puts("  ok: a dead worker is reaped, Play restarts it, stop() returns");
}

void testStepRetirementDoesNotClobberRun()
{
    static const uint8_t kLoop[] = { 0xEA, 0xEA, 0xEA, 0x4C, 0x00, 0x08 };
    EmulationController ctrl;
    poke(ctrl, kLoop, sizeof kLoop);
    ctrl.setMode(EmulationController::Mode::Stopped);
    ctrl.start();
    ctrl.setMode(EmulationController::Mode::Stopped);
    while (!ctrl.rewindIsParked()) std::this_thread::yield();
    int lost = 0;
    const int kTrials = 3000;
    for (int t = 0; t < kTrials; ++t) {
        ctrl.setMode(EmulationController::Mode::Stopped);
        while (!ctrl.rewindIsParked()) std::this_thread::yield();
        ctrl.requestStep(1);
        for (volatile int s = 0; s < (t % 600); ++s) {}
        ctrl.setMode(EmulationController::Mode::Running);
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        if (ctrl.getMode() != EmulationController::Mode::Running) ++lost;
    }
    ctrl.stop();
    if (lost) std::printf("FAIL: %d of %d Run clicks were clobbered by the Step retirement\n", lost, kTrials);
    assert(lost == 0);
    std::puts("  ok: Run right after a Step is never clobbered");
}

void testStopCancelsQueuedSteps()
{
    static const uint8_t kNops[] = { 0xEA,0xEA,0xEA,0xEA,0xEA,0xEA,0xEA,0xEA, 0x4C,0x00,0x08 };
    EmulationController ctrl;
    poke(ctrl, kNops, sizeof kNops);
    ctrl.setMode(EmulationController::Mode::Stopped);
    ctrl.start();
    ctrl.stop();
    ctrl.requestStep(20000);
    ctrl.stop();
    ctrl.coldBoot();
    poke(ctrl, kNops, sizeof kNops);
    ctrl.stop();
    const uint64_t before = cycles(ctrl);
    ctrl.requestStep(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ctrl.stop();
    const uint64_t burned = cycles(ctrl) - before;
    if (burned > 8) std::printf("FAIL: one Step press burned %llu cycles\n", (unsigned long long)burned);
    assert(burned <= 8 && "a Stop must cancel the queued steps");
    // An uninterrupted burst still runs whole.
    const uint64_t b0 = cycles(ctrl);
    ctrl.requestStep(4);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    ctrl.stop();
    assert(cycles(ctrl) - b0 == 8 && "four NOP steps must burn eight cycles");
    std::puts("  ok: Stop cancels queued steps, a burst still runs whole");
}

void testBootFromSlotUnsuspendsAudio()
{
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        st.memory().slotBus().plug(6, std::make_unique<BootableCard>());
    }
    ctrl.start();
    assert(!ctrl.audio().isSuspended());
    ctrl.setMode(EmulationController::Mode::Stopped);
    ctrl.stop();
    assert(ctrl.audio().isSuspended() && "a paused machine silences the bus");
    const bool booted = ctrl.bootFromSlot(6);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(booted && ctrl.getMode() == EmulationController::Mode::Running);
    assert(!ctrl.audio().isSuspended() && "Boot from a paused machine ran it mute");
    ctrl.stop();
    std::puts("  ok: bootFromSlot from a paused machine un-suspends the bus");
}

}  // namespace

int main(int argc, char** argv)
{
    // One case by name, for the negative check against the unfixed tree.
    const std::string only = argc > 1 ? argv[1] : "";
    if (only.empty() || only == "audio") testBootFromSlotUnsuspendsAudio();
    if (only.empty() || only == "steps") testStopCancelsQueuedSteps();
    if (only.empty() || only == "race")  testStepRetirementDoesNotClobberRun();
    if (only.empty() || only == "dead")  testDeadWorkerIsRestartedAndStopReturns();
    std::puts("controller_mode_races OK");
    return 0;
}
