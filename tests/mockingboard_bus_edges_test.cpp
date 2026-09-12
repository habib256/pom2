// Bug hunt #18 — four Mockingboard edges, each one a defect this file pins.
//
//   1. A DDR write moves PINS. The VIA reported a port change only when the
//      DDR-masked OUTPUT LATCH changed, so a bit whose latch is 0 going from
//      pulled-up 1 to driven 0 raised nothing — and on this card that pin can
//      be PB2, the AY's /RESET.
//   2. The diagnostic peek returned the RAW counter where a guest read returns
//      `counter - 1`, so the number a raster-timing investigation would trust
//      was one too high on all four counter bytes.
//   3. `$C0(8+n)X` answered a hard $FF instead of the floating bus (MAME's
//      ayboard base: `read_c0nx` → `return get_open_bus();`).
//   4. A rewind re-seeded the AY generators, so every envelope re-attacked at
//      the top of its ramp on every seek.

#include "AudioSource.h"
#include "Mockingboard.h"
#include "SlotBus.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

// PB0 = BC1, PB1 = BDIR, PB2 = /RESET (active low).
constexpr uint8_t kPbInactive = 0x04;
constexpr uint8_t kPbWrite    = 0x06;
constexpr uint8_t kPbLatch    = 0x07;

// `pbMask` is what DDRB drives: $07 is the usual "BC1 + BDIR + /RESET",
// $03 the idiom that leaves /RESET to the board's pull-up — and there the
// ORB latch's bit 2 stays 0, which is what makes the DDR test below an
// edge at all.
void ayWrite(MockingboardCard& c, uint8_t reg, uint8_t v, uint8_t pbMask = 0x07)
{
    c.slotRomWrite(0x01, reg);
    c.slotRomWrite(0x00, static_cast<uint8_t>(kPbLatch & pbMask));
    c.slotRomWrite(0x00, static_cast<uint8_t>(kPbInactive & pbMask));
    c.slotRomWrite(0x01, v);
    c.slotRomWrite(0x00, static_cast<uint8_t>(kPbWrite & pbMask));
    c.slotRomWrite(0x00, static_cast<uint8_t>(kPbInactive & pbMask));
}

// ── 1. Taking /RESET over as an output is an edge the AY must see ────────
//
// The driver idiom this covers is the one DEV.md documents: drive BC1 + BDIR
// only (DDRB = $03) and leave /RESET to the board's pull-up. When such a
// driver later takes PB2 over as an output with ORB's bit 2 still 0, the pin
// goes 1 → 0: on hardware the AY is reset, and its register bank goes with it.
void testDdrWriteDeliversTheResetEdge()
{
    MockingboardCard card(4);
    card.slotRomWrite(0x03, 0xFF);        // DDRA = $FF
    card.slotRomWrite(0x02, 0x03);        // DDRB = $03 — /RESET undriven
    ayWrite(card, 13, 0x0E, 0x03);        // envelope shape
    ayWrite(card, 7, 0x38, 0x03);         // mixer
    assert(card.getAyRegister(0, 13) == 0x0E && card.getAyRegister(0, 7) == 0x38);

    card.slotRomWrite(0x02, 0x07);        // DDRB = $07: PB2 driven, latch = 0
    assert(card.getAyRegister(0, 13) == 0x00 &&
           card.getAyRegister(0, 7) == 0x00 &&
           "a DDR write that drives /RESET low must reset the AY");

    // …and the mirror image: releasing a driven-low pin back to the pull-up
    // is an edge too, so the chip comes out of reset and stores again.
    card.slotRomWrite(0x02, 0x03);        // /RESET back to the pull-up
    ayWrite(card, 7, 0x3F, 0x03);
    assert(card.getAyRegister(0, 7) == 0x3F &&
           "the AY must come out of reset when the pin is released");
    std::printf("  ok: a DDR write delivers the /RESET edge\n");
}

