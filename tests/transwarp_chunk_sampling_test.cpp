// The TransWarp's multiplier is sampled per chunk, not per frame — 2026-09-09.
//
// A slow window that opens mid-frame (a program that starts hammering a
// stock-speed slot) used to leave the whole frame at 3.5× because the
// frame's cycle budget had been fixed from the multiplier read at its
// start. The frame is budgeted in base cycles now and the multiplier is
// re-read per 4096-cycle chunk. Three frames through the single-threaded
// tickFrame() path: no window (full 3.5×), a window open from the first
// cycle (1× throughout), and a window opening a third of the way in — the
// case per-frame sampling got wrong by a factor of ~2.7.

#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "TranswarpCard.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int kBase = 17045;   // NTSC frame, base cycles

// $0800: a NOP run of `nops` bytes (2 cycles each), then a loop that reads
// slot 6's device select ($C0E0 — stock speed on the shipped DIP block) and
// jumps back to itself: window permanently open from there on.
void load(EmulationController& ctrl, int nops)
{
    auto st = ctrl.lockState();
    uint16_t a = 0x0800;
    for (int i = 0; i < nops; ++i) st.memory().memWrite(a++, 0xEA);
    const uint16_t loop = a;
    st.memory().memWrite(a++, 0xAD); st.memory().memWrite(a++, 0xE0); st.memory().memWrite(a++, 0xC0);  // LDA $C0E0
    st.memory().memWrite(a++, 0x4C); st.memory().memWrite(a++, static_cast<uint8_t>(loop)); st.memory().memWrite(a++, static_cast<uint8_t>(loop >> 8));
    st.cpu().setProgramCounter(0x0800);
}

// A pure NOP loop: never touches a slot, so the card never slows.
void loadPureNops(EmulationController& ctrl)
{
    auto st = ctrl.lockState();
    for (uint16_t a = 0x0800; a < 0x0900; ++a) st.memory().memWrite(a, 0xEA);
    st.memory().memWrite(0x0900, 0x4C); st.memory().memWrite(0x0901, 0x00); st.memory().memWrite(0x0902, 0x08);
    st.cpu().setProgramCounter(0x0800);
}

long frameCycles(EmulationController& ctrl)
{
    long before, after;
    { auto st = ctrl.lockState(); before = static_cast<long>(st.memory().getCycleCounter()); }
    ctrl.tickFrame();
    { auto st = ctrl.lockState(); after = static_cast<long>(st.memory().getCycleCounter()); }
    return after - before;
}

EmulationController* make(std::unique_ptr<EmulationController>& holder)
{
    holder = std::make_unique<EmulationController>();
    EmulationController& ctrl = *holder;
    ctrl.setCyclesPerFrame(kBase);
    {
        auto st = ctrl.lockState();
        auto card = std::make_unique<pom2::TranswarpCard>(4);
        card->setMemory(&st.memory());
        st.memory().slotBus().plug(4, std::move(card));
    }
    ctrl.setMode(EmulationController::Mode::Running);   // tickFrame runs a frame in Running
    return holder.get();
}

}  // namespace

int main()
{
    std::unique_ptr<EmulationController> h;
    // 1. No window: the frame is 3.5× the base budget.
    {
        EmulationController& c = *make(h);
        loadPureNops(c);
        const long n = frameCycles(c);
        std::printf("  no window:           %ld CPU cycles in a frame (expect ~%d)\n", n, static_cast<int>(kBase * 3.5));
        assert(n > kBase * 3.3 && n < kBase * 3.7 && "acceleration lost");
    }
    // 2. Window open from the first cycle: the frame is the base budget.
    {
        EmulationController& c = *make(h);
        load(c, 0);
        const long n = frameCycles(c);
        // The first chunk runs at 3.5× (the multiplier is read before the
        // loop's first access), so up to one chunk of slack sits on top.
        std::printf("  window from cycle 0: %ld CPU cycles (expect %d..%d)\n", n, kBase, kBase + 4096);
        assert(n >= kBase * 0.98 && n <= kBase + 4096 + 256);
    }
    // 3. Window opening a third of the way in: 7000 CPU cycles of NOPs at
    //    3.5× (2000 base), then 1× for the remaining ~15045 base cycles.
    //    Per-frame sampling answered ~59 657; per-chunk lands near 22 000
    //    (the chunk in which the window opens finishes at 3.5×, up to ~4096
    //    CPU cycles of slack).
    {
        EmulationController& c = *make(h);
        load(c, 3500);
        const long n = frameCycles(c);
        std::printf("  window at 1/3 frame: %ld CPU cycles (expect ~22 000, per-frame gave ~59 657)\n", n);
        assert(n > 18000 && n < 30000 && "the multiplier is still sampled once per frame");
    }
    std::puts("transwarp_chunk_sampling OK");
    return 0;
}
