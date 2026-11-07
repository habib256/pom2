// POM2 — GPL-3.0-or-later
#include "EmulationController.h"
#include "PremiumSoftCard.h"
#include "SessionMachine.h"
#include "SessionSettings.h"
#include "Settings.h"
#include "SuperSerialCard.h"
#include "SessionSnapshot.h"
#include "SessionLoadQueue.h"
#include "SessionFiles.h"
#include "ByteIO.h"
#include "CffaCard.h"
#include "TranswarpCard.h"
#include "PhasorCard.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "SamCard.h"
#include "FourPlayCard.h"
#include "ProDOSHardDiskCard.h"
#include <array>
#include "DiskIICard.h"
#include "Mockingboard.h"
#include "SlinkyCard.h"
#include "SnesMaxCard.h"
#include "TestTempPath.h"
#include <cassert>
#include <future>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <fstream>
#include <filesystem>

using namespace pom2;
namespace {
SessionArchive capture(EmulationController& c) {
    SessionArchive s; std::string e; auto state = c.lockState();
    assert(captureSession(s, state.cpu(), state.memory(), c.machineId(), e)); return s;
}
void requireLoad(EmulationController& c, const SessionArchive& s, const SessionMachineHooks& h = {}) {
    std::string e;
    if (!reconstructSession(c, s, e, h)) { std::fprintf(stderr, "%s\n", e.c_str()); assert(false); }
}
}
namespace {
void profiles()
{
    auto target = std::make_unique<EmulationController>();
    for (auto profile : allProfiles()) {
        auto source = std::make_unique<EmulationController>();
        const auto& cfg = profileConfig(profile);
        source->setMachineId(snapshotMachineId(profile));
        source->setVideoStandard(cfg.videoStandard);
        source->cpu().setCpuMode(cfg.defaultCpu);
        source->memory().setIIEMode(cfg.iieMode);
        Memory::AppleIIRomImage rom;
        rom.bytes.assign(cfg.noPhysicalSlots ? 0x8000 : 0x4000, 0xEA);
        if (cfg.noPhysicalSlots) { rom.bytes[0x3BC0] = 0; rom.bytes[0x3BBF] = profile == SystemProfile::AppleIIcPlus ? 5 : 3; }
        assert(source->memory().loadAppleIIRom(rom, cfg.noPhysicalSlots));
        {
            // As the profile switch does: a //c Plus seats its accelerator.
            std::lock_guard<std::mutex> lk(source->stateMutex());
            source->installSocketAccelerator();
        }
        source->cpu().setProgramCounter(0x4567);
        source->memory().writeRamUnchecked(0x1234, uint8_t(profile));
        source->memory().setJoyportMode(SiriusJoyport::Right);
        source->memory().setJoyportContacts(1, SiriusJoyport::Fire);
        const auto archive = capture(*source);
        requireLoad(*target, archive);
        assert(target->machineId() == archive.machineId);
        assert(target->cpu().getCpuMode() == cfg.defaultCpu);
        assert(target->memory().isIIcClass() == cfg.noPhysicalSlots);
        assert(target->memory().joyportMode() == SiriusJoyport::Right);
        assert(target->memory().joyportActive() == !cfg.noPhysicalSlots);
        if (!cfg.noPhysicalSlots) assert(!(target->memory().memRead(0xC061) & 0x80));
        assert(capture(*target).core == archive.core);
        if (profile == SystemProfile::AppleIIcPlus) {
            // Written before the //c Plus accelerator was modelled: the v1
            // configuration, no accelerator bytes (bug hunt 2026-10-23).
            auto old = archive; old.configuration.resize(4); old.configuration[0] = 1;
            for (uint32_t version : {6u, 7u}) { old.version = version; requireLoad(*target, old); }
            assert(capture(*target).core == archive.core);
        }
        source->memory().setGamePortConnected(false);
        requireLoad(*target, capture(*source));
        assert(target->memory().joyportMode() == SiriusJoyport::Right);
        assert(!target->memory().gamePortConnected() && !target->memory().joyportActive());
    }
}
void legacyBundles()
{
    auto source = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    source->setMachineId(snapshotMachineId(SystemProfile::AppleIIPlus));
    auto archive = capture(*source);
    for (uint32_t version = 1; version <= 6; ++version) {
        archive.version = version;
        std::vector<uint8_t> bytes; SessionArchive decoded; std::string error;
        assert(encodeSessionArchive(archive, bytes, error) && decodeSessionArchive(bytes, decoded, error));
        auto target = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
        requireLoad(*target, decoded);
        assert(target->machineId() == source->machineId() && capture(*target).core == archive.core);
    }
}

void zipAndExpansions()
{
    auto source = std::make_unique<EmulationController>();
    source->setMachineId(snapshotMachineId(SystemProfile::AppleIIe)); source->memory().setIIEMode(true);
    source->setZipChipInstalled(true); source->zipChip().setBootFast(false);
    for (int i = 0; i < 4; ++i) source->memory().memWrite(0xC05A, 0x5A);
    source->memory().memWrite(0xC05C, 0x28);
    source->memory().slotBus().plug(1, std::make_unique<PhasorCard>(1));
    source->memory().slotBus().plug(2, std::make_unique<EchoPlusCard>(2));
    source->memory().slotBus().plug(3, std::make_unique<SamCard>(3));
    source->memory().slotBus().plug(4, std::make_unique<FourPlayCard>(4));
    auto tms = std::make_unique<EchoPlusTMS5220Card>(5);
    tms->setSampleRate(source->audio().getActualSampleRate());
    source->memory().slotBus().plug(5, std::move(tms));
    const auto session = capture(*source);
    auto target = std::make_unique<EmulationController>(); requireLoad(*target, session);
    assert(target->zipChipInstalled()); assert(capture(*target).core == session.core);
    for (int i = 1; i <= 5; ++i) {
        assert(target->memory().slotBus().peripheral(i));
        assert(capture(*target).slots[i].state == session.slots[i].state);
    }
    // Reconstructing a stock machine removes the socket accelerator as well.
    source->setZipChipInstalled(false); const auto stock = capture(*source);
    requireLoad(*target, stock); assert(!target->zipChipInstalled());
    // A malformed timer must fail before the current topology is detached.
    auto bad = session; bad.slots[1].state[18] = 0x7F;
    std::string error; assert(!reconstructSession(*target, bad, error));
    assert(!target->zipChipInstalled());
}
void transwarp()
{
    auto source = std::make_unique<EmulationController>();
    source->setMachineId(snapshotMachineId(SystemProfile::AppleIIPlus));
    source->cpu().setCpuMode(M6502::CpuMode::NMOS);
    Memory::AppleIIRomImage rom; rom.bytes.assign(0x3000, 0xEA); assert(source->memory().loadAppleIIRom(rom));
    auto card = std::make_unique<TranswarpCard>(3); auto* tw = card.get();
    assert(card->setRom(std::vector<uint8_t>(4096, 0xAB))); card->setMemory(&source->memory());
    source->memory().slotBus().plug(3, std::move(card));
    assert(tw->shadowActive() && source->cpu().effectiveCpuMode() == M6502::CpuMode::CMOS);
    const auto session = capture(*source);
    auto target = std::make_unique<EmulationController>(); requireLoad(*target, session);
    assert(target->cpu().effectiveCpuMode() == M6502::CpuMode::CMOS);
    assert(target->memory().memRead(0xF800) == 0xAB);
    auto* restored = dynamic_cast<TranswarpCard*>(target->memory().slotBus().peripheral(3)); assert(restored);
    assert(capture(*target).slots[3].state == session.slots[3].state);
    target->memory().memWrite(0xC072, 0);
    assert(!restored->shadowActive() && target->memory().memRead(0xF800) == 0xEA);
}
void cffaAndFolder()
{
    auto source = std::make_unique<EmulationController>();
    source->setMachineId(snapshotMachineId(SystemProfile::AppleIIPlus));
    auto card = std::make_unique<CffaCard>(7); auto* cffa = card.get();
    std::vector<uint8_t> rom(4097, 0xEA); rom[0] = 1; assert(card->installSessionIdentity(rom));
    std::vector<uint8_t> disk(8 * 512); for (size_t i = 0; i < disk.size(); ++i) disk[i] = uint8_t(i * 7);
    assert(cffa->blockBacking()->loadFromBytes(disk, "private CFFA test", ""));
    cffa->setWriteBackEnabled(true);
    source->memory().slotBus().plug(7, std::move(card));
    auto lba = [&] (uint8_t block) {
        cffa->deviceSelectWrite(0xA, 1); cffa->deviceSelectWrite(0xB, block);
        cffa->deviceSelectWrite(0xC, 0); cffa->deviceSelectWrite(0xD, 0); cffa->deviceSelectWrite(0xE, 0xE0);
    };
    lba(2); cffa->deviceSelectWrite(0xF, AtaBlockDevice::kCmdRead);
    for (int i = 0; i < 23; ++i) { cffa->deviceSelectRead(8); cffa->deviceSelectRead(0); }
    auto target = std::make_unique<EmulationController>(); requireLoad(*target, capture(*source));
    auto* restored = dynamic_cast<CffaCard*>(target->memory().slotBus().peripheral(7)); assert(restored);
    for (int i = 23; i < 256; ++i) {
        assert(restored->deviceSelectRead(8) == cffa->deviceSelectRead(8));
        assert(restored->deviceSelectRead(0) == cffa->deviceSelectRead(0));
    }
    // IDENTIFY must finish without advancing the ATA taskfile.
    lba(4); cffa->deviceSelectWrite(0xF, AtaBlockDevice::kCmdIdentify);
    for (int i = 0; i < 11; ++i) { cffa->deviceSelectRead(8); cffa->deviceSelectRead(0); }
    requireLoad(*target, capture(*source)); restored = dynamic_cast<CffaCard*>(target->memory().slotBus().peripheral(7));
    for (int i = 11; i < 256; ++i) {
        assert(restored->deviceSelectRead(8) == cffa->deviceSelectRead(8));
        assert(restored->deviceSelectRead(0) == cffa->deviceSelectRead(0));
    }
    assert(restored->deviceSelectRead(0xB) == 4 && cffa->deviceSelectRead(0xB) == 4);
    // Continue a split WRITE after the source machine has gone away.
    lba(3); cffa->deviceSelectWrite(0xF, AtaBlockDevice::kCmdWrite);
    for (int i = 0; i < 37; ++i) { cffa->deviceSelectWrite(0, 0x42); cffa->deviceSelectWrite(8, uint8_t(i)); }
    auto partial = capture(*source); cffa->setWriteBackEnabled(false); source.reset();
    requireLoad(*target, partial); restored = dynamic_cast<CffaCard*>(target->memory().slotBus().peripheral(7));
    for (int i = 37; i < 256; ++i) { restored->deviceSelectWrite(0, 0x42); restored->deviceSelectWrite(8, uint8_t(i)); }
    std::array<uint8_t, 512> bytes{}; assert(restored->blockBacking()->readBlock(3, bytes.data()));
    for (int i = 0; i < 256; ++i) { assert(bytes[i * 2] == uint8_t(i)); assert(bytes[i * 2 + 1] == 0x42); }
    assert(std::string(restored->blockBacking()->persistenceState()) == "session");
    // A synthesized host-folder volume is embedded as detached blocks.
    auto folderSource = std::make_unique<EmulationController>();
    folderSource->setMachineId(snapshotMachineId(SystemProfile::AppleIIPlus));
    auto hdv = std::make_unique<ProDOSHardDiskCard>(5);
    assert(hdv->backing(0).loadFromBytes(disk, "folder volume", "/nonexistent/session-host-folder"));
    folderSource->memory().slotBus().plug(5, std::move(hdv));
    const auto folderSession = capture(*folderSource); folderSource.reset();
    requireLoad(*target, folderSession);
    auto* volume = dynamic_cast<ProDOSHardDiskCard*>(target->memory().slotBus().peripheral(5)); assert(volume);
    assert(!volume->backing(0).isSynthVolume()); assert(volume->backing(0).readBlock(1, bytes.data()));
    assert(std::equal(bytes.begin(), bytes.end(), disk.begin() + 512));
}
void outgoingDurability()
{
    auto saved = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    saved->setMachineId(snapshotMachineId(SystemProfile::AppleIIPlus));
    const auto archive = capture(*saved);
    auto live = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    live->setMachineId(snapshotMachineId(SystemProfile::AppleIIe)); live->memory().setIIEMode(true);
    auto hdv = std::make_unique<ProDOSHardDiskCard>(5); auto* card = hdv.get();
    const auto path = pom2test::tempPath("session-outgoing-dirty.hdv");
    { std::ofstream f(path, std::ios::binary); std::vector<uint8_t> bytes(1024, 0x5A);
      f.write(reinterpret_cast<char*>(bytes.data()), bytes.size()); }
    assert(card->backing(0).loadImage(path));
    card->backing(0).setWriteBackEnabled(true);
    live->memory().slotBus().plug(5, std::move(hdv));
    std::array<uint8_t, 512> dirty{}; dirty.fill(0xA6); assert(card->backing(0).writeBlock(1, dirty.data()));
    int detached = 0; SessionMachineHooks hooks;
    hooks.detach = [&](const StateAccess&) { ++detached; };
    std::string error; assert(!reconstructSession(*live, archive, error, hooks));
    assert(!detached && live->memory().slotBus().peripheral(5) == card);
    assert(card->backing(0).readBlock(1, dirty.data()) && dirty[511] == 0xA6);
    // Captured host writes remain outstanding even after dirty flags retire.
    auto pending = card->backing(0).takeWriteBack(); assert(pending.valid);
    assert(!reconstructSession(*live, archive, error, hooks)); assert(!detached);
    pending = {}; card->backing(0).setWriteBackEnabled(false); std::filesystem::remove(path);
}

void tapes()
{
    const auto tapePath = pom2test::tempPath("session-program-tape.aci");
    std::vector<uint8_t> tape{'P','O','M','1','A','C','I','1',1,0,0,0};
    byteio::putU32(tape, 100); for (int i = 0; i < 100; ++i) byteio::putU32(tape, 100 + i);
    { std::ofstream f(tapePath, std::ios::binary); f.write(reinterpret_cast<char*>(tape.data()), tape.size()); }
    auto source = std::make_unique<EmulationController>(); source->setMachineId(snapshotMachineId(SystemProfile::AppleIIPlus));
    assert(source->cassette().loadTape(tapePath)); source->cassette().playTape();
    source->cassette().advanceCycles(357); source->cassette().armRecording();
    source->cassette().toggleOutput(); source->cassette().advanceCycles(91); source->cassette().toggleOutput();
    auto session = capture(*source); std::string error;
    assert(source->cassette().appendSessionState(session.cassette, error));
    const auto bundle = pom2test::tempPath("session-machine-tape.pom2session");
    assert(saveSessionFile(*source, bundle, error));
    std::filesystem::remove(tapePath);
    auto fileTarget = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    assert(loadSessionFile(*fileTarget, bundle, error)); std::filesystem::remove(bundle);
    assert(fileTarget->cassette().hasLoadedTape() && fileTarget->cassette().isRecordingArmed());
    auto target = std::make_unique<EmulationController>(); requireLoad(*target, session);
    assert(target->cassette().hasLoadedTape() && target->cassette().isRecordingArmed());
    std::vector<uint8_t> recaptured; assert(target->cassette().appendSessionState(recaptured, error));
    assert(recaptured == session.cassette);
    for (int i = 0; i < 100; ++i) {
        source->cassette().advanceCycles(13); target->cassette().advanceCycles(13);
        assert(source->cassette().readTapeInput() == target->cassette().readTapeInput());
        assert(source->cassette().toggleOutput() == target->cassette().toggleOutput());
    }
    std::vector<uint8_t> expected, actual;
    assert(source->cassette().appendSessionState(expected, error)); assert(target->cassette().appendSessionState(actual, error));
    assert(actual == expected);
    for (size_t cut = 1; cut < session.cassette.size(); ++cut) {
        auto truncated = session.cassette; truncated.resize(cut);
        assert(!target->cassette().loadSessionState(truncated, error, false));
    }
    auto bad = session; bad.cassette.pop_back();
    const auto before = capture(*target).core;
    assert(!reconstructSession(*target, bad, error)); assert(capture(*target).core == before);
    // Host recordings cannot be discarded by opening another machine.
    assert(!reconstructSession(*source, session, error));
    // Detached recordings may be rolled back to an explicit checkpoint.
    requireLoad(*target, session);
}

// REW pressed mid-segment: the forward countdown stayed set while the head
// moved back, so a session saved during the rewind failed its own reader's
// check and the whole session was refused (bug hunt 2026-10-23).
void tapeRewindSession()
{
    const auto tapePath = pom2test::tempPath("session-rewind-tape.aci");
    std::vector<uint8_t> tape{'P','O','M','1','A','C','I','1',1,0,0,0};
    byteio::putU32(tape, 40);
    for (int i = 0; i < 40; ++i) byteio::putU32(tape, (i & 1) ? 600 : 200);
    { std::ofstream f(tapePath, std::ios::binary); f.write(reinterpret_cast<char*>(tape.data()), tape.size()); }
    auto c = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    c->setMachineId(snapshotMachineId(SystemProfile::AppleIIPlus));
    assert(c->cassette().loadTape(tapePath)); c->cassette().playTape();
    for (int i = 0; i < 30; ++i) {             // the program reads $C060: the
        c->cassette().advanceCycles(10);       // tape runs, into the 600-cycle
        (void)c->cassette().readTapeInput();   // segment after the first 200
    }
    c->cassette().rewindTape();
    c->cassette().advanceCycles(31);           // 620 rewind cycles: one segment back
    std::vector<uint8_t> state; std::string error;
    assert(c->cassette().appendSessionState(state, error));
    if (!c->cassette().loadSessionState(state, error, false))
        std::fprintf(stderr, "rewind session: %s\n", error.c_str());
    assert(c->cassette().loadSessionState(state, error, false) &&
           "a session saved mid-rewind was refused");
    std::filesystem::remove(tapePath);
}
}

