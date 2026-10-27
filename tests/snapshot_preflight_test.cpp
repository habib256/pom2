// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "IIcExternalSmartPort.h"
#include "IIcMouse.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "MemoryProfile_IIcClass.h"
#include "NoSlotClock.h"
#include "SlotBus.h"
#include "SmartPortHub.h"
#include "SnapshotIO.h"
#include "Sony35Drive.h"
#include "ZipChip.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
using Blob = std::vector<uint8_t>;

uint32_t u32(const Blob& b, size_t pos)
{
    assert(pos <= b.size() && b.size() - pos >= 4);
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v |= uint32_t(b[pos + k]) << (8 * k);
    return v;
}
void put32(Blob& b, size_t pos, uint32_t v)
{
    assert(pos <= b.size() && b.size() - pos >= 4);
    for (int k = 0; k < 4; ++k) b[pos + k] = uint8_t(v >> (8 * k));
}
void append32(Blob& b, uint32_t v)
{
    const size_t pos = b.size(); b.resize(pos + 4); put32(b, pos, v);
}
Blob capture(M6502& cpu, Memory& mem)
{
    Blob b;
    pom2::SnapshotWriter w(b);
    pom2::captureMachineState(w, cpu, mem);
    assert(w.finish());
    return b;
}
Blob mex(Memory& mem) { Blob b; mem.appendSnapshotState(b); return b; }

Blob replaceMex(const Blob& full, const Blob& replacement)
{
    Blob out;
    pom2::SnapshotReader r(full.data(), full.size());
    pom2::SnapshotWriter w(out);
    std::string name;
    uint32_t len;
    while (r.nextSection(name, len)) {
        Blob b(len);
        if (len) r.readBytes(b.data(), len);
        if (name == "MEX") b = replacement;
        w.writeSection(name, b.data(), b.size());
    }
    assert(r.good() && w.finish());
    return out;
}

struct Parts {
    Blob core;
    std::vector<Blob> devices;
    explicit Parts(const Blob& b)
    {
        constexpr size_t bankOffset = 1 + 32 + 0x4000;
        const uint32_t banks = u32(b, bankOffset);
        size_t pos = bankOffset + 5;
        pos += banks <= 1 ? 0x14000 : 4 + u32(b, pos);
        ++pos; // SmartPort arming latch
        assert(pos <= b.size());
        core.assign(b.begin(), b.begin() + pos);
        while (pos < b.size()) {
            const uint32_t len = u32(b, pos); pos += 4;
            assert(len <= b.size() - pos);
            devices.emplace_back(b.begin() + pos, b.begin() + pos + len);
            pos += len;
        }
    }
    Blob bytes() const
    {
        Blob b = core;
        for (const Blob& d : devices) {
            append32(b, uint32_t(d.size()));
            b.insert(b.end(), d.begin(), d.end());
        }
        return b;
    }
};

struct DmaSentinel final : SlotPeripheral {
    int resets = 0;
    std::string_view name() const override { return "preflight DMA sentinel"; }
    bool dmaActive() const override { return true; }
    void onReset() override { ++resets; }
};

struct Rig {
    pom2::IWMDevice iwm;
    pom2::NoSlotClock clock;
    pom2::Sony35Drive internal, external, external2;
    pom2::SmartPortHub hub;
    pom2::ZipChip zip;
    Memory memory;
    M6502 cpu{&memory};
    pom2::IIcExternalSmartPort port{&memory.slotBus()};
    DmaSentinel* dma = nullptr;
    Rig()
    {
        memory.setIIEMode(true);
        memory.setCpu(&cpu);
        memory.setIWM(&iwm);
        memory.setNoSlotClock(&clock);
        memory.setCpuSocketAccelerator(&zip);
        hub.attach(&iwm);
        hub.setSony35(&internal, &external, &external2);
        memory.setSmartPortHub(&hub);
        memory.setExternalSmartPort(&port);
        Memory::AppleIIRomImage rom;
        rom.path = "in-memory preflight //c+ fixture";
        rom.bytes.assign(0x8000, 0xEA);
        rom.bytes[0x3BC0] = 0; rom.bytes[0x3BBF] = 5;
        assert(memory.loadAppleIIRom(rom, true));
        memory.slotBus().plug(4, std::make_unique<IIcMouse>());
        auto sentinel = std::make_unique<DmaSentinel>();
        dma = sentinel.get();
        memory.slotBus().plug(7, std::move(sentinel));
        // Keep a pending REQ and non-default register tracker on the external
        // port. No disk, host connection or media write is involved.
        port.write(1, 0, 64);
        assert(port.registers().phases() & 1);
        iwm.tick(128);
        clock.interceptRead(0xF801, 0xEA);
        memory.setCycleCounter(4096);
        cpu.setProgramCounter(0x4567);
        memory.writeRamUnchecked(0x2000, 0xA7);
    }
    void armEvents()
    {
        memory.memWrite(0xC050, 0);
        memory.beginVideoEventFrame();
        memory.memWrite(0xC051, 0);
        memory.memWrite(0xC050, 0);
    }
    void checkEvents()
    {
        const auto events = memory.takeVideoEvents();
        assert(events.size() == 2);
        assert(events[0].value && !events[1].value);
        assert(events[0].emuCycle == events[1].emuCycle);
    }
    void refuse(const Blob& candidateMex, bool malformed = true)
    {
        armEvents();
        const Blob before = capture(cpu, memory);
        const uint32_t irq = cpu.getIrqSourceMask();
        // Direct restore must refuse without side effects too, not only the
        // transactional MachineSnapshot wrapper.
        if (malformed) {
            assert(!memory.loadSnapshotState(candidateMex.data(), candidateMex.size()));
            assert(capture(cpu, memory) == before);
            assert(cpu.getIrqSourceMask() == irq && dma->resets == 0);
        }
        const Blob file = replaceMex(before, candidateMex);
        pom2::SnapshotReader r(file.data(), file.size());
        const auto result = pom2::restoreMachineState(r, cpu, memory);
        assert(!result.ok && "malformed MEX was accepted");
        assert(capture(cpu, memory) == before);
        assert(cpu.getIrqSourceMask() == irq);
        assert(dma->resets == 0);
        checkEvents();
    }
};

