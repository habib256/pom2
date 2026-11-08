// POM2 adapter regression against Sébastien Pourcel's unmodified MEA8000.
#include "Mea8000.h"
#include "PorteParoleCard.h"
#include "../third_party/porteparole-mea8000/src/Mea8000.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main()
{
    pom2::Mea8000 adapted;
    ::Mea8000 reference;
    pom2::PorteParoleCard card;
    card.setCpuClock(4000000); // One CPU cycle is one quartz clock.
    card.setSampleRate(48000);
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    auto advance = [&](int clocks) {
        std::vector<int16_t> a, b;
        adapted.advanceClocks(clocks, &a);
        reference.AdvanceClocks(clocks, &b);
        card.advanceCycles(clocks);
        const auto c = card.drainSamples(100000);
        check(a == b, "PCM matches reference exactly");
        check(c.size() == b.size(), "card schedules the reference sample count");
        for (size_t i = 0; i < c.size() && i < b.size(); ++i)
            if (c[i] != float(b[i]) / 32768.0f) { check(false, "card PCM matches reference"); break; }
        check(adapted.status() == reference.ReadStatus() &&
              (card.deviceSelectRead(0) & 0x80) == reference.ReadStatus(), "REQ agrees");
        check(int(adapted.state()) == int(reference.GetMode()), "mode agrees");
    };
    auto send = [&](uint8_t value) {
        while (!reference.ReadStatus()) advance(83);
        check(adapted.writeData(value) == reference.WriteData(value), "byte acceptance agrees");
        card.deviceSelectWrite(0, value);
        // Busy writes must be ignored, including after the starting pitch.
        check(!adapted.writeData(255) && !reference.WriteData(255), "busy write is dropped");
        card.deviceSelectWrite(0, 255);
        advance(11); advance(1);
    };
    auto command = [&](uint8_t value) {
        adapted.writeCommand(value); reference.WriteCommand(value); card.deviceSelectWrite(1, value);
    };
    send(60);
    // Mix all durations, voiced/noise sources, pitch directions and FM2 code 16.
    for (int i = 0; i < 32; ++i) {
        for (uint8_t v : {uint8_t(i & 1 ? 0xff : 0x55), uint8_t(0xb0),
                          uint8_t(0x97), uint8_t(0x80 | ((i % 4) << 5) | (i % 32))}) send(v);
        advance(7919);
    }
    command(0x0c); advance(40000); advance(137);
    std::vector<uint8_t> snapshot;
    adapted.appendSnapshot(snapshot);
    check(snapshot.size() == pom2::Mea8000::kSnapshotBytes, "snapshot layout size");
    pom2::Mea8000 restored;
    check(restored.loadSnapshot(snapshot.data(), snapshot.size()), "snapshot loads");
    std::vector<int16_t> a, b;
    restored.advanceClocks(100001, &a);
    auto copy = adapted;
    for (int i = 0; i < 100001; ++i) copy.advanceClocks(1, &b);
    check(a == b, "restore and chunk boundaries preserve PCM");
    auto before = snapshot;
    snapshot[3] = 1;
    check(!adapted.loadSnapshot(snapshot.data(), snapshot.size()), "old engine version rejected");
    snapshot = before; snapshot.resize(snapshot.size() - 1);
    check(!adapted.loadSnapshot(snapshot.data(), snapshot.size()), "truncation rejected");
    snapshot = before;
    // Corrupt mode with an invalid finite double.
    double invalid = 99; uint64_t bits; std::memcpy(&bits, &invalid, 8);
    for (int i = 0; i < 8; ++i) snapshot[4+i] = uint8_t(bits >> (8*i));
    check(!adapted.loadSnapshot(snapshot.data(), snapshot.size()), "invalid mode rejected");
    snapshot.clear(); adapted.appendSnapshot(snapshot);
    check(snapshot == before, "invalid load is atomic");
    command(0x10); advance(40000);
    for (int i = 0; i < 100; ++i) advance(40000); // resonators settle during STOP
    send(60);
    for (uint8_t v : {0xff, 0x63, 0x90, 0x00}) send(v);
    advance(30720);
    // Host rate affects queued audio only; quartz state is identical at every rate.
    std::vector<uint8_t> machine;
    for (uint32_t rate : {8000u, 44100u, 48000u, 96000u, 192000u}) {
        pom2::PorteParoleCard c;
        c.setCpuClock(4000000); c.setSampleRate(rate);
        c.advanceCycles(40000);
        check(c.queuedSamples() == rate / 100, "resampling count at each supported rate");
        std::vector<uint8_t> state; c.appendSnapshotState(state);
        if (machine.empty()) machine = state;
        check(state == machine, "host rate does not affect machine state");
    }
    pom2::PorteParoleCard whole, split;
    for (auto* c : {&whole, &split}) {
        c->setCpuClock(4000000); c->setSampleRate(44100);
        c->deviceSelectWrite(1, 0x0c);
        for (uint8_t v : {60, 0x55, 0x63, 0x97, 0x80}) {
            c->deviceSelectWrite(0, v); c->advanceCycles(12);
        }
    }
    whole.advanceCycles(100001);
    for (int i = 0; i < 100001; ++i) split.advanceCycles(1);
    check(whole.drainSamples(100000) == split.drainSamples(100000),
          "host resampling survives arbitrary advance boundaries");
    // Exact PAL quartz boundary: 65 CPU cycles = 256 quartz clocks.
    // Neither instruction slicing nor DMA-sized batches may change it.
    for (int hz : {1015625, 1022727}) {
        pom2::PorteParoleCard bulk, sliced;
        bulk.setCpuClock(hz); sliced.setCpuClock(hz);
        bulk.advanceCycles(hz);
        for (int i = 0; i < hz; ++i) sliced.advanceCycles(1);
        std::vector<uint8_t> bulkState, slicedState;
        bulk.appendSnapshotState(bulkState); sliced.appendSnapshotState(slicedState);
        check(bulkState == slicedState, "one second of quartz time is independent of CPU slicing");
    }
    // v2 snapshots remain readable. Use a half-clock phase, exactly
    // representable in both formats, to verify continuation after migration.
    {
        pom2::PorteParoleCard source;
        source.setCpuClock(8000000);
        source.advanceCycles(1);
        std::vector<uint8_t> legacy;
        source.appendSnapshotState(legacy);
        legacy[3] = 2;
        legacy.resize(legacy.size() - 8); // remove the v3 frequency denominator
        double phase = 0.5; uint64_t bits;
        std::memcpy(&bits, &phase, 8);
        for (int i = 0; i < 8; ++i) legacy[legacy.size()-8+i] = uint8_t(bits >> (8*i));
        pom2::PorteParoleCard migrated;
        migrated.setCpuClock(8000000);
        migrated.loadSnapshotState(legacy.data(), legacy.size());
        source.advanceCycles(1003); migrated.advanceCycles(1003);
        std::vector<uint8_t> expected, actual;
        source.appendSnapshotState(expected); migrated.appendSnapshotState(actual);
        check(expected == actual, "v2 quartz phase migrates to v3");
        // A damaged v3 frequency must leave the existing state untouched.
        auto damaged = expected;
        for (size_t i = damaged.size()-8; i < damaged.size(); ++i) damaged[i] = 0;
        migrated.loadSnapshotState(damaged.data(), damaged.size());
        actual.clear(); migrated.appendSnapshotState(actual);
        check(actual == expected, "invalid saved frequency is rejected atomically");
    }
    std::printf("porte_parole_reference: %d failures\n", failures);
    return failures ? 1 : 0;
}
