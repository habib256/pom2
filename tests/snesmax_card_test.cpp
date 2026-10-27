// POM2 — GPL-3.0-or-later
#include "SnesMaxCard.h"
#include "SlotBus.h"
#include "SlotCardFactory.h"
#include "SlotConfigurationCoordinator.h"
#include <cassert>
#include <memory>
#include <vector>

using pom2::SnesMaxCard;
static std::vector<uint8_t> capture(const SnesMaxCard& card)
{
    std::vector<uint8_t> blob;
    card.appendSnapshotState(blob);
    return blob;
}
int main()
{
    for (int slot = 1; slot <= 7; ++slot) {
        auto made = pom2::SlotCardFactory{}.create({"snesmax", slot});
        assert(made);
        auto* card = dynamic_cast<SnesMaxCard*>(made.card.get());
        assert(card && card->getSlot() == slot);
        SlotBus bus;
        bus.plug(slot, std::move(made.card));
        assert(pom2::SlotConfigurationCoordinator{}.captureLive(bus).keys[slot] == "snesmax");
    }
    SnesMaxCard card(4);
    assert(card.deviceSelectRead(0) == 0xFF);
    const uint16_t p0 = SnesMaxCard::B | SnesMaxCard::Start | SnesMaxCard::A | SnesMaxCard::R;
    const uint16_t p1 = SnesMaxCard::Y | SnesMaxCard::Select | SnesMaxCard::Left | SnesMaxCard::X;
    card.setButtons(0, p0); card.setButtons(1, p1);
    card.deviceSelectWrite(0, 0);
    card.advanceCycles(6);
    assert(card.deviceSelectRead(3) == 0xFF);
    const auto pendingLatch = capture(card);
    SnesMaxCard twin(4);
    twin.setButtons(0, p0); twin.setButtons(1, p1);
    twin.loadSnapshotState(pendingLatch.data(), pendingLatch.size());
    card.advanceCycles(1); twin.advanceCycles(1);
    assert(capture(card) == capture(twin));
    for (int bit = 0; bit < 16; ++bit) {
        const uint8_t expected = static_cast<uint8_t>(0x3F |
            ((p0 & (1u << bit)) ? 0 : 0x80) | ((p1 & (1u << bit)) ? 0 : 0x40));
        // Only $C0n0 is driven, and only its bits 7-6; the rest is the
        // floating bus — $FF on a standalone card (AppleWin SNESMAX.cpp, by
        // the card's designer). Reads never clock.
        assert(card.deviceSelectRead(0) == expected);
        for (int mirror = 1; mirror < 16; ++mirror)
            assert(card.deviceSelectRead(mirror) == 0xFF);
        // A changed live pad cannot change the latched serial word.
        card.setButtons(0, 0); card.setButtons(1, 0);
        const auto saved = capture(card);
        SnesMaxCard restored(4);
        restored.loadSnapshotState(saved.data(), saved.size());
        card.deviceSelectWrite(15, 0xFF); restored.deviceSelectWrite(1, 0);
        assert(capture(card) == capture(restored));
    }
    assert(card.deviceSelectRead(0) == 0x3F); // trailing serial bits are high before inversion
    card.setButtons(0, SnesMaxCard::Up | SnesMaxCard::Down | SnesMaxCard::Left | SnesMaxCard::Right);
    card.deviceSelectWrite(14, 0); card.advanceCycles(7);
    for (int bit = 0; bit < 8; ++bit) {
        const bool pressed = bit == 4 || bit == 6;
        assert(bool(card.deviceSelectRead(0) & 0x80) != pressed);
        card.deviceSelectWrite(1, 0);
    }
    const auto saved = capture(card);
    for (size_t length = 0; length < saved.size(); ++length) {
        card.loadSnapshotState(saved.data(), length);
        assert(capture(card) == saved);
    }
    auto corrupt = saved;
    for (size_t n = 5; n < 13; ++n) corrupt[n] = 0;
    corrupt[11] = 0x20; corrupt[12] = 0x40; // IEEE binary64 8.0, outside the pulse
    card.loadSnapshotState(corrupt.data(), corrupt.size());
    assert(capture(card) == saved);
    corrupt = saved; corrupt[0] = '?';
    card.loadSnapshotState(corrupt.data(), corrupt.size());
    assert(capture(card) == saved);
    card.onReset();
    assert(capture(card) == saved); // RESET is not wired to the SNES connector
    card.onPowerCycle();
    assert(card.deviceSelectRead(0) == 0xFF);

    // Accelerator clocks do not accelerate the slot's latch generator.
    card.setCpuClock(4000000.0); card.setStandardClock(1000000.0);
    card.setButtons(0, SnesMaxCard::B);
    card.deviceSelectWrite(0, 0);
    card.advanceCycles(1);
    const auto fractional = capture(card);
    SnesMaxCard acceleratedTwin(4);
    acceleratedTwin.setCpuClock(4000000.0); acceleratedTwin.setStandardClock(1000000.0);
    acceleratedTwin.setButtons(0, SnesMaxCard::B);
    acceleratedTwin.loadSnapshotState(fractional.data(), fractional.size());
    card.advanceCycles(26); acceleratedTwin.advanceCycles(26);
    assert(card.deviceSelectRead(0) == 0xFF);
    card.advanceCycles(1); acceleratedTwin.advanceCycles(1);
    assert(card.deviceSelectRead(0) == 0x7F);
    assert(capture(card) == capture(acceleratedTwin));

    // The exact slot bus dispatch, including mirrors and paced latch.
    SlotBus bus;
    auto device = std::make_unique<SnesMaxCard>(4);
    device->setButtons(0, SnesMaxCard::B);
    bus.plug(4, std::move(device));
    bus.deviceSelectWrite(0xC0C0, 0);
    bus.advanceCycles(7);
    assert(bus.deviceSelectRead(0xC0C0) == 0x7F);
    assert(bus.deviceSelectRead(0xC0CF) == 0xFF);   // undriven: open bus here
}
