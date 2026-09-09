// The Phasor's AY clock SCALE is a timeline quantity, like every other AY
// event on the card.
//
// `PhasorCard::AudioSrc::render` used to snapshot `parent->clockScale()`
// once per fill and apply it to the whole buffer. The render cursor
// deliberately trails the producer by two 20 ms bursts (the jitter buffer
// the Mockingboard's design brought over on 2026-09-09), so the ~40 ms of
// register writes still queued when the guest hits $C0(8+s)D got rendered
// with the NEW chip clock: a 1997 Hz tone written in Mockingboard-compat
// mode came out at 3996 Hz for 27.8 ms BEFORE the switch's own cycle was
// reached, and the mirror an octave down on the way back.
//
// The mode switch is stamped like any other event now (kRegClockScale), so
// the octave lands where the guest put it.

#include "M6502.h"
#include "Memory.h"
#include "PhasorCard.h"
#include "SlotBus.h"
#include "Via6522.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr uint32_t kSr = 44100;

uint8_t makePb()
{
    uint8_t pb = 0xFF;
    pb &= ~0x03;      // BC1 / BDIR low (inactive)
    pb |=  0x04;      // /RESET released
    pb &= ~0x08;      // primary AY selected (active low)
    pb |=  0x10;      // secondary deselected
    return pb;
}

void ayWrite(PhasorCard& card, uint8_t regAddr, uint8_t data)
{
    const uint8_t base =
        (card.mode() == PhasorCard::PH_Mockingboard) ? 0x00 : 0x10;
    const uint8_t pb = makePb();
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRA, 0xFF);
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRB, 0xFF);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  regAddr);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB, static_cast<uint8_t>(pb | 0x03));
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB, pb);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  data);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB, static_cast<uint8_t>(pb | 0x02));
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB, pb);
}

double freqOf(const std::vector<float>& v)
{
    if (v.size() < 8) return 0.0;
    double mean = 0.0;
    for (float s : v) mean += s;
    mean /= static_cast<double>(v.size());
    int crossings = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= mean && v[i] > mean) ++crossings;
    return crossings * static_cast<double>(kSr) / static_cast<double>(v.size());
}

}  // namespace

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    // PLUGGED, not free-standing: `lastSyncCycle_` is half of the render
    // cursor's `producerNow`, and only the card's own MMIO / advanceCycles
    // move it. A free-standing card leaves the producer frozen between
    // writes and the cursor's re-anchor then parks two bursts behind it
    // forever — the harness, not the card, would be the thing under test.
    auto owned = std::make_unique<PhasorCard>(4);
    PhasorCard& card = *owned;
    mem.slotBus().plug(4, std::move(owned));
    card.setCpu(&cpu);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);

    // AY0 channel A, TP = 32: 1 022 727 / (16 * 32) = 1997 Hz in
    // Mockingboard-compat mode, 3994 Hz once the Phasor's doubled chip
    // clock is in force.
    ayWrite(card, 0, 32);        // R0  tone A fine
    ayWrite(card, 1, 0);         // R1  tone A coarse
    ayWrite(card, 7, 0x3E);      // R7  mixer: tone A only
    ayWrite(card, 8, 0x0F);      // R8  channel A volume 15

    AudioSource* src = card.audioSource();
    assert(src);
    constexpr int kChunk       = 245;      // ~5.6 ms
    constexpr int kFrameCycles = 17045;
    constexpr int kBufPerFrame = 3;

    std::vector<float> buf(kChunk);
    auto pump = [&](int frames, std::vector<float>* sink) {
        for (int f = 0; f < frames; ++f) {
            mem.advanceCycles(kFrameCycles);
            for (int b = 0; b < kBufPerFrame; ++b) {
                src->fillAudioBuffer(buf.data(), kChunk);
                if (sink) sink->insert(sink->end(), buf.begin(), buf.end());
            }
        }
    };

    pump(30, nullptr);                     // let the cursor settle at its lag
    std::vector<float> before;
    pump(10, &before);
    const double fBefore = freqOf(before);
    std::printf("  MB-compat, steady:            %.0f Hz (expect ~1997)\n", fBefore);
    assert(fBefore > 1800.0 && fBefore < 2200.0);

    // The guest switches to Phasor-native. Nothing else moves: same
    // registers, same chip select, same ~40 ms of backlog in the cursor.
    card.deviceSelectWrite(0x08, 0);       // clear the mode bits
    card.deviceSelectWrite(0x05, 0);       // ...and set them to 5 = Phasor
    assert(card.mode() == PhasorCard::PH_Phasor);
    assert(card.clockScale() == 2);

    // The next 5 buffers = 27.8 ms, well inside the two-burst (40.9 ms)
    // replay lag, so they render CPU cycles from BEFORE the switch. They
    // must still be the Mockingboard's octave.
    std::vector<float> justAfter;
    for (int b = 0; b < 5; ++b) {
        src->fillAudioBuffer(buf.data(), kChunk);
        justAfter.insert(justAfter.end(), buf.begin(), buf.end());
    }
    const double fJust = freqOf(justAfter);
    std::printf("  first 27.8 ms after switch:   %.0f Hz (expect ~1997; a "
                "CPU-now clockScale gives ~3994)\n", fJust);
    assert(fJust < 2600.0 &&
           "the mode switch retuned audio whose cycles the cursor has not "
           "reached yet");

    // ...and once the cursor walks past the switch's own stamp, the octave
    // is real. (Without this the pin would pass on a card that simply
    // ignored the mode.)
    pump(10, nullptr);
    std::vector<float> later;
    pump(10, &later);
    const double fLater = freqOf(later);
    std::printf("  settled in Phasor-native:     %.0f Hz (expect ~3994)\n", fLater);
    assert(fLater > 3400.0 && fLater < 4400.0 &&
           "the doubled Phasor chip clock never arrived");

    std::puts("phasor_mode_scale_timeline OK");
    return 0;
}
