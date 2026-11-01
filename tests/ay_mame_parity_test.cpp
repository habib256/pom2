// Differential checks against MAME a2b6ba2d4be70dabf7ff7a642749dda0c6e70498.
// sound/ay8910.cpp:1073-1084 (tone), :1113-1147 (envelope),
// :1305-1325 (reset), :1356-1393 (selection).
#include "AyPsgSynth.h"
#include "Via6522.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void toneParity()
{
    // Independent transcription of MAME's count/subtract loop. Exercise
    // every 12-bit old period, dropping to periods with odd/even quotients,
    // zero-period aliasing, and all three channels at different phases.
    for (int old = 2; old <= 4095; ++old) {
        for (int next : {0, 1, 2, 3, 7, 31, 255, 4095}) {
            pom2::ay::ChipSynthState cs;
            uint8_t r[16] = {};
            int count[3], duty[3] = {};
            for (int ch = 0; ch < 3; ++ch) {
                cs.toneCounter[ch] = count[ch] = old - 1 - std::min(ch, old - 1);
                r[ch*2] = next & 255;
                r[ch*2+1] = (next >> 8) | 0xf0; // inaccessible bits ignored
            }
            for (int tick = 0; tick < 8; ++tick) {
                pom2::ay::stepTick(cs, r);
                for (int ch = 0; ch < 3; ++ch) {
                    ++count[ch];
                    while (count[ch] >= std::max(1, next)) {
                        duty[ch] = (duty[ch] - 1) & 31;
                        count[ch] -= std::max(1, next);
                    }
                    if (cs.toneCounter[ch] != count[ch] || cs.toneOut[ch] != (duty[ch] & 1)) {
                        std::fprintf(stderr, "tone mismatch old=%d new=%d tick=%d ch=%d\n", old, next, tick, ch);
                        std::abort();
                    }
                }
            }
        }
    }
}

void envelopeParity()
{
    for (int shape = 0; shape < 16; ++shape) {
        for (int ep : {0, 1, 2, 17, 65535}) {
            pom2::ay::ChipSynthState cs;
            uint8_t r[16] = {};
            r[11] = ep & 255; r[12] = ep >> 8; r[13] = shape;
            pom2::ay::applyEnvShape(cs, r);
            int step = 15, attack = (shape & 4) ? 15 : 0;
            int hold = (shape & 8) ? (shape & 1) : 1;
            int alternate = (shape & 8) ? (shape & 2) : attack;
            unsigned count = 0;
            bool holding = false;
            const int ticks = std::max(1, ep * 2) * 40;
            for (int t = 0; t < ticks; ++t) {
                if (!holding && ++count >= unsigned(ep * 2)) {
                    count = 0;
                    if (--step < 0) {
                        if (hold) {
                            if (alternate) attack ^= 15;
                            holding = true; step = 0;
                        } else {
                            if (alternate && (step & 16)) attack ^= 15;
                            step &= 15;
                        }
                    }
                }
                pom2::ay::stepTick(cs, r);
                assert(cs.envCounter == count && cs.envStep == step);
                assert(cs.envAttack == attack && bool(cs.envHolding) == holding);
            }
            // Same-value R13 restarts the ramp without resetting its counter.
            cs.envCounter = 1;
            cs.envRetrigger = true;
            pom2::ay::applyEnvShape(cs, r);
            assert(cs.envStep == 15 && !cs.envHolding && cs.envCounter == 1);
        }
    }
}

void resetSelection()
{
    pom2::Ay3_8910 ay;
    for (int pass = 0; pass < 2; ++pass) {
        // Reset clears m_active: data writes ignored, reads high impedance.
        assert(ay.applyControl(0x55, 6) == pom2::Ay3_8910::NoChange);
        assert(ay.regs[0] == 0);
        assert(ay.applyControl(0, 5) == pom2::Ay3_8910::Read && ay.busOut == 0xff);
        ay.applyControl(0, 7);
        assert(ay.applyControl(0x55, 6) == pom2::Ay3_8910::Wrote);
        assert(ay.regs[0] == 0x55);
        ay.applyControl(0, 0); // assert /RESET
    }
}

