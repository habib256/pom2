// POM2 — GPL-3.0-or-later
#include "BrowserSessionStore.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "IIcMouse.h"
#include "LeChatMauveCard.h"
#include "Mockingboard.h"
#include "SessionSnapshot.h"
#include "SessionFiles.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SuperSerialCard.h"
#include "SystemProfile.h"
#include "TestTempPath.h"
#include "Woz35Fixture.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
using Blob = std::vector<uint8_t>;
struct Fixture {
    EmulationController ctrl;
    DiskIICard* disk;
    pom2::SmartPortCard* port;
    IIcMouse* mouse;
    SuperSerialCard* serial;
    std::string error;
    std::string floppy = pom2test::tempPath("pom2-session-iic.dsk");
    std::string volume = pom2test::tempPath("pom2-session-iic.po");
    std::string session = pom2test::tempPath("pom2-session-iic.pom2session");
    explicit Fixture(pom2::SystemProfile profile, bool writable = false) {
        const bool plus = profile == pom2::SystemProfile::AppleIIcPlus;
        ctrl.setMachineId(pom2::snapshotMachineId(profile));
        Blob source(DiskImage::kBytesPerImage, 0xAA);
        { std::ofstream f(floppy, std::ios::binary); f.write(reinterpret_cast<const char*>(source.data()), source.size()); }
        source.resize(pom2::Disk35Image::kBytesPerImage);
        for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i * 17 + i / 512);
        { std::ofstream f(volume, std::ios::binary); f.write(reinterpret_cast<const char*>(source.data()), source.size()); }
        auto state = ctrl.lockState();
        auto& mem = state.memory();
        mem.setIIEMode(true);
        mem.setVideoStandard(pom2::profileConfig(profile).videoStandard);
        state.cpu().setCpuMode(M6502::CpuMode::CMOS);
        Memory::AppleIIRomImage rom;
        rom.path = "synthetic //c session firmware"; rom.bytes.assign(0x8000, 0xEA);
        rom.bytes[0x3BC0] = 0; rom.bytes[0x3BBF] = plus ? 5 : 3;
        assert(mem.loadAppleIIRom(rom, true) && mem.isIIcClass());
        for (int slot : {1, 2}) {
            auto card = std::make_unique<SuperSerialCard>(slot);
            card->setBuiltInPort(true);
            if (slot == 1) serial = card.get();
            mem.slotBus().plug(slot, std::move(card));
        }
        mem.slotBus().plug(3, std::make_unique<MockingboardCard>(3));
        auto nativeMouse = std::make_unique<IIcMouse>(plus ? 7 : 4);
        mouse = nativeMouse.get(); mem.slotBus().plug(4, std::move(nativeMouse));
        auto smart = std::make_unique<pom2::SmartPortCard>(5);
        port = smart.get(); smart->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
        auto* unit = static_cast<pom2::SmartPort35Unit*>(smart->unit(0));
        assert(unit->loadImage(volume)); unit->setHostWriteProtected(!writable); unit->setWriteBackEnabled(true);
        mem.slotBus().plug(5, std::move(smart));
        auto controller = std::make_unique<DiskIICard>(6);
        disk = controller.get(); assert(disk->insertDisk(floppy));
        disk->driveImage(0).setHostWriteProtected(!writable); disk->setWriteBackEnabled(true);
        disk->setIWM(&ctrl.iwm()); mem.slotBus().plug(6, std::move(controller));
        mem.slotBus().plug(7, std::make_unique<LeChatMauveCard>(7, LeChatMauveCard::Variant::IIcAdapter));
        assert(ctrl.disk35(0).loadFile(volume)); ctrl.disk35(0).setHostWriteProtected(!writable); ctrl.disk35(0).setWriteBackEnabled(true);
        ctrl.sony35(0).notifyMediaChange();
        mem.setCycleCounter(5000); state.cpu().setProgramCounter(0x2000);
        mem.writeRamUnchecked(0x2000, 0xE8); // INX; JMP $2000
        mem.writeRamUnchecked(0x2001, 0x4C); mem.writeRamUnchecked(0x2002, 0); mem.writeRamUnchecked(0x2003, 0x20);
        mem.memWrite(0xC400, 0x37); // wake Mockingboard 4c
        serial->deviceSelectWrite(0xA, 0x09); serial->deviceSelectWrite(0xB, 0x1E);
        mouse->setHostMouse(15, 30, false); mouse->advanceCycles(73);
        port->deviceSelectWrite(0, 0); port->deviceSelectWrite(1, 2); port->deviceSelectWrite(2, 0);
        for (int i = 0; i < 37; ++i) (void)port->deviceSelectRead(3);
        if (plus) {
            (void)mem.memRead(0xC028); // alternate ROM/MIG window
            mem.memWrite(0xCCA0, 0); mem.memWrite(0xCC00, 0x6B);
        }
    }
    ~Fixture() {
        std::error_code ec;
        for (const auto& path : {floppy, volume, session}) std::filesystem::remove(path, ec);
    }
    pom2::SessionArchive capture() {
        pom2::SessionArchive result;
        auto state = ctrl.lockState();
        if (!pom2::captureSession(result, state.cpu(), state.memory(), ctrl.machineId(), error)) {
            std::fprintf(stderr, "//c capture: %s\n", error.c_str()); assert(false);
        }
        return result;
    }
    Blob encoded() {
        Blob bytes; assert(pom2::encodeSessionArchive(capture(), bytes, error)); return bytes;
    }
};
void roundtrip(pom2::SystemProfile profile) {
    auto f = std::make_unique<Fixture>(profile);
    const auto before = f->encoded();
    assert(pom2::saveSessionFile(f->ctrl, f->session, f->error));
    {
        auto state = f->ctrl.lockState(); state.cpu().setProgramCounter(0x3333);
        state.memory().writeRamUnchecked(0x2000, 0xEA);
        f->serial->onReset(); f->mouse->onReset(); f->port->onReset();
        assert(f->disk->ejectDisk()); assert(f->port->unit(0)->eject()); f->ctrl.disk35(0).eject();
    }
    std::filesystem::remove(f->floppy); std::filesystem::remove(f->volume);
    if (!pom2::loadSessionFile(f->ctrl, f->session, f->error)) {
        std::fprintf(stderr, "//c restore: %s\n", f->error.c_str()); assert(false);
    }
    assert(f->encoded() == before);
    uint8_t block[512];
    assert(f->port->unit(0)->readBlock(2, block));
    assert(f->port->deviceSelectRead(3) == block[37]); // partial read continued exactly
    assert(f->ctrl.disk35(0).readBlock(2, block));
    assert(f->port->unit(0)->isFileWriteProtected() && f->ctrl.disk35(0).isFileWriteProtected());
    auto good = f->capture();
    const auto unchanged = f->encoded();
    for (int slot : {0, 1, 4, 5, 7}) {
        auto bad = good; bad.slots[size_t(slot)].state.pop_back();
        auto state = f->ctrl.lockState();
        assert(!pom2::restoreSession(bad, state.cpu(), state.memory(), f->ctrl.machineId(), f->error));
    }
    assert(f->encoded() == unchanged);
    { auto state = f->ctrl.lockState(); state.cpu().run(20); }

    // The browser checkpoint adapter must accept the complete //c topology.
    const auto root = pom2test::tempPath("pom2-iic-browser-resume");
    std::filesystem::create_directories(root);
    pom2::BrowserSessionStore store(root);
    assert(store.setEnabled(f->ctrl, true));
    const auto checkpoint = f->encoded();
    { auto state = f->ctrl.lockState(); state.cpu().setProgramCounter(0x4321); }
    pom2::BrowserSessionStore reloaded(root); reloaded.initialize(f->ctrl);
    assert(!reloaded.suspended() && reloaded.enabled());
    assert(f->encoded() == checkpoint);
    std::filesystem::remove_all(root);
}
void writableRoundtrip() {
    auto f = std::make_unique<Fixture>(pom2::SystemProfile::AppleIIcPlus, true);
    uint8_t data[512]; std::memset(data, 0x6D, 512);
    {
        auto state = f->ctrl.lockState();
        f->disk->driveImage(0).writeNibbleAt(0, 23, 0xD5);
        assert(f->port->unit(0)->writeBlock(17, data));
        assert(f->ctrl.disk35(0).writeBlock(17, data));
    }
    f->ctrl.sony35(0).seekPhaseW(2); f->ctrl.sony35(0).seekPhaseW(10); f->ctrl.sony35(0).seekPhaseW(2);
    f->ctrl.sony35(0).writeStart();
    const int64_t ticks[] = {0, 14, 42};
    f->ctrl.sony35(0).writeFlux(0, 64, ticks, 3, 0, 14);
    const auto cells = f->ctrl.sony35(0).debugCellStream();
    const auto saved = f->capture();
    {
        auto state = f->ctrl.lockState();
        f->disk->driveImage(0).eject();
        static_cast<pom2::SmartPort35Unit*>(f->port->unit(0))->image().eject();
        f->ctrl.disk35(0).eject();
        assert(pom2::restoreSession(saved, state.cpu(), state.memory(), f->ctrl.machineId(), f->error));
    }
    assert(f->ctrl.sony35(0).debugCellStream() == cells);
    uint8_t got[512];
    assert(!f->disk->driveImage(0).isWriteProtected());
    assert(f->disk->driveImage(0).nibbleAt(0, 23) == 0xD5);
    assert(!f->port->unit(0)->isWriteProtected() && f->port->unit(0)->readBlock(17, got));
    assert(!std::memcmp(data, got, 512));
    assert(!f->ctrl.disk35(0).isWriteProtected() && f->ctrl.disk35(0).readBlock(17, got));
    assert(!std::memcmp(data, got, 512));
    auto state = f->ctrl.lockState();
    f->disk->driveImage(0).writeNibbleAt(0, 23, 0xAB);
    std::memset(data, 0x91, 512);
    assert(f->port->unit(0)->writeBlock(17, data) && f->ctrl.disk35(0).writeBlock(17, data));
    pom2::SessionArchive changed, again;
    assert(pom2::captureSession(changed, state.cpu(), state.memory(), f->ctrl.machineId(), f->error));
    assert(pom2::restoreSession(saved, state.cpu(), state.memory(), f->ctrl.machineId(), f->error));
    assert(pom2::restoreSession(changed, state.cpu(), state.memory(), f->ctrl.machineId(), f->error));
    assert(pom2::captureSession(again, state.cpu(), state.memory(), f->ctrl.machineId(), f->error));
    Blob a, b; assert(pom2::encodeSessionArchive(changed, a, f->error));
    assert(pom2::encodeSessionArchive(again, b, f->error) && a == b);
}
void captureMedia() {
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<uint8_t> data(12 * 512, 0x5A);
    tracks[0] = pom2::test::encodeTrack(0, 0, data.data());
    const auto path = pom2::test::writeTemp("pom2-session-35.woz", pom2::test::buildWoz(tracks));
    pom2::Disk35Image image, restored;
    assert(image.loadFile(path.string())); image.setHostWriteProtected(true);
    Blob media; std::string error;
    assert(image.appendReadOnlySession(media, error)); std::filesystem::remove(path);
    assert(restored.loadReadOnlySession(media.data(), media.size(), error));
    assert(*restored.originalBitCells(0, 0) == tracks[0]);
    uint8_t block[512]; assert(restored.readBlock(0, block));
    for (auto byte : block) assert(byte == 0x5A);
    restored.setHostWriteProtected(false); restored.setWriteBackEnabled(true);
    assert(restored.isFileWriteProtected() && !restored.writeBlock(0, block));
}
}
int main() {
    for (auto profile : {pom2::SystemProfile::AppleIIc, pom2::SystemProfile::AppleIIcPAL, pom2::SystemProfile::AppleIIcPlus}) roundtrip(profile);
    writableRoundtrip();
    captureMedia();
    std::puts("//c sessions: all passed");
}