// A session load must REFUSE a live Premium SoftCard, not destroy it.
//
// `SlotBus::clear()` empties slot 0 — the //e auxiliary slot — along with the
// rest, and `restoreSession`'s own slot-0 guard runs AFTER that clear. So it
// found an empty slot, passed, and the card (its Z80, its $C07E register, its
// live CP/M) was gone with the UI reporting "Session loaded". The outgoing
// check now refuses first, which is what docs/session-format.md promises:
// "No unsupported device is silently omitted", and the refusal names it.
void premiumSoftCardSurvivesARefusedLoad()
{
    auto source = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    source->setMachineId(snapshotMachineId(SystemProfile::AppleIIePAL));
    source->setVideoStandard(VideoStandard::PAL);
    source->memory().setIIEMode(true);
    const SessionArchive session = capture(*source);

    auto target = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    target->setMachineId(snapshotMachineId(SystemProfile::AppleIIePAL));
    target->setVideoStandard(VideoStandard::PAL);
    target->memory().setIIEMode(true);
    target->memory().slotBus().plug(
        PremiumSoftCard::kAuxSlot,
        std::make_unique<PremiumSoftCard>());
    assert(target->memory().slotBus().peripheral(PremiumSoftCard::kAuxSlot));

    std::string error;
    assert(!reconstructSession(*target, session, error));
    // The refusal NAMES the card — not "slot 0", which no Slot Config row
    // shows and which would send the user looking for a slot the //e has not
    // got.
    assert(error.find("Premium SoftCard") != std::string::npos);
    assert(error.find("Machine menu") != std::string::npos);
    // And the card is still there: a refused load leaves the machine intact.
    assert(target->memory().slotBus().peripheral(PremiumSoftCard::kAuxSlot));
    std::printf("  ok: a session load refuses a Premium SoftCard and keeps it\n");
}