void testPurePreflight()
{
    // Two IWMs exceed MSVC's default 1 MiB stack before the fixture's RAM.
    auto storage = std::make_unique<Rig>();
    Rig& rig = *storage;
    rig.memory.setVideoClockRatio(0.25);   // an accelerated beam: its section is not empty
    const Blob valid = mex(rig.memory);
    Parts parts(valid);
    assert(parts.devices.size() == 10);
    for (const Blob& b : parts.devices) assert(!b.empty());
    // Valid preflight is as inert as refusal, including the detached port
    // decoder's callbacks. Repeat to catch a hidden reset-on-validate.
    rig.armEvents();
    const Blob before = capture(rig.cpu, rig.memory);
    const uint32_t irq = rig.cpu.getIrqSourceMask();
    for (int i = 0; i < 3; ++i) {
        uint32_t banks = 0;
        assert(rig.memory.validateSnapshotState(valid.data(), valid.size(), &banks));
        assert(banks == 1);
        assert(capture(rig.cpu, rig.memory) == before);
        assert(rig.cpu.getIrqSourceMask() == irq && rig.dma->resets == 0);
    }
    rig.checkEvents();

    // Corrupt each nested device independently. A valid outer section length
    // must never disguise a malformed IWM/MIG/clock/drive/mouse/Zip payload.
    for (size_t k = 0; k < parts.devices.size(); ++k) {
        Parts bad = parts;
        if (k == 2) bad.devices[k].resize(3); // paging/IOU section minimum
        else if (k == 9) bad.devices[k][23] ^= 0xFF;   // beam clock: the ratio's sign
        else bad.devices[k][0] ^= 0xFF;
        rig.refuse(bad.bytes());
    }
    // A valid MIG wrapping a named but corrupt external SmartPort section.
    Parts bad = parts;
    constexpr size_t portStart = 4 + 2 + 0x800 + 3;
    assert(bad.devices[1].size() > portStart + 8);
    assert(bad.devices[1][portStart] == 'X');
    put32(bad.devices[1], portStart + 4, 0xFFFFFFFF);
    rig.refuse(bad.bytes());
    // Every historical MIG tail ends at a complete field/tag boundary.
    // Partial ROMBANK fields, XSP tags or S35 tags are torn, not legacy.
    for (size_t n : {portStart - 2, portStart - 1, portStart + 1,
                     portStart + 2, parts.devices[1].size() - 1}) {
        Parts torn = parts;
        torn.devices[1].resize(n);
        rig.refuse(torn.bytes());
    }

    // Truncations of the core and each length/payload. Optional trailers at
    // clean boundaries remain legal for older files; all others must refuse.
    std::vector<size_t> cuts{0, 1, 32, 33, 16417, 16421, parts.core.size() - 2};
    size_t pos = parts.core.size();
    for (const Blob& b : parts.devices) {
        cuts.push_back(pos + 1); cuts.push_back(pos + 3);
        cuts.push_back(pos + 4 + b.size() - 1);
        pos += 4 + b.size();
    }
    for (size_t n : cuts) {
        Blob truncated(valid.begin(), valid.begin() + n);
        if (!rig.memory.validateSnapshotState(truncated.data(), truncated.size()))
            rig.refuse(truncated);
    }
    // ENV may match while MEX names another valid geometry. The semantic
    // preflight must catch that discrepancy before any apply/rollback.
    rig.memory.setRamWorksBanks(4);
    const Blob expanded = mex(rig.memory);
    rig.memory.setRamWorksBanks(1);
    uint32_t banks = 0;
    assert(rig.memory.validateSnapshotState(expanded.data(), expanded.size(), &banks));
    assert(banks == 4);
    rig.refuse(expanded, false);
    // The full valid file still applies: the old timeline's DMA is disarmed.
    const Blob full = capture(rig.cpu, rig.memory);
    rig.cpu.setProgramCounter(0x1111);
    pom2::SnapshotReader r(full.data(), full.size());
    assert(pom2::restoreMachineState(r, rig.cpu, rig.memory).ok);
    assert(rig.cpu.getProgramCounter() == 0x4567);
    assert(rig.dma->resets == 1);
}