// ── 2. The panel's counter read-back is the guest's ──────────────────────
void testPeekMatchesTheGuestReadback()
{
    MockingboardCard card(4);
    card.slotRomWrite(0x06, 0x00);        // T1 latch low
    card.slotRomWrite(0x05, 0x20);        // T1CH: latch high + start
    card.slotRomWrite(0x08, 0x34);        // T2 low
    card.slotRomWrite(0x09, 0x12);        // T2CH: start
    card.advanceCycles(37);

    // No cycles pass between the two calls (no CPU attached), so the peek
    // must answer exactly what the 6502's LDA would get.
    const struct { uint8_t reg; const char* what; } kRegs[] = {
        {0x04, "T1C-L"}, {0x05, "T1C-H"}, {0x08, "T2C-L"}, {0x09, "T2C-H"},
    };
    for (const auto& r : kRegs) {
        const uint8_t peeked = card.peekViaRegister(0, r.reg);
        const uint8_t read   = card.slotRomRead(r.reg);
        if (peeked != read) {
            std::fprintf(stderr, "peek %s = $%02X, guest read = $%02X\n",
                         r.what, peeked, read);
            std::abort();
        }
    }
    std::printf("  ok: the diagnostic peek matches the guest read-back\n");
}

// ── 3. The device-select window is the floating bus ──────────────────────
void testDeviceSelectIsOpenBus()
{
    SlotBus bus;
    uint8_t floating = 0x5A;
    bus.setFloatingBusSource([&floating]() { return floating; });
    bus.plug(4, std::make_unique<MockingboardCard>(4));
    assert(bus.deviceSelectRead(0xC0C0) == 0x5A);
    floating = 0xA5;                       // the video byte moves…
    assert(bus.deviceSelectRead(0xC0C7) == 0xA5 &&
           "…and the card must not freeze it at $FF");
    std::printf("  ok: $C0(8+n)X reads the floating bus\n");
}

// ── 4. A rewind does not re-attack the envelopes ─────────────────────────
//
// The generators live on the audio thread and are not in the blob (they are
// derived state — the registers are what is restored). A restore therefore
// used to re-seed them, which puts `envStep` back at 15: the top of the ramp.
// A decayed envelope jumped back to full volume on every seek, and a scrub
// does that once per frame.
void testRewindKeepsTheEnvelopeWhereItWas()
{
    constexpr uint32_t kSr = 44100;
    constexpr int      kN  = 2048;
    MockingboardCard card(4);
    card.setSampleRate(kSr);
    card.slotRomWrite(0x03, 0xFF);
    card.slotRomWrite(0x02, 0x07);
    ayWrite(card, 7, 0x3F);               // tone + noise off: the envelope alone
    ayWrite(card, 8, 0x10);               // channel A follows the envelope
    ayWrite(card, 11, 0x00);
    ayWrite(card, 12, 0x08);              // a slow ramp (EP = $0800)
    ayWrite(card, 13, 0x08);              // shape 8: decay, repeating

    AudioSource* src = card.audioSource();
    assert(src);
    std::vector<float> buf(kN);
    auto rms = [&](int frames) {
        double acc = 0.0;
        for (int i = 0; i < frames; ++i) acc += double(buf[i]) * buf[i];
        return std::sqrt(acc / frames);
    };

    // Render until the ramp is well down its decay.
    for (int i = 0; i < 6; ++i) src->fillAudioBuffer(buf.data(), kN);
    const double before = rms(kN);

    std::vector<uint8_t> blob;
    card.appendSnapshotState(blob);
    src->fillAudioBuffer(buf.data(), kN);        // time passes…
    card.loadSnapshotState(blob.data(), blob.size());   // …and a rewind lands
    src->fillAudioBuffer(buf.data(), kN);
    const double after = rms(kN);

    std::printf("  envelope RMS: %.4f before the rewind, %.4f after\n",
                before, after);
    // A re-seeded generator restarts at level 15 — several times the decayed
    // level. Continuity means the same order of magnitude.
    if (!(after < before * 2.0)) {
        std::fprintf(stderr,
            "a rewind re-attacked the envelope: RMS %.4f -> %.4f\n",
            before, after);
        std::abort();
    }
    std::printf("  ok: a rewind leaves the envelope where it was\n");
}

}  // namespace

int main()
{
    testDdrWriteDeliversTheResetEdge();
    testPeekMatchesTheGuestReadback();
    testDeviceSelectIsOpenBus();
    testRewindKeepsTheEnvelopeWhereItWas();
    std::puts("mockingboard_bus_edges OK");
    return 0;
}
