// A zero-persistence phosphor has no history to preserve — bug hunt #10.
//
// `effectivePhosphorDecay` answered "no decay" whenever the emulated frame
// index stood still, for EVERY phosphor, MonoWhite (decay 0.00) included.
// With merged = max(target, prev x decay) that made the mono painters
// "pixels can only ever light up" for two permanent callers: the paint
// editor's never-clocked canvas Memory and a paused / breakpointed machine
// whose RAM the debugger or the AI server pokes. An erased MonoWhite dot
// stayed lit forever. Green and amber keep their afterglow: that is the
// documented intent while paused.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

bool anyLit(const Apple2Display& d, int y)
{
    const int w = d.width();
    for (int x = 0; x < w; ++x)
        if ((d.pixels()[y * w + x] & 0xFFFFFF) != 0) return true;
    return false;
}

bool eraseSticks(Apple2Display::HiResMode m)
{
    Memory mem; mem.setIIEMode(true);
    Apple2Display d; d.setAuxMemory(mem.auxData()); d.setHiResMode(m);
    mem.memRead(0xC050); mem.memRead(0xC052); mem.memRead(0xC057); mem.memRead(0xC054);
    for (int c = 0; c < 40; ++c) mem.writeRamUnchecked(0x2000 + c, 0x7F);   // row 0 lit
    d.render(mem);                       // never-clocked: frame delta stays 0
    assert(anyLit(d, 0));
    for (int c = 0; c < 40; ++c) mem.writeRamUnchecked(0x2000 + c, 0x00);   // erased
    d.render(mem);
    return anyLit(d, 0);
}

}  // namespace

int main()
{
    assert(!eraseSticks(Apple2Display::HiResMode::MonoWhite) &&
           "an erased MonoWhite dot stayed lit while the frame index stood still");
    assert(eraseSticks(Apple2Display::HiResMode::MonoGreen) &&
           "green keeps its afterglow while paused — deliberate");
    std::printf("phosphor_decay_zero OK\n");
    return 0;
}