// Quitting with a loaded session's machine must not write its serial ports
// and No-Slot Clock over the user's own settings: the session's //c ports
// have no listener and the default port, its clock may be off, and the next
// normal launch came back with the user's telnet listener and clock gone.
void sessionMachineKeepsUserSerialAndClockSettings()
{
    auto c = std::make_unique<EmulationController>(AudioDevice::OutputMode::Offline);
    c->memory().slotBus().plug(2, std::make_unique<SuperSerialCard>(2));
    c->noSlotClock().setEnabled(false);
    c->noSlotClock().setSlot(-1);
    Settings settings;
    settings.setBool("ssc_listening_slot2", true);
    settings.setInt ("ssc_port_slot2", 6800);
    settings.setBool("ssc_listening", true);
    settings.setInt ("ssc_port", 6800);
    settings.setBool("nsclock_enable", true);
    settings.setInt ("nsclock_slot", 3);
    persistSerialAndClockSettings(settings, c->memory().slotBus(), c->noSlotClock(), true);
    assert(settings.getBool("ssc_listening_slot2") && settings.getInt("ssc_port_slot2") == 6800);
    assert(settings.getBool("ssc_listening") && settings.getInt("ssc_port") == 6800);
    assert(settings.getBool("nsclock_enable") && settings.getInt("nsclock_slot") == 3);
    assert(!settings.has("ssc_cable_slot2") && !settings.has("ssc_raw_mode_slot2"));
    // The user's own machine still persists them.
    persistSerialAndClockSettings(settings, c->memory().slotBus(), c->noSlotClock(), false);
    assert(!settings.getBool("ssc_listening_slot2", true));
    assert(settings.getInt("ssc_port_slot2") != 6800 && settings.has("ssc_cable_slot2"));
    assert(!settings.getBool("nsclock_enable", true) && settings.getInt("nsclock_slot") == -1);
    std::printf("  ok: a session's machine leaves the serial and clock settings alone\n");
}

