// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SnapshotIO.h"
#include "TranswarpCard.h"
#include "TestTempPath.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
using Blob = std::vector<uint8_t>;

Blob capture(M6502& cpu, Memory& mem, bool rewind = false)
{
    Blob out;
    pom2::SnapshotWriter w(out);
    pom2::captureMachineState(w, cpu, mem, rewind);
    assert(w.finish());
    return out;
}

pom2::RestoreResult restore(const Blob& blob, M6502& cpu, Memory& mem)
{
    pom2::SnapshotReader r(blob.data(), blob.size());
    return pom2::restoreMachineState(r, cpu, mem);
}

// Rewrite a section without depending on offsets in the outer file format.
Blob rewrite(const Blob& blob, const std::string& section, const Blob* replacement,
             bool duplicate = false, bool moveToEnd = false)
{
    Blob out, saved;
    pom2::SnapshotReader r(blob.data(), blob.size());
    pom2::SnapshotWriter w(out);
    std::string name;
    uint32_t len;
    while (r.nextSection(name, len)) {
        Blob payload(len);
        if (len) r.readBytes(payload.data(), len);
        if (name == section) {
            if (!replacement) continue;
            payload = *replacement;
            if (moveToEnd) { saved = payload; continue; }
        }
        w.writeSection(name, payload.data(), payload.size());
        if (duplicate && name == section)
            w.writeSection(name, payload.data(), payload.size());
    }
    if (moveToEnd) w.writeSection(section, saved.data(), saved.size());
    assert(r.good() && w.finish());
    return out;
}

Blob payload(const Blob& blob, const std::string& wanted)
{
    pom2::SnapshotReader r(blob.data(), blob.size());
    std::string name;
    uint32_t len;
    while (r.nextSection(name, len)) {
        if (name != wanted) { r.skipCurrentSection(); continue; }
        Blob result(len);
        if (len) r.readBytes(result.data(), len);
        return result;
    }
    return {};
}

struct DmaSentinel final : SlotPeripheral {
    bool active = true;
    int resets = 0;
    std::string_view name() const override { return "snapshot DMA sentinel"; }
    bool dmaActive() const override { return active; }
    void onReset() override { active = false; ++resets; }
};

// Refusal must not visit even the reversible MEX apply/rollback path: that
// resets the beam log. Compare CPU/RAM/MEX bytes, DMA and pending video edges.
void refusedWithoutMutation(const Blob& candidate, M6502& cpu, Memory& mem,
                            const char* diagnostic)
{
    cpu.setProgramCounter(0x4321);
    mem.writeRamUnchecked(0x4000, 0xA7);
    mem.setCycleCounter(123456);
    auto sentinel = std::make_unique<DmaSentinel>();
    auto* dma = sentinel.get();
    mem.slotBus().plug(7, std::move(sentinel));
    mem.beginVideoEventFrame();
    mem.memWrite(0xC051, 0);
    mem.memWrite(0xC050, 0);
    const Blob before = capture(cpu, mem);
    const auto result = restore(candidate, cpu, mem);
    assert(!result.ok);
    if (result.error.find(diagnostic) == std::string::npos)
        std::fprintf(stderr, "Expected %s, got %s\n", diagnostic, result.error.c_str());
    assert(result.error.find(diagnostic) != std::string::npos);
    assert(capture(cpu, mem) == before);
    assert(dma->active && dma->resets == 0);
    assert(!mem.takeVideoEvents().empty());
    mem.slotBus().unplug(7);
}

void installRom(Memory& mem, const Blob& bytes, bool iic = false)
{
    Memory::AppleIIRomImage image;
    image.path = "in-memory snapshot fixture";
    image.bytes = bytes;
    assert(mem.loadAppleIIRom(image, iic));
}

