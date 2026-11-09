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
#include "PhasorCard.h"
#include "MouseCard.h"
#include "ResourcePaths.h"
#include "EzColorCard.h"
#include "PremiumSoftCard.h"
#include "Memory.h"
#include "SlotBus.h"
#include "TranswarpCard.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>

namespace {

constexpr int    kBase   = POM2_TIMING_NTSC.cyclesPerFrame;   // one NTSC video frame, base cycles
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

// Menu pacing fast-forwards the whole machine. Compare all four counters
// against elapsed machine time, including speed changes and a real TransWarp:
// the menu multiplier must disappear from the divider, the accelerator must
// remain. Exercise the controller fan-out and per-instruction lazy catch-up.
template<class Card>
void checkSoundCardFollowsSpeedMenu()
{
    for (auto standard : {VideoStandard::NTSC, VideoStandard::PAL}) {
        const auto& vt = pom2VideoTiming(standard);
        for (bool accelerated : {false, true}) {
            EmulationController c(AudioDevice::OutputMode::Offline);
            Card* mb;
            {
                auto st = c.lockState();
                if (accelerated) {
                    auto tw = std::make_unique<pom2::TranswarpCard>(4);
                    tw->setMemory(&st.memory());
                    st.memory().slotBus().plug(4, std::move(tw));
                }
                auto card = std::make_unique<Card>(5);
                card->setCpu(&st.cpu());
                mb = card.get();
                st.memory().slotBus().plug(5, std::move(card));
            }
            loadNops(c);
            c.setMode(EmulationController::Mode::Running);
            // No setVideoStandard between transitions: the chunk fan-out
            // must update the card when the menu alone changes its budget.
            c.setCyclesPerFrame(vt.cyclesPerFrame);
            c.setVideoStandard(standard);
            for (int budget : {vt.cyclesPerFrame, 2 * vt.cyclesPerFrame,
                               4 * vt.cyclesPerFrame, vt.cyclesPerFrame,
                               vt.cyclesPerFrame / 2, 1'000'000}) {
                c.setCyclesPerFrame(budget);
                c.tickFrame(); // settle clock fan-out and instruction residue
                uint16_t before[2][2];
                uint64_t start;
                auto counter = [&](int chip, int timer) {
                    const int reg = timer == 0 ? 4 : 8;
                    return static_cast<uint16_t>(mb->peekViaRegister(chip, reg) |
                        (mb->peekViaRegister(chip, reg + 1) << 8));
                };
                {
                    auto st = c.lockState();
                    for (int chip = 0; chip < 2; ++chip) {
                        const int base = chip * 0x80;
                        mb->slotRomWrite(base + 0x0B, 0); // one-shot T1, timed T2
                        mb->slotRomWrite(base + 0x04, 0xFF);
                        mb->slotRomWrite(base + 0x05, 0xFF);
                        mb->slotRomWrite(base + 0x08, 0xFF);
                        mb->slotRomWrite(base + 0x09, 0xFF);
                        for (int t = 0; t < 2; ++t) before[chip][t] = counter(chip, t);
                    }
                    start = st.memory().getCycleCounter();
                }
                c.tickFrame();
                auto st = c.lockState();
                const auto elapsed = st.memory().getCycleCounter() - start;
                const double multiplier = accelerated ? 3.5 : 1.0;
                const auto ticks = static_cast<uint64_t>(elapsed / multiplier);
                for (int chip = 0; chip < 2; ++chip) {
                    for (int t = 0; t < 2; ++t) {
                        // T1 reloads its $FFFF latch every N+2 ticks; T2
                        // wraps at 65536. MAX crosses several such periods.
                        const auto expected = static_cast<uint16_t>(t == 0 ? ticks % 65537 : ticks);
                        const auto got = static_cast<uint16_t>(before[chip][t] - counter(chip, t));
                        assert(std::abs(static_cast<int16_t>(got - expected)) <= 3 &&
                               "the sound-card timer must follow menu pacing, not CPU acceleration");
                    }
                }
                std::printf("  %.*s %s budget %d, accelerator %.1fx: timers follow %llu CPU cycles\n",
                    int(mb->name().size()), mb->name().data(), standard == VideoStandard::PAL ? "PAL" : "NTSC", budget, multiplier,
                    static_cast<unsigned long long>(elapsed));
            }
        }
    }
}

// Equal CPU work at equal hardware clocks must produce equal peripheral
// work regardless of the menu's pacing. Snapshot comparison includes the
// E-Z Color beam and framebuffer; the mouse reports retired MCU cycles.
void checkOtherBusDevicesFollowSpeedMenu()
{
    for (double hz : {1022727.0, 1015625.0})
        for (int accelerator : {1, 4})
            for (int pacing : {1, 2, 4}) {
                MouseCard mouse(4), referenceMouse(4);
                const auto eprom = pom2::findResource("roms/mouse_341-0270-c.bin");
                const auto mcu = pom2::findResource("roms/mouse_341-0269.bin");
                assert(mouse.loadRoms(eprom, mcu));
                assert(referenceMouse.loadRoms(eprom, mcu));
                EzColorCard video, referenceVideo;
                for (auto* card : {static_cast<SlotPeripheral*>(&mouse),
                                   static_cast<SlotPeripheral*>(&video)}) {
                    card->setStandardClock(hz);
                    card->setCpuClock(hz * accelerator * pacing);
                    card->setMachineCpuClock(hz * accelerator);
                }
                for (auto* card : {static_cast<SlotPeripheral*>(&referenceMouse),
                                   static_cast<SlotPeripheral*>(&referenceVideo)}) {
                    card->setStandardClock(hz);
                    card->setCpuClock(hz * accelerator);
                }
                for (int i = 0; i < 300; ++i) {
                    mouse.advanceCycles(251);
                    referenceMouse.advanceCycles(251);
                    video.advanceCycles(251);
                    referenceVideo.advanceCycles(251);
                }
                assert(mouse.mcuCyclesRun() > 0);
                assert(mouse.mcuCyclesRun() == referenceMouse.mcuCyclesRun());
                std::vector<uint8_t> actual, expected;
                video.appendSnapshotState(actual);
                referenceVideo.appendSnapshotState(expected);
                assert(actual == expected);
            }
}

// The aux-slot Z80 must retire the same work per emulated machine time
// when the menu changes. Exercise the real chunk fan-out, not just setters.
void checkAuxSlotFollowsSpeedMenu()
{
    for (auto standard : {VideoStandard::NTSC, VideoStandard::PAL})
        for (bool accelerated : {false, true}) {
            EmulationController c(AudioDevice::OutputMode::Offline);
            PremiumSoftCard* card;
            {
                auto st = c.lockState();
                st.memory().setIIEMode(true);
                if (accelerated) {
                    auto tw = std::make_unique<pom2::TranswarpCard>(4);
                    tw->setMemory(&st.memory());
                    st.memory().slotBus().plug(4, std::move(tw));
                }
                auto owned = std::make_unique<PremiumSoftCard>();
                owned->setMemory(&st.memory());
                card = owned.get();
                st.memory().slotBus().plug(PremiumSoftCard::kAuxSlot, std::move(owned));
                // An all-NOP Z80 address space, including the card's own DRAM.
                for (unsigned a = 0; a < 65536; ++a)
                    st.memory().auxPhysical(static_cast<uint16_t>(a)) = 0;
            }
            loadNops(c);
            const auto& vt = pom2VideoTiming(standard);
            c.setCyclesPerFrame(vt.cyclesPerFrame);
            c.setVideoStandard(standard);
            c.setMode(EmulationController::Mode::Running);
            c.tickFrame(); // establish the hardware accelerator's ratio
            {
                auto st = c.lockState();
                card->c07eWrite(PremiumSoftCard::kRunOutOfReset | PremiumSoftCard::kRun);
            }
            for (int budget : {vt.cyclesPerFrame, 2 * vt.cyclesPerFrame,
                               4 * vt.cyclesPerFrame, vt.cyclesPerFrame,
                               vt.cyclesPerFrame / 2, 1'000'000}) {
                c.setCyclesPerFrame(budget);
                uint64_t start;
                uint16_t before;
                {
                    auto st = c.lockState();
                    start = st.memory().getCycleCounter();
                    before = card->z80().getState().pc;
                }
                c.tickFrame();
                auto st = c.lockState();
                const auto elapsed = st.memory().getCycleCounter() - start;
                const double machineHz = double(vt.cpuClockHz) * (accelerated ? 3.5 : 1.0);
                // Premium SoftCard's independent 6 MHz oscillator; NOP = 4 T.
                const auto expected = static_cast<uint16_t>(std::llround(elapsed * PremiumSoftCard::kZ80Hz / machineHz / 4));
                const auto retired = static_cast<uint16_t>(card->z80().getState().pc - before);
                assert(std::abs(static_cast<int16_t>(retired - expected)) <= 2 &&
                       "aux-slot Z80 work must follow menu pacing through the controller");
            }
        }
}

// The //e aux slot is SlotBus slot 0 (PremiumSoftCard::kAuxSlot), and every
// clock fan-out walked slots 1-7: the Premium SoftCard's Z80 divided by the
// stock NTSC 1 022 727 Hz for ever — 0.7 % fast on PAL, ~3.5x under a
// TransWarp, 4x under a FASTChip.
void checkAuxSlotGetsTheClock()
{
    auto c = makeWithTranswarp();
    PremiumSoftCard* psc = nullptr;
    {
        auto st = c->lockState();
        auto card = std::make_unique<PremiumSoftCard>();
        card->setMemory(&st.memory());
        psc = card.get();
        st.memory().slotBus().plug(PremiumSoftCard::kAuxSlot, std::move(card));
    }
    loadNops(*c);
    for (int i = 0; i < 3; ++i) c->tickFrame();
    std::printf("  aux-slot card at 3.5x: holds %9.0f Hz, machine %9.0f Hz\n",
                psc->cpuClockHz(), c->machineCpuClockHz());
    assert(std::fabs(psc->cpuClockHz() - c->machineCpuClockHz()) < 1.0 &&
           "the accelerator fan-out skipped the //e aux slot");
    c->setVideoStandard(VideoStandard::PAL);
    std::printf("  aux-slot card on PAL:  holds %9.0f Hz, machine %9.0f Hz\n",
                psc->cpuClockHz(), c->machineCpuClockHz());
    assert(std::fabs(psc->cpuClockHz() - c->machineCpuClockHz()) < 1.0 &&
           "the video-standard fan-out skipped the //e aux slot");
}

}  // namespace