// [A] mb-audit issue #22 (Tom Charlesworth), measured on real chips with the
// sequence mb-audit uses to identify the part — R7=$FF, R14=$55, R15=$69, read
// back: AY-3-8913 → 3F 0E 0F (ReactiveMicro Mockingboard, both Phasor AYs);
// AY-3-8910 (MB-K) and AY38912A/P (from a Mockingboard 4c) → FF 55 69, and
// FF FF for both ports once set as INPUTS ([B] GI datasheet p. 5-19, "internal
// pull-ups"). AUDIO-010 pinned the opposite here on 2026-10-24 — "the stored
// byte, input or output, matching MAME" — a grade-D alignment against a
// grade-A measurement; mb-audit then flagged every POM2 AY as an 8910.
// `docs/audits/parite-ay8910-2026-11-01.md`.
void portReadBack()
{
    using Chip = pom2::Ay3_8910::Chip;
    auto store = [](pom2::Ay3_8910& ay, uint8_t reg, uint8_t v) {
        ay.applyControl(reg, 7);       // latch (BDIR|BC1, /RESET high)
        ay.applyControl(v, 6);         // write (BDIR)
    };
    auto load = [](pom2::Ay3_8910& ay, uint8_t reg) {
        ay.applyControl(reg, 7);
        assert(ay.applyControl(0, 5) == pom2::Ay3_8910::Read);   // read (BC1)
        return ay.busOut;
    };
    {   // The default is the 8913, which every POM2 slot card carries.
        pom2::Ay3_8910 ay;
        assert(ay.chip == Chip::Ay8913);
        store(ay, 7, 0xFF); store(ay, 14, 0x55); store(ay, 15, 0x69);
        assert(load(ay, 7)  == 0x3F && "8913: R7 has no port-direction bits");
        assert(load(ay, 14) == 0x0E && "8913: R14 reads $0E, there is no port");
        assert(load(ay, 15) == 0x0F && "8913: R15 reads $0F");
    }
    for (Chip c : { Chip::Ay8910, Chip::Ay8912 }) {
        pom2::Ay3_8910 ay; ay.chip = c;
        store(ay, 7, 0xFF); store(ay, 14, 0x55); store(ay, 15, 0x69);
        assert(load(ay, 7) == 0xFF);
        assert(load(ay, 14) == 0x55 && load(ay, 15) == 0x69 && "outputs read the latch");
        store(ay, 7, 0x3F);                 // both ports as inputs
        assert(load(ay, 14) == 0xFF && load(ay, 15) == 0xFF && "inputs read the pull-ups");
        store(ay, 7, 0x38);                 // a music driver's usual R7
        assert(load(ay, 14) == 0xFF && "still an input: $FF, not the stored $55");
        store(ay, 7, 0x40);                 // port A out, port B in
        assert(load(ay, 14) == 0x55 && load(ay, 15) == 0xFF);
    }
}

void repeatedPortB()
{
    pom2::Via6522 via;
    via.write(2, 7);
    via.write(0, 6);
    assert((via.write(0, 6) & 1) && "MAME dispatches identical ORB stores");
    via.write(2, 0);
    assert(!(via.write(0, 6) & 1)); // no outputs to dispatch
}
}
int main(int argc, char** argv)
{
    const char* which = argc > 1 ? argv[1] : "all";
    if (!std::strcmp(which, "all") || !std::strcmp(which, "tone")) toneParity();
    if (!std::strcmp(which, "all") || !std::strcmp(which, "envelope")) envelopeParity();
    if (!std::strcmp(which, "all") || !std::strcmp(which, "reset")) resetSelection();
    if (!std::strcmp(which, "all") || !std::strcmp(which, "bus")) repeatedPortB();
    if (!std::strcmp(which, "all") || !std::strcmp(which, "ports")) portReadBack();
    std::puts("AY MAME parity OK");
}