void testFirmwareAndConfiguration()
{
    Memory mem;
    mem.setIIEMode(true);
    M6502 cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);
    Blob rom(0x4000, 0xEA);
    installRom(mem, rom);
    const Blob golden = capture(cpu, mem);
    assert(!payload(golden, "ENV").empty());
    assert(payload(capture(cpu, mem, true), "ENV").empty());

    // Main firmware, then internal I/O firmware with the SAME main ROM.
    rom[0x3000] ^= 1;
    installRom(mem, rom);
    refusedWithoutMutation(golden, cpu, mem, "firmware differs");
    rom[0x3000] ^= 1;
    rom[0x0100] ^= 1;
    installRom(mem, rom);
    refusedWithoutMutation(golden, cpu, mem, "firmware differs");
    rom[0x0100] ^= 1;
    installRom(mem, rom);

    mem.setVideoStandard(VideoStandard::PAL);
    refusedWithoutMutation(golden, cpu, mem, "PAL/NTSC");
    mem.setVideoStandard(VideoStandard::NTSC);
    mem.setRamWorksBanks(4);
    refusedWithoutMutation(golden, cpu, mem, "ramworks_banks=1");
    const Blob expanded = capture(cpu, mem);
    mem.memWrite(0xC073, 3);
    assert(mem.ramWorksBank() == 3);
    assert(payload(capture(cpu, mem), "ENV") == payload(expanded, "ENV"));
    assert(restore(expanded, cpu, mem).ok);
    mem.setRamWorksBanks(1);

    // Paging, auxiliary bank selection and read/write watchpoints are STATE,
    // not configuration: matching machines must still load successfully.
    mem.memWrite(0xC003, 0); // RAMRD
    mem.memWrite(0xC005, 0); // RAMWRT
    mem.setWriteWatch(0xF000, true);
    assert(restore(golden, cpu, mem).ok);
    mem.setWriteWatch(0xF000, false);

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    refusedWithoutMutation(golden, cpu, mem, "NMOS");
    cpu.setCpuMode(M6502::CpuMode::NMOS);

    Blob env = payload(golden, "ENV");
    refusedWithoutMutation(rewrite(golden, "ENV", &env, true), cpu, mem, "duplicate ENV");
    env[0] = 255;
    refusedWithoutMutation(rewrite(golden, "ENV", &env), cpu, mem, "ENV version");
    env.resize(7);
    refusedWithoutMutation(rewrite(golden, "ENV", &env), cpu, mem, "invalid length");
    env.resize(65537);
    refusedWithoutMutation(rewrite(golden, "ENV", &env), cpu, mem, "invalid length");
    env = payload(golden, "ENV");
    env.back() ^= 1;
    // Even an ENV placed LAST must be rejected before applying CPU/MEM/MEX.
    refusedWithoutMutation(rewrite(golden, "ENV", &env, false, true), cpu, mem, "firmware differs");

    Blob cpuBytes = payload(golden, "CPU");
    cpuBytes[7] = 2;
    refusedWithoutMutation(rewrite(golden, "CPU", &cpuBytes), cpu, mem, "CPU mode is invalid");

    // Legacy files remain accepted, explicitly without firmware verification,
    // and must never replace the live ROM with the MEM section's old bytes.
    rom[0x3000] ^= 1;
    installRom(mem, rom);
    assert(restore(rewrite(golden, "ENV", nullptr), cpu, mem).ok);
    assert(mem.data()[0xF000] == rom[0x3000]);
}

void testAlternateBank()
{
    Memory mem;
    mem.setIIEMode(true);
    M6502 cpu(&mem);
    Blob rom(0x8000, 0xEA);
    rom[0x3BC0] = 0; // //c-class ROM discriminator
    installRom(mem, rom, true);
    const Blob golden = capture(cpu, mem);
    // Merely selecting bank 1 must not change identity.
    (void)mem.memRead(0xC028);
    assert(payload(capture(cpu, mem), "ENV") == payload(golden, "ENV"));
    assert(restore(golden, cpu, mem).ok);
    // A ROM update solely in the normally invisible bank must be refused.
    rom[0x5000] ^= 1;
    installRom(mem, rom, true);
    refusedWithoutMutation(golden, cpu, mem, "firmware differs");
}

void testCharacterRom()
{
    const std::string path = pom2test::tempPath("pom2-snapshot-env.chr");
    struct Cleanup { std::string path; ~Cleanup() { std::filesystem::remove(path); } } cleanup{path};
    Blob characters(4096, 0xA5);
    auto writeChars = [&]() {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(characters.data()), characters.size());
        assert(out.good());
    };
    Memory mem;
    mem.setIIEMode(true);
    M6502 cpu(&mem);
    writeChars();
    assert(mem.loadCharRom(path.c_str()));
    const Blob golden = capture(cpu, mem);
    characters[17] ^= 1;
    writeChars();
    assert(mem.loadCharRom(path.c_str()));
    refusedWithoutMutation(golden, cpu, mem, "firmware differs");
    characters[17] ^= 1;
    writeChars();
    assert(mem.loadCharRom(path.c_str()));
    assert(restore(golden, cpu, mem).ok);
}

void testTranswarpShadow()
{
    Memory mem;
    mem.setIIEMode(true);
    M6502 cpu(&mem);
    const Blob golden = capture(cpu, mem);
    auto card = std::make_unique<pom2::TranswarpCard>(3);
    card->setMemory(&mem);
    assert(card->setRom(Blob(4096, 0xEA)));
    auto* tw = card.get();
    mem.slotBus().plug(3, std::move(card));
    assert(tw->shadowActive());
    assert(mem.data()[0xF000] == 0xEA);
    assert(payload(capture(cpu, mem), "ENV") == payload(golden, "ENV"));
    tw->busSnoop(0xC072, true, 0);
    assert(!tw->shadowActive());
    assert(payload(capture(cpu, mem), "ENV") == payload(golden, "ENV"));
    // Unplug while Memory still exists (the card releases its shadow).
    mem.slotBus().unplug(3);
}
} // namespace

int main()
{
    testFirmwareAndConfiguration();
    testAlternateBank();
    testCharacterRom();
    testTranswarpShadow();
    std::puts("snapshot_environment: all ok");
}