int main()
{
    sessionMachineKeepsUserSerialAndClockSettings();
    profiles(); legacyBundles(); zipAndExpansions(); transwarp(); cffaAndFolder(); outgoingDurability(); tapes();
    tapeRewindSession();
    premiumSoftCardSurvivesARefusedLoad();
    auto source = std::make_unique<EmulationController>();
    source->setMachineId(snapshotMachineId(SystemProfile::AppleIIePAL));
    source->setVideoStandard(VideoStandard::PAL);
    source->memory().setIIEMode(true); source->memory().setRamWorksBanks(4);
    source->cpu().setCpuMode(M6502::CpuMode::CMOS);
    source->memory().writeRamUnchecked(0x2345, 0xA6); source->cpu().setProgramCounter(0x2345);
    source->memory().slotBus().plug(2, std::make_unique<MockingboardCard>(2));
    auto disk = std::make_unique<DiskIICard>(7); disk->setIwmHost(false);
    source->memory().slotBus().plug(7, std::move(disk));
    const auto session = capture(*source);
    auto target = std::make_unique<EmulationController>();
    target->setMachineId(snapshotMachineId(SystemProfile::AppleIIPlus));
    target->memory().slotBus().plug(4, std::make_unique<SnesMaxCard>(4));
    target->debugger().setWatchpoint(0x2345, Debugger::Read);
    target->debugger().setWatchpoint(0xE000, Debugger::Write); target->syncDebugHook();
    int detached = 0, published = 0;
    SessionMachineHooks hooks;
    hooks.detach = [&](const StateAccess& s) { ++detached; assert(s.memory().slotBus().peripheral(4)); };
    hooks.publish = [&](const StateAccess& s, SystemProfile p) {
        ++published; assert(p == SystemProfile::AppleIIePAL); assert(s.memory().slotBus().peripheral(7));
    };
    requireLoad(*target, session, hooks);
    assert(detached == 1 && published == 1);
    assert(target->memory().hasReadWatch(0x2345) && target->memory().hasWriteWatch(0xE000));
    assert(target->memory().ramWritable(0xE000) == source->memory().ramWritable(0xE000));
    assert(target->machineId() == source->machineId());
    assert(target->cpu().getProgramCounter() == 0x2345);
    assert(target->memory().memRead(0x2345) == 0xA6);
    assert(!target->memory().slotBus().peripheral(4));
    assert(dynamic_cast<MockingboardCard*>(target->memory().slotBus().peripheral(2)));
    assert(target->memory().videoStandard() == VideoStandard::PAL);
    assert(capture(*target).core == session.core);
    // Broken firmware/card/core payloads never detach the current machine.
    auto oldCard = target->memory().slotBus().peripheral(7);
    for (int broken = 0; broken < 3; ++broken) {
        auto bad = session;
        if (broken == 0) bad.slots[7].identity.pop_back();
        if (broken == 1) bad.core.resize(100);
        if (broken == 2) bad.machineId = 0x12345678;
        std::string e; assert(!reconstructSession(*target, bad, e, hooks)); assert(!e.empty());
        assert(target->memory().slotBus().peripheral(7) == oldCard);
        assert(capture(*target).core == session.core); assert(detached == 1);
    }
    // A session that puts an NMOS core under a soldered 65C02 (enhanced //e
    // PAL here) is refused, not built: the ROM would hit KIL opcodes.
    {
        source->cpu().setCpuMode(M6502::CpuMode::NMOS);
        const auto nmos = capture(*source);
        source->cpu().setCpuMode(M6502::CpuMode::CMOS);
        std::string e; assert(!reconstructSession(*target, nmos, e, hooks)); assert(!e.empty());
        assert(target->cpu().getCpuMode() == M6502::CpuMode::CMOS && detached == 1);
    }
    // A UI queue runs background loads on the owner and closes pending waits.
    const auto owner = std::this_thread::get_id();
    auto queue = std::make_shared<SessionLoadQueue>([&](const SessionArchive&, std::string&) {
        assert(std::this_thread::get_id() == owner); return true;
    });
    auto task = std::async(std::launch::async, [&] { std::string e; return queue->load(session, e); });
    while (task.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) queue->process();
    assert(task.get());
    // Stopping the AI server joins its worker from the owner thread: a load
    // that worker posted must fail, not wait for a frame that never comes,
    // and one posted during the stop must fail at once. The queue stays
    // open afterwards.
    {
        auto waiting = std::async(std::launch::async, [&] { std::string e; return queue->load(session, e); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        queue->suspendRemote(true);
        assert(!waiting.get());
        auto late = std::async(std::launch::async, [&] { std::string e; return queue->load(session, e); });
        assert(!late.get());
        queue->suspendRemote(false);
        auto again = std::async(std::launch::async, [&] { std::string e; return queue->load(session, e); });
        while (again.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) queue->process();
        assert(again.get());
    }
    // The owner's own load is behind the same exception barrier as process().
    {
        SessionLoadQueue throwing([](const SessionArchive&, std::string&) -> bool {
            throw std::runtime_error("out of memory");
        });
        std::string e;
        assert(!throwing.load(session, e) && e == "out of memory");
    }
    auto blocked = std::async(std::launch::async, [&] { std::string e; return queue->load(session, e); });
    queue->close(); assert(!blocked.get());
}
