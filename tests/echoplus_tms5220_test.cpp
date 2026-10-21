// POM2 — GPL-3.0-or-later
#include "EchoPlusTMS5220Card.h"
#include "EmulationController.h"
#include "Mockingboard.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <memory>
#include <vector>

void writeAy(EchoPlusTMS5220Card& card, int chip, uint8_t reg, uint8_t value) {
    const uint8_t select = uint8_t(8 << chip);
    card.slotRomWrite(0, select | 4); card.slotRomWrite(1, reg);
    card.slotRomWrite(0, select | 7); card.slotRomWrite(0, select | 4);
    card.slotRomWrite(1, value); card.slotRomWrite(0, select | 6); card.slotRomWrite(0, select | 4);
}
int main() {
    EchoPlusTMS5220Card card;
    card.setCpuClock(1000000);
    assert(card.deviceSelectRead(0) == 0x7F);
    assert(card.deviceSelectRead(1) == 0);
    card.slotRomWrite(3, 0xFF); card.slotRomWrite(2, 0xFF);
    writeAy(card, 0, 0, 64); writeAy(card, 0, 7, 0x3E); writeAy(card, 0, 8, 15);
    assert(card.snapshot().ayRegs[0][8] == 15 && card.snapshot().ayRegs[1][8] == 0);
    writeAy(card, 1, 11, 0xC4);
    // Real PSG readback goes through the VIA's port-A input pins.
    card.slotRomWrite(1, 11); card.slotRomWrite(0, 0x17); card.slotRomWrite(0, 0x14);
    card.slotRomWrite(3, 0); card.slotRomWrite(0, 0x15);
    assert(card.slotRomRead(1) == 0xC4 && card.slotRomRead(0x81) == 0xC4);
    card.slotRomWrite(0, 0x14); card.slotRomWrite(3, 0xFF);
    card.advanceCycles(20000);
    std::array<float, 882> left{}, right{};
    assert(card.audioSource()->fillAudioBufferStereo(left.data(), right.data(), int(left.size())));
    assert(*std::max_element(left.begin(), left.end()) > 0.05f);
    assert(std::all_of(right.begin(), right.end(), [](float v) { return v == 0; }));
    // Fractional speech/PSG clocks survive a state captured between samples.
    card.advanceCycles(791);
    std::vector<float> discard(8192); card.audioSource()->fillAudioBuffer(discard.data(), int(discard.size()));
    std::vector<uint8_t> state; card.appendSnapshotState(state);
    EchoPlusTMS5220Card restored; restored.setCpuClock(1000000);
    restored.loadSnapshotState(state.data(), state.size());
    std::vector<uint8_t> roundTrip; restored.appendSnapshotState(roundTrip); assert(state == roundTrip);
    card.advanceCycles(10000); restored.advanceCycles(10000);
    std::array<float, 441> a{}, b{};
    card.audioSource()->fillAudioBuffer(a.data(), int(a.size()));
    restored.audioSource()->fillAudioBuffer(b.data(), int(b.size()));
    assert(a == b);
    restored.setMuted(true); restored.advanceCycles(10000);
    restored.audioSource()->fillAudioBuffer(b.data(), int(b.size()));
    assert(std::all_of(b.begin(), b.end(), [](float v) { return v == 0; }));
    // VIA timers run on the slot clock even with a 4x CPU.
    EchoPlusTMS5220Card fast;
    fast.setStandardClock(1000000); fast.setCpuClock(4000000);
    fast.slotRomWrite(4, 20); fast.slotRomWrite(5, 0); fast.advanceCycles(40);
    assert(fast.slotRomRead(4) == 11);
    // Reset is a low /RESET pin on PB2, applied to both selected or idle AYs.
    card.slotRomWrite(0, 0);
    assert(card.snapshot().ayRegs[0][8] == 0 && card.snapshot().ayRegs[1][11] == 0);
    card.onReset(); assert(card.deviceSelectRead(0) == 0x7F);
    // LPC voiced frame followed by repeats, packed LSB-first in the FIFO.
    const std::array<uint8_t, 16> voice = {69,9,33,34,34,146,84,165,42,85,169,74,85,170,82,149};
    card.deviceSelectWrite(0, 0x60);
    for (auto byte : voice) card.deviceSelectWrite(0, byte);
    assert(card.deviceSelectRead(0) & 0x80);
    card.advanceCycles(100000);
    std::vector<float> speechLeft(4410), speechRight(4410);
    card.audioSource()->fillAudioBufferStereo(speechLeft.data(), speechRight.data(), 4410);
    assert(speechLeft == speechRight);
    assert(std::any_of(speechLeft.begin(), speechLeft.end(), [](float v) { return std::abs(v) > 0.01f; }));
    card.advanceCycles(1591);
    card.audioSource()->fillAudioBuffer(discard.data(), int(discard.size()));
    std::vector<uint8_t> speaking; card.appendSnapshotState(speaking);
    EchoPlusTMS5220Card restoredVoice; restoredVoice.setCpuClock(1000000);
    restoredVoice.loadSnapshotState(speaking.data(), speaking.size());
    card.advanceCycles(4319); restoredVoice.advanceCycles(4319);
    card.audioSource()->fillAudioBuffer(a.data(), int(a.size()));
    restoredVoice.audioSource()->fillAudioBuffer(b.data(), int(b.size()));
    assert(a == b && card.deviceSelectRead(0) == restoredVoice.deviceSelectRead(0));
    // Compare lazy VIA sync through the actual CPU/Memory/SlotBus path.
    auto controller = std::make_unique<EmulationController>();
    auto machine = controller->lockState();
    auto realEcho = std::make_unique<EchoPlusTMS5220Card>(3);
    auto reference = std::make_unique<MockingboardCard>(4);
    auto* echo = realEcho.get(); auto* mb = reference.get();
    echo->setCpu(&machine.cpu()); mb->setCpu(&machine.cpu());
    machine.memory().slotBus().plug(3, std::move(realEcho));
    machine.memory().slotBus().plug(4, std::move(reference));
    for (int i = 0; i < 64; ++i) machine.memory().writeRamUnchecked(uint16_t(0x300 + i), 0xEA);
    machine.cpu().setProgramCounter(0x300);
    echo->slotRomWrite(4, 100); echo->slotRomWrite(5, 0);
    mb->slotRomWrite(4, 100); mb->slotRomWrite(5, 0);
    for (int i = 0; i < 30; ++i) {
        machine.cpu().step();
        assert(echo->slotRomRead(4) == mb->slotRomRead(4));
    }
    // A backlog built while emulation ran ahead (disk turbo) must not stay
    // as latency: with no rate matching the queue sat at its 8192 cap and
    // the card played ~186 ms late for the rest of the session. One fill
    // snaps it to 50 ms (2205 samples at 44.1 kHz), and that is all left.
    {
        EchoPlusTMS5220Card lag; lag.setCpuClock(1000000);
        lag.slotRomWrite(3, 0xFF); lag.slotRomWrite(2, 0xFF);
        writeAy(lag, 0, 0, 64); writeAy(lag, 0, 7, 0x3E); writeAy(lag, 0, 8, 15);
        lag.advanceCycles(1000000);                    // one second ahead
        std::vector<float> l(441), r(441);
        lag.audioSource()->fillAudioBufferStereo(l.data(), r.data(), 441);
        std::vector<float> kept(2205), keptR(2205);
        lag.audioSource()->fillAudioBufferStereo(kept.data(), keptR.data(), 2205);
        assert(*std::max_element(kept.begin(), kept.end()) > 0.05f);
        lag.audioSource()->fillAudioBufferStereo(l.data(), r.data(), 441);
        assert(std::all_of(l.begin(), l.end(), [](float v) { return v == 0; }) &&
               "the turbo backlog must be trimmed to 50 ms, not kept");
    }
}
