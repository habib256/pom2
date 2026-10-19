// POM2 — GPL-3.0-or-later
#include "Block512Backing.h"
#include "EmulationController.h"
#include "ProDOSHardDiskCard.h"
#include "SessionSnapshot.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SystemProfile.h"
#include "TestTempPath.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
using Blob = std::vector<uint8_t>;
using pom2::Block512Backing;
struct NoHostWrites final : Block512Backing::WriteBackExecutor {
    std::shared_ptr<Block512Backing::CommitBarrier> reserve(std::shared_ptr<Block512Backing::CommitBarrier>) override {
        assert(false && "detached session reserved host I/O"); return {};
    }
    std::shared_ptr<Block512Backing::WriteBackOperation> submit(Block512Backing::PendingWriteBack,
        std::shared_ptr<Block512Backing::WriteBackOperation>) override {
        assert(false && "detached session submitted host I/O"); return {};
    }
};
Blob pack(const Block512Backing& backing) {
    Blob bytes; std::string error;
    assert(backing.appendSessionMedia(bytes, error)); return bytes;
}
void codec() {
    std::string error;
    const auto path = pom2test::tempPath("session-block-source.hdv");
    Blob original(512 * 4, 0); original[2 * 512] = 0x19;
    { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(original.data()), original.size()); }
    Block512Backing source;
    assert(source.loadImage(path)); source.setWriteBackEnabled(true); source.setHostWriteProtected(false);
    std::array<uint8_t, 512> block{}; block.fill(0x63);
    assert(source.writeBlock(3, block.data()));
    const auto epoch = pom2::mediaWriteEpoch().load();
    const auto bytes = pack(source);
    assert(bytes.size() == 14 + 2 * 517 && source.hasUnsavedChanges());
    assert(pom2::mediaWriteEpoch().load() == epoch);
    { std::ifstream f(path, std::ios::binary); Blob actual(original.size());
      f.read(reinterpret_cast<char*>(actual.data()), actual.size()); assert(actual == original); }
    auto pending = source.takeWriteBack(); assert(pending.valid);
    Blob ignored;
    assert(!source.appendSessionMedia(ignored, error));
    assert(!source.sessionReplaceable());
    pending = {};
    source.eject(); std::filesystem::remove(path);
    Block512Backing restored;
    assert(restored.loadSessionMedia(bytes.data(), bytes.size(), error));
    assert(restored.readBlock(3, block.data()) && block[511] == 0x63);
    assert(pack(restored) == bytes && restored.hasUnsavedChanges());
    assert(std::string(restored.persistenceState()) == "session");
    assert(restored.writeBlock(0, block.data()));
    restored.setWriteBackExecutor(std::make_shared<NoHostWrites>());
    restored.setWriteBackEnabled(true);
    assert(restored.saveDirty() && restored.hasUnsavedChanges());
    assert(!restored.takeWriteBack().valid && !restored.takeDetachWriteBack().valid);
    assert(!restored.pollWriteBack(true));
    assert(!std::filesystem::exists("(session disk)"));
    // Invalid geometry, order, flags, truncation: refusal is atomic.
    const auto baseline = pack(restored);
    for (int corruption = 0; corruption < 6; ++corruption) {
        auto bad = bytes;
        if (corruption == 0) bad[4] = 8;
        if (corruption == 1) bad[8] = 2; // >65536 blocks
        if (corruption == 2) bad[14 + 517] = 2; // duplicate block index
        if (corruption == 3) bad[18] = 2; // dirty flag
        if (corruption == 4) bad.pop_back();
        if (corruption == 5) bad.push_back(0);
        assert(!restored.loadSessionMedia(bad.data(), bad.size(), error));
        assert(pack(restored) == baseline);
    }
    // Header lock and host notch survive independently of write-back policy.
    for (uint8_t flags : {uint8_t(1), uint8_t(2), uint8_t(7)}) {
        auto locked = bytes; locked[4] = flags;
        assert(restored.loadSessionMedia(locked.data(), locked.size(), error));
        assert(restored.isWriteProtected() && !restored.writeBlock(0, block.data()));
        assert(pack(restored) == locked);
    }
    Block512Backing folder;
    assert(folder.loadFromBytes(Blob(512, 0), "folder", "/not-accessed"));
    Blob folderBytes;
    assert(folder.appendSessionMedia(folderBytes, error));
    Block512Backing detachedFolder;
    assert(detachedFolder.loadSessionMedia(folderBytes.data(), folderBytes.size(), error));
    assert(!detachedFolder.isSynthVolume() && detachedFolder.isLoaded());
    // A locked 2IMG becomes detached blocks, preserving its real header lock.
    Block512Backing container;
    Block512Backing::PreparedImage prepared;
    prepared.bytes.assign(64 + 512, 0); prepared.path = "unused.2mg"; prepared.valid = true;
    auto& image = prepared.bytes;
    image[0] = '2'; image[1] = 'I'; image[2] = 'M'; image[3] = 'G';
    image[8] = 64; image[12] = 1; image[19] = 0x80; image[20] = 1;
    image[24] = 64; image[29] = 2; image[64] = 0x45;
    assert(container.adoptImage(std::move(prepared)) && container.isMediumLocked());
    const auto lockedContainer = pack(container);
    Block512Backing decoded;
    assert(decoded.loadSessionMedia(lockedContainer.data(), lockedContainer.size(), error));
    assert(decoded.isMediumLocked() && decoded.readByte(0) == 0x45);
    Block512Backing dense;
    assert(dense.loadFromBytes(Blob(32u << 20, 0xA5), "dense", ""));
    Blob unchanged{0x19};
    assert(!dense.appendSessionMedia(unchanged, error) && unchanged == Blob{0x19});
}
void roundTrip() {
    EmulationController ctrl;
    ctrl.setMachineId(pom2::snapshotMachineId(pom2::SystemProfile::AppleIIe));
    auto state = ctrl.lockState();
    state.memory().setIIEMode(true);
    auto hdvOwner = std::make_unique<ProDOSHardDiskCard>(4);
    auto* hdv = hdvOwner.get();
    // Largest addressable HDV: sparse encoding must retain its final block.
    assert(hdv->loadDriveFromBytes(0, Blob(32u << 20, 0), "large.hdv"));
    assert(hdv->loadDriveFromBytes(1, Blob(4 * 512, 0), "second.hdv"));
    for (int i = 0; i < 2; ++i) { hdv->backing(i).setWriteBackEnabled(true); hdv->backing(i).setHostWriteProtected(false); }
    hdv->backing().writeByte((32u << 20) - 1, 0xB4);
    hdv->deviceSelectWrite(6, 0x80); hdv->deviceSelectWrite(0, 2);
    for (int i = 0; i < 37; ++i) hdv->deviceSelectWrite(2, 0xA6);
    state.memory().slotBus().plug(4, std::move(hdvOwner));
    auto portOwner = std::make_unique<pom2::SmartPortCard>(5);
    auto* port = portOwner.get();
    port->setUnitCount(3);
    auto unit = std::make_unique<pom2::SmartPortHdvUnit>();
    assert(unit->blockBacking()->loadFromBytes(Blob(4 * 512, 0), "smart.hdv", ""));
    unit->setWriteBackEnabled(true);
    port->setUnit(0, std::move(unit));
    port->setUnit(1, std::make_unique<pom2::SmartPort35Unit>()); // mixed chain
    port->setUnit(2, std::make_unique<pom2::SmartPortHdvUnit>()); // empty typed bay
    port->deviceSelectWrite(0, 0); port->deviceSelectWrite(1, 1);
    for (int i = 0; i < 125; ++i) port->deviceSelectWrite(3, 0xC3);
    state.memory().slotBus().plug(5, std::move(portOwner));
    std::string error;
    pom2::SessionArchive saved;
    assert(pom2::captureSession(saved, state.cpu(), state.memory(), ctrl.machineId(), error));
    assert(saved.version == 7);
    Blob encoded;
    assert(pom2::encodeSessionArchive(saved, encoded, error) && encoded.size() < (1u << 20));
    // Dirty host media cannot be silently replaced, even with write-back off.
    state.cpu().setProgramCounter(0x1234);
    assert(!pom2::restoreSession(saved, state.cpu(), state.memory(), ctrl.machineId(), error));
    assert(state.cpu().getProgramCounter() == 0x1234);
    for (int i = 0; i < 2; ++i) hdv->backing(i).eject();
    assert(pom2::restoreSession(saved, state.cpu(), state.memory(), ctrl.machineId(), error));
    assert(hdv->selectedDrive() == 1 && hdv->backing().blockCount() == 65536);
    assert(hdv->backing().readByte((32u << 20) - 1) == 0xB4);
    pom2::SessionArchive again;
    assert(pom2::captureSession(again, state.cpu(), state.memory(), ctrl.machineId(), error));
    Blob recaptured; assert(pom2::encodeSessionArchive(again, recaptured, error));
    assert(recaptured == encoded);
    for (int i = 37; i < 512; ++i) hdv->deviceSelectWrite(2, 0xD7);
    assert(hdv->backing(1).readByte(2 * 512 + 36) == 0xA6);
    assert(hdv->backing(1).readByte(2 * 512 + 37) == 0xD7);
    for (int i = 125; i < 512; ++i) port->deviceSelectWrite(3, 0xD7);
    std::array<uint8_t, 512> block{};
    assert(port->unit(0)->readBlock(1, block.data()));
    assert(block[0] == 0xC3 && block[124] == 0xC3 && block[125] == 0xD7 && block[511] == 0xD7);
    // Roll back dirty detached media repeatedly without involving host files.
    assert(pom2::restoreSession(saved, state.cpu(), state.memory(), ctrl.machineId(), error));
    assert(hdv->backing(1).readByte(2 * 512 + 37) == 0);
    auto legacy = saved; legacy.version = 5;
    assert(!pom2::restoreSession(legacy, state.cpu(), state.memory(), ctrl.machineId(), error));
    assert(!std::filesystem::exists("large.hdv") && !std::filesystem::exists("smart.hdv"));
}
}
int main() { codec(); roundTrip(); std::puts("block session media: OK"); }