void testGeometry()
{
    Memory mem;
    mem.setIIEMode(true);
    mem.setRamWorksBanks(4);
    const Blob valid = mex(mem);
    uint32_t banks = 0;
    assert(mem.validateSnapshotState(valid.data(), valid.size(), &banks) && banks == 4);
    constexpr size_t offset = 1 + 32 + 0x4000;
    for (uint32_t count : {129u, 0xFFFFFFFFu}) {
        Blob bad = valid; put32(bad, offset, count);
        assert(!mem.validateSnapshotState(bad.data(), bad.size()));
    }
    Blob bad = valid;
    bad[offset + 4] = 4; // selected bank outside the four-bank backing
    assert(!mem.validateSnapshotState(bad.data(), bad.size()));
    bad = valid;
    put32(bad, offset + 5, 0x14000); // four declared banks, only one in backing
    assert(!mem.validateSnapshotState(bad.data(), bad.size()));
    assert(mex(mem) == valid);
    // Header/core-only legacy blobs still load; optional device trailers are
    // not newly mandatory. Unknown future trailer bytes are ignored as before.
    Parts parts(valid);
    assert(mem.validateSnapshotState(parts.core.data(), parts.core.size()));
    Blob future = valid;
    future.push_back(0x99);
    assert(mem.validateSnapshotState(future.data(), future.size()));
}

void testProfileRefusalIsAtomic()
{
    pom2::SmartPortHub hub;
    auto port = std::make_unique<pom2::IIcExternalSmartPort>(nullptr);
    Blob firmware(0x4000, 0);
    firmware[0x3BBF] = 5;
    IIcClassProfile profile(firmware.data(), firmware.size(), nullptr, nullptr, &hub, true);
    profile.setExternalSmartPort(port.get());
    port->write(1, 0, 32);
    Blob before;
    profile.appendSnapshotState(before);
    Blob bad = before;
    constexpr size_t portStart = 4 + 2 + 0x800 + 3;
    put32(bad, portStart + 4, 0xFFFFFFFF);
    assert(profile.validateSnapshotState(bad.data(), bad.size()) == 0);
    assert(profile.loadSnapshotState(bad.data(), bad.size()) == 0);
    Blob portBefore;
    port->appendSnapshotState(portBefore);
    Blob badPort = portBefore;
    put32(badPort, 4, 0xFFFFFFFF);
    assert(port->loadSnapshotState(badPort.data(), badPort.size()) == 0);
    Blob portAfter;
    port->appendSnapshotState(portAfter);
    assert(portAfter == portBefore);
    Blob after;
    profile.appendSnapshotState(after);
    assert(after == before);
}
} // namespace

// A blob taken with no accelerator seated says nothing about the one seated
// now: it takes its reset state. The branch that does this was dead code —
// the section reader skipped empty sections before calling it.
void testEmptyAcceleratorSectionResets()
{
    Memory source;
    M6502 sourceCpu{&source};
    source.setCpu(&sourceCpu);
    std::vector<uint8_t> blob;
    source.appendSnapshotState(blob);

    Memory target;
    M6502 targetCpu{&target};
    target.setCpu(&targetCpu);
    pom2::ZipChip zip;
    zip.setBootFast(false);
    zip.onReset(0);                 // the previous machine left it slow…
    zip.setBootFast(true);          // …and its reset state is fast
    assert(!zip.fast());
    target.setCpuSocketAccelerator(&zip);
    assert(target.loadSnapshotState(blob.data(), blob.size()));
    assert(zip.fast() && "an empty accelerator section resets the live chip");
    std::puts("  empty accelerator section resets the seated chip: ok");
}

int main()
{
    testEmptyAcceleratorSectionResets();
    testPurePreflight();
    testGeometry();
    testProfileRefusalIsAtomic();
    std::puts("snapshot_preflight: all ok");
}
