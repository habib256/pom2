// The clock the emuCycles consumers hold is the clock the CHUNK ran at.
//
// Since 2026-09-09 the frame is budgeted in BASE cycles and the accelerator
// multiplier is re-read every 4096-cycle chunk (`transwarp_chunk_sampling`),
// but `refreshAcceleratorClock()` — the fan-out that hands SpeakerDevice,
// CassetteDevice, the floppy sound banks and every slot card the Hz they use
// to turn a cycle stamp into a second — was still sampled ONCE, before the
// frame's first chunk. The two then disagree for the whole of any frame in
// which a TransWarp slow window opens or closes:
//
//   window opening 1/3 into the frame: 22 901 CPU cycles burned, devices
//     told 3 579 544 Hz — those cycles are worth 1 374 096 Hz. 2.61x out.
//   the frame in which it closes:      49 417 CPU cycles burned, devices
//     told 1 022 727 Hz — worth 2 965 098 Hz. 2.9x the other way.
//
// That is the //c+ defect of bug hunt #7 and the TransWarp defect of #10,
// re-entering through the frame edge: a replay cursor fed a clock 2.6x wrong
// for a frame either starves or runs up onto the producer, trips its
// re-anchor and drops the backlog — a click at every boundary of every DOS
// disk read (slot 6 ships at stock speed on the shipped DIP block, so RWTS
// polling $C0EC holds a window open for the whole read).
//
// The invariant this pins is the simple one: after a frame, the clock the
// devices are holding must be the clock the machine is running at.

#include "CpuClock.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Mockingboard.h"
#include "Memory.h"
#include "SlotBus.h"
#include "TranswarpCard.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>