// An accelerator that goes away (a profile switch, a replug) must take the
// beam's ratio with it. The clock-only fast path kept the accelerated ratio:
// the beam ran ~1/3.5 speed on the stock machine (bug hunt 2026-11-07).
void testRatioFollowsTheAcceleratorAway()
{
    auto c = makeWithTranswarp();
    loadNops(*c);
    for (int i = 0; i < 3; ++i) c->tickFrame();
    {
        auto st = c->lockState();
        assert(st.memory().videoClockRatio() < 0.5 && "the TransWarp runs fast");
        st.memory().slotBus().plug(4, nullptr);
    }
    c->setVideoStandard(VideoStandard::NTSC);
    c->tickFrame();
    uint64_t v0, c0, v1, c1;
    { auto st = c->lockState(); v0 = st.memory().videoNow(); c0 = st.memory().getCycleCounter(); }
    c->tickFrame();
    { auto st = c->lockState(); v1 = st.memory().videoNow(); c1 = st.memory().getCycleCounter(); }
    assert(v1 - v0 == c1 - c0 && "the beam kept the accelerator's rate");
    std::printf("  ok: the beam's ratio follows the accelerator away\n");
}

int main()
{
    testRatioFollowsTheAcceleratorAway();
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
    checkSoundCardFollowsSpeedMenu<MockingboardCard>();
    checkSoundCardFollowsSpeedMenu<PhasorCard>();
    checkOtherBusDevicesFollowSpeedMenu();
    // 6. The //e aux slot (slot 0) is a card too.
    checkAuxSlotGetsTheClock();
    checkAuxSlotFollowsSpeedMenu();
    std::puts("accelerator_clock_chunk OK");
    return 0;
}