namespace {

constexpr int    kBase   = 17045;        // NTSC frame, base cycles
constexpr double kNominal = 1022727.0;

// $0800: `nops` NOPs, then a loop reading slot 6's device select ($C0E0 —
// stock speed on the shipped DIP block) forever: the slow window is open
// from that instruction on.
void loadHammer(EmulationController& ctrl, int nops)
{
    auto st = ctrl.lockState();
    uint16_t a = 0x0800;
    for (int i = 0; i < nops; ++i) st.memory().memWrite(a++, 0xEA);
    const uint16_t loop = a;
    st.memory().memWrite(a++, 0xAD); st.memory().memWrite(a++, 0xE0); st.memory().memWrite(a++, 0xC0);
    st.memory().memWrite(a++, 0x4C);
    st.memory().memWrite(a++, static_cast<uint8_t>(loop));
    st.memory().memWrite(a++, static_cast<uint8_t>(loop >> 8));
    st.cpu().setProgramCounter(0x0800);
}

// A pure NOP loop: never touches a slot, so no window ever opens.
void loadNops(EmulationController& ctrl)
{
    auto st = ctrl.lockState();
    for (uint16_t a = 0x0800; a < 0x0900; ++a) st.memory().memWrite(a, 0xEA);
    st.memory().memWrite(0x0900, 0x4C);
    st.memory().memWrite(0x0901, 0x00);
    st.memory().memWrite(0x0902, 0x08);
    st.cpu().setProgramCounter(0x0800);
}

std::unique_ptr<EmulationController> makeWithTranswarp()
{
    auto c = std::make_unique<EmulationController>();
    c->setCyclesPerFrame(kBase);
    c->setVideoStandard(VideoStandard::NTSC);
    {
        auto st = c->lockState();
        auto card = std::make_unique<pom2::TranswarpCard>(4);
        card->setMemory(&st.memory());
        st.memory().slotBus().plug(4, std::move(card));
    }
    c->setMode(EmulationController::Mode::Running);
    return c;
}

// The whole contract in one line: the devices' clock == the machine's speed.
void checkAgrees(EmulationController& c, const char* what)
{
    double mul;
    long   cycles;
    {
        auto st = c.lockState();
        mul    = st.memory().slotBus().cpuSpeedMultiplier();
        cycles = static_cast<long>(st.memory().getCycleCounter());
    }
    (void)cycles;
    const double want = kNominal * mul;
    const double got  = c.emulatedCpuClockHz();
    std::printf("  %-26s multiplier %.2fx -> devices should hold %9.0f Hz, "
                "hold %9.0f Hz\n", what, mul, want, got);
    assert(std::fabs(got - want) < 1.0 &&
           "the accelerator clock fan-out is coarser than the speed the "
           "chunks actually ran at");
}

// The other half of the contract: what an accelerator must NOT re-clock.
// The Mockingboard's 6522s tick on the slot bus's phase 0 (MAME: the a2bus
// clock), which a TransWarp leaves alone. Before 2026-09-29 the fan-out
// handed the card 3.5x and its VIAs ticked once per CPU cycle, so T1-paced
// music ran 3.5x fast. Arm a one-shot T1 at $FFFF and let one frame of NOPs
// run at 3.5x: the timer must have moved one frame of BUS ticks (17 045),
// not one frame of CPU cycles (~59 657).
void checkMockingboardStaysOnPhase0()
{
    auto c = makeWithTranswarp();
    MockingboardCard* mb = nullptr;
    {
        auto st = c->lockState();
        auto card = std::make_unique<MockingboardCard>(5);
        card->setCpu(&st.cpu());
        mb = card.get();
        st.memory().slotBus().plug(5, std::move(card));
    }
    // What Slot Config Apply does after a replug: both clocks, to every card.
    c->setVideoStandard(VideoStandard::NTSC);
    loadNops(*c);
    c->tickFrame();                          // settle the 3.5x fan-out
    uint16_t before;
    {
        auto st = c->lockState();
        mb->slotRomWrite(0x0B, 0x00);        // ACR: T1 one-shot
        mb->slotRomWrite(0x06, 0xFF);        // T1LL
        mb->slotRomWrite(0x05, 0xFF);        // T1CH: load $FFFF + arm
        before = static_cast<uint16_t>(mb->peekViaRegister(0, 0x04) |
                                       (mb->peekViaRegister(0, 0x05) << 8));
    }
    c->tickFrame();
    uint16_t after;
    long cpuCycles;
    {
        auto st = c->lockState();
        after = static_cast<uint16_t>(mb->peekViaRegister(0, 0x04) |
                                      (mb->peekViaRegister(0, 0x05) << 8));
        cpuCycles = static_cast<long>(st.memory().getCycleCounter());
    }
    (void)cpuCycles;
    const int ticks = static_cast<uint16_t>(before - after);
    std::printf("  Mockingboard T1 over one 3.5x frame: %d ticks "
                "(phase 0 wants ~%d)\n", ticks, kBase);
    assert(c->emulatedCpuClockHz() > kNominal * 3.0 &&
           "the fixture is supposed to run accelerated");
    assert(std::abs(ticks - kBase) < 400 &&
           "an accelerator re-clocked the Mockingboard's 6522");
}

}  // namespace

int main()
{
    // 1. Steady 3.5x: nothing to get wrong, and it must stay right.
    {
        auto c = makeWithTranswarp();
        loadNops(*c);
        for (int i = 0; i < 3; ++i) c->tickFrame();
        checkAgrees(*c, "no window");
    }
    // 2. Steady 1x (window held open from the first cycle).
    {
        auto c = makeWithTranswarp();
        loadHammer(*c, 0);
        for (int i = 0; i < 3; ++i) c->tickFrame();
        checkAgrees(*c, "window held open");
    }
    // 3. The window OPENS a third of the way into the frame. The frame ends
    //    inside it (1x), so the devices must be holding 1 022 727 Hz — a
    //    frame-granular fan-out leaves them on 3 579 544.
    {
        auto c = makeWithTranswarp();
        loadHammer(*c, 3500);
        c->tickFrame();
        checkAgrees(*c, "window opens mid-frame");
    }
    // 4. ...and the mirror: a frame that begins inside a window and leaves
    //    it. The frame ends at 3.5x, so the devices must be holding
    //    3 579 544 Hz — a frame-granular fan-out leaves them on 1 022 727.
    {
        auto c = makeWithTranswarp();
        loadHammer(*c, 0);
        c->tickFrame();                 // ends with the window wide open
        loadNops(*c);
        c->tickFrame();                 // ...and this one never re-opens it
        checkAgrees(*c, "window closes mid-frame");
    }
    // 5. The accelerator must leave phase-0 devices alone.
    checkMockingboardStaysOnPhase0();
    std::puts("accelerator_clock_chunk OK");
    return 0;
}
