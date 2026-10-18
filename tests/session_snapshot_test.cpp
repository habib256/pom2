// POM2 — GPL-3.0-or-later
#include "SessionArchive.h"
#include "SessionFiles.h"
#include "SessionSnapshot.h"
#include "DiskIICard.h"
#include "ClockCard.h"
#include "EmulationController.h"
#include "Mockingboard.h"
#include "SnesMaxCard.h"
#include "SlinkyCard.h"
#include "SnapshotIO.h"
#include "SystemProfile.h"
#include "TestTempPath.h"
#include "ByteIO.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
using Blob = std::vector<uint8_t>;
using pom2::SessionArchive;
struct Unsupported final : SlotPeripheral {
    std::string_view name() const override { return "unsupported session card"; }
};
struct Fixture {
    EmulationController ctrl;
    DiskIICard* disk = nullptr;
    MockingboardCard* mb = nullptr;
    pom2::SnesMaxCard* pad = nullptr;
    std::string source = pom2test::tempPath("pom2-session-source.dsk");
    std::string bundle = pom2test::tempPath("pom2-roundtrip.pom2session");
    std::string error;
    explicit Fixture(MockingboardCard::Variant variant = MockingboardCard::Variant::SoundII,
                     bool bitLss = false)
    {
        Blob bytes(DiskImage::kBytesPerImage);
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 13 + i / 256);
        { std::ofstream f(source, std::ios::binary); f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); assert(f.good()); }
        auto card = std::make_unique<DiskIICard>(6);
        if (bitLss) {
            const auto prom = std::filesystem::path(__FILE__).parent_path().parent_path() /
                              "roms" / "diskii_p6.rom";
            assert(card->loadLssRom(prom.string()));
        }
        assert(card->insertDisk(source));
        card->driveImage(0).setHostWriteProtected(true);
        card->setIwmHost(false);
        disk = card.get();
        auto sound = std::make_unique<MockingboardCard>(4, variant);
        mb = sound.get();
        auto controller = std::make_unique<pom2::SnesMaxCard>(5);
        pad = controller.get();
        ctrl.setMachineId(pom2::snapshotMachineId(pom2::SystemProfile::AppleIIe));
        auto state = ctrl.lockState();
        state.memory().setIIEMode(true);
        state.memory().slotBus().plug(4, std::move(sound));
        state.memory().slotBus().plug(5, std::move(controller));
        state.memory().slotBus().plug(6, std::move(card));
        state.memory().setCycleCounter(5000);
        mb->setCpu(&state.cpu());
        disk->setCpu(&state.cpu());
        disk->setIWM(&ctrl.iwm());
        state.cpu().setProgramCounter(0x2000);
        state.memory().writeRamUnchecked(0x2000, 0xE8); // INX
        state.memory().writeRamUnchecked(0x2001, 0x4C); // JMP $2000
        state.memory().writeRamUnchecked(0x2002, 0x00);
        state.memory().writeRamUnchecked(0x2003, 0x20);
        mb->slotRomWrite(0x44, 0x34); // VIA timer + SSI register alias
        mb->slotRomWrite(0x45, 0x12);
        pad->setButtons(0, pom2::SnesMaxCard::A | pom2::SnesMaxCard::B);
        pad->deviceSelectWrite(0, 0);
        pad->advanceCycles(7);
        disk->deviceSelectRead(9); // motor on, non-default read state
        disk->advanceCycles(1000);
    }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove(source, ec);
        std::filesystem::remove(bundle, ec);
    }
    SessionArchive capture()
    {
        SessionArchive out;
        auto state = ctrl.lockState();
        const bool ok = pom2::captureSession(out, state.cpu(), state.memory(), ctrl.machineId(), error);
        if (!ok) std::fprintf(stderr, "capture: %s\n", error.c_str());
        assert(ok);
        return out;
    }
    Blob encoded()
    {
        Blob bytes;
        assert(pom2::encodeSessionArchive(capture(), bytes, error));
        return bytes;
    }

};

void roundtrip(MockingboardCard::Variant variant, bool bitLss)
{
    auto fixture = std::make_unique<Fixture>(variant, bitLss);
    auto& f = *fixture;
    const Blob original = f.encoded();
    assert(pom2::saveSessionFile(f.ctrl, f.bundle, f.error));
    auto diskTrace = [&]() {
        Blob trace;
        auto state = f.ctrl.lockState();
        for (int i = 0; i < 512; ++i) {
            f.disk->advanceCycles(32);
            trace.push_back(f.disk->deviceSelectRead(0xC));
        }
        return trace;
    };
    const Blob expectedTrace = diskTrace();
    {
        auto state = f.ctrl.lockState();
        state.cpu().setProgramCounter(0x3333);
        state.memory().writeRamUnchecked(0x2000, 0xEA);
        f.mb->onPowerCycle();
        f.pad->onPowerCycle();
        assert(f.disk->ejectDisk());
    }
    assert(std::filesystem::remove(f.source)); // bundle must not need it
    if (!pom2::loadSessionFile(f.ctrl, f.bundle, f.error)) {
        std::fprintf(stderr, "load: %s\n", f.error.c_str()); assert(false);
    }
    assert(f.encoded() == original);
    assert(diskTrace() == expectedTrace);
    {
        auto state = f.ctrl.lockState();
        assert(f.disk->isDiskLoaded());
        assert(f.disk->driveImage(0).isFileWriteProtected());
        assert(f.disk->getDiskPath() == "(session disk)");
        const uint8_t first = f.disk->driveImage(0).nibbleAt(0, 0);
        f.disk->driveImage(0).writeNibbleAt(0, 0, first ^ 1);
        assert(f.disk->driveImage(0).nibbleAt(0, 0) == first);
        const auto oldX = state.cpu().getXRegister();
        state.cpu().run(20);
        assert(state.cpu().getXRegister() != oldX); // execution resumes in restored RAM
    }
    // Strictly different magic: loading as a CPU-only snapshot cannot
    // accidentally report success while dropping cards and embedded media.
    pom2::SnapshotReader oldReader(original.data(), original.size());
    assert(!oldReader.good());
}

void malformed()
{
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    const SessionArchive good = f.capture();
    auto legacy = good; legacy.version = 1;
    Blob oldBytes; SessionArchive oldArchive;
    assert(pom2::encodeSessionArchive(legacy, oldBytes, f.error));
    assert(pom2::decodeSessionArchive(oldBytes, oldArchive, f.error));
    assert(oldArchive.version == 1);
    { auto state = f.ctrl.lockState();
      assert(pom2::restoreSession(oldArchive, state.cpu(), state.memory(), f.ctrl.machineId(), f.error)); }
    legacy.version = 2;
    assert(pom2::encodeSessionArchive(legacy, oldBytes, f.error));
    assert(pom2::decodeSessionArchive(oldBytes, oldArchive, f.error));
    assert(oldArchive.version == 2);
    { auto state = f.ctrl.lockState();
      assert(pom2::restoreSession(oldArchive, state.cpu(), state.memory(), f.ctrl.machineId(), f.error)); }

    Blob bytes;
    assert(pom2::encodeSessionArchive(good, bytes, f.error));
    SessionArchive parsed;
    assert(pom2::decodeSessionArchive(bytes, parsed, f.error));
    for (size_t pos : {size_t(0), size_t(8), size_t(12), size_t(16), size_t(20), bytes.size() - 5}) {
        Blob bad = bytes; bad[pos] ^= 1;
        assert(!pom2::decodeSessionArchive(bad, parsed, f.error));
    }
    for (size_t n : {size_t(0), size_t(23), bytes.size() - 1}) {
        Blob bad(bytes.begin(), bytes.begin() + n);
        assert(!pom2::decodeSessionArchive(bad, parsed, f.error));
    }
    auto reject = [&](SessionArchive bad) {
        auto state = f.ctrl.lockState();
        state.memory().memWrite(0xC050, 0);
        state.memory().beginVideoEventFrame();
        state.memory().memWrite(0xC051, 0);
        state.memory().memWrite(0xC050, 0);
        SessionArchive before;
        assert(pom2::captureSession(before, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        assert(!pom2::restoreSession(bad, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        SessionArchive after;
        assert(pom2::captureSession(after, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        Blob a, b;
        assert(pom2::encodeSessionArchive(before, a, f.error));
        assert(pom2::encodeSessionArchive(after, b, f.error));
        assert(a == b);
        assert(state.memory().takeVideoEvents().size() == 2);
    };
    auto bad = good; bad.machineId ^= 1; reject(bad);
    bad = good; bad.configuration[2] ^= 1; reject(bad);
    bad = good; bad.slots[6].identity[2] ^= 1; reject(bad);
    bad = good; bad.slots[6].state.pop_back(); reject(bad);
    bad = good;
    // Canonical but impossible clocks must not reach an active sequencer.
    for (size_t i = 0; i < 8; ++i) bad.slots[6].state[4 + 59 + i] = 0xFF;
    reject(bad);
    bad = good; bad.slots[4].state[0] ^= 1; reject(bad);
    bad = good;
    for (size_t i = 0; i < 4; ++i) bad.slots[4].state[6 + 9 + i] = 0xFF;
    reject(bad);
    bad = good; bad.slots[5].state[0] ^= 1; reject(bad);
    bad = good; bad.core.pop_back(); reject(bad);
    bad = good; bad.slots[1].kind = SessionArchive::Card::SnesMax; reject(bad);
    // No file writes when the suffix is wrong, and failures leave the
    // previous session file intact rather than partially replacing it.
    assert(pom2::saveSessionFile(f.ctrl, f.bundle, f.error));
    assert(!pom2::saveSessionFile(f.ctrl, f.source, f.error));
    { auto state = f.ctrl.lockState(); state.memory().slotBus().plug(1, std::make_unique<Unsupported>()); }
    assert(!pom2::saveSessionFile(f.ctrl, f.bundle, f.error));
    { auto state = f.ctrl.lockState(); state.memory().slotBus().unplug(1); }
    assert(pom2::loadSessionFile(f.ctrl, f.bundle, f.error));
    { auto state = f.ctrl.lockState(); f.disk->driveImage(0).setHostWriteProtected(false); }
    // Embedded media have a physical header lock that the host notch cannot clear.
    assert(pom2::saveSessionFile(f.ctrl, f.bundle, f.error));
    { auto state = f.ctrl.lockState(); f.ctrl.cassette().armRecording(); }
    assert(!pom2::saveSessionFile(f.ctrl, f.bundle, f.error));
}

void writableRefusal()
{
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    const auto saved = f.capture();
    auto state = f.ctrl.lockState();
    f.disk->driveImage(0).setHostWriteProtected(false);
    SessionArchive out;
    assert(pom2::captureSession(out, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
    // A dirty outgoing disk must not disappear when loading an older session.
    const uint8_t old = f.disk->driveImage(0).nibbleAt(0, 0);
    f.disk->driveImage(0).writeNibbleAt(0, 0, old ^ 1);
    assert(f.disk->driveImage(0).hasUnsavedChanges());
    const auto pc = state.cpu().getProgramCounter();
    assert(!pom2::restoreSession(saved, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
    assert(state.cpu().getProgramCounter() == pc);
    assert(f.disk->driveImage(0).nibbleAt(0, 0) == uint8_t(old ^ 1));
    f.disk->setWriteBackEnabled(false); // fixture changes must never reach its source
}

void slinkySessions()
{
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    Blob rom(pom2::SlinkyCard::kRomBytes, 0xA5);
    auto card = std::make_unique<pom2::SlinkyCard>(2);
    assert(card->loadRom(rom));
    auto* slinky = card.get();
    {
        auto state = f.ctrl.lockState();
        state.memory().slotBus().plug(2, std::move(card));
        slinky->deviceSelectWrite(0, 0x34);
        slinky->deviceSelectWrite(1, 0x12);
        slinky->deviceSelectWrite(2, 0x07);
        slinky->deviceSelectWrite(3, 0x5A);
        slinky->deviceSelectWrite(0, 0x34); // reload the saved pointer
    }
    const auto saved = f.capture();
    assert(saved.version == 6);
    assert(saved.slots[2].kind == SessionArchive::Card::Slinky);
    assert(saved.slots[2].identity == rom);
    Blob encoded;
    SessionArchive decoded;
    assert(pom2::encodeSessionArchive(saved, encoded, f.error));
    assert(pom2::decodeSessionArchive(encoded, decoded, f.error));
    {
        auto state = f.ctrl.lockState();
        state.cpu().setProgramCounter(0x3333);
        slinky->deviceSelectWrite(3, 0x11);
        assert(slinky->ramAt(0x71234) == 0x11);
        assert(pom2::restoreSession(decoded, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        assert(state.cpu().getProgramCounter() == 0x2000);
        assert(slinky->ramAt(0x71234) == 0x5A);
        assert(slinky->livePointer() == 0x71234);

        // A malformed sparse page record must be rejected before core/RAM
        // changes. The outer archive remains well-formed and checksummed.
        auto corrupt = decoded;
        corrupt.slots[2].state.push_back(0);
        const auto pc = state.cpu().getProgramCounter();
        assert(!pom2::restoreSession(corrupt, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        assert(state.cpu().getProgramCounter() == pc);
        assert(slinky->ramAt(0x71234) == 0x5A);

        rom[0] ^= 1;
        assert(slinky->loadRom(rom));
        assert(!pom2::restoreSession(decoded, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        assert(state.cpu().getProgramCounter() == pc);
        rom[0] ^= 1;
        assert(slinky->loadRom(rom));
    }
    // Older formats retain their exact card-kind vocabulary.
    auto legacy = saved;
    legacy.version = 2;
    assert(!pom2::encodeSessionArchive(legacy, encoded, f.error));
    { auto state = f.ctrl.lockState();
      assert(!pom2::restoreSession(legacy, state.cpu(), state.memory(), f.ctrl.machineId(), f.error)); }

    // A populated 1 MB card exceeds v1's 1 MB per-card record limit once
    // sparse page indices are included. V3 must carry the whole RAM disk.
    {
        auto state = f.ctrl.lockState();
        for (uint32_t page = 0; page < 4096; ++page) {
            const uint32_t address = page * 256;
            slinky->deviceSelectWrite(0, uint8_t(address));
            slinky->deviceSelectWrite(1, uint8_t(address >> 8));
            slinky->deviceSelectWrite(2, uint8_t(address >> 16));
            slinky->deviceSelectWrite(3, uint8_t(page % 254));
        }
    }
    const auto full = f.capture();
    assert(full.slots[2].state.size() > 1024u * 1024u);
    assert(pom2::encodeSessionArchive(full, encoded, f.error));
    assert(pom2::decodeSessionArchive(encoded, decoded, f.error));
    { auto state = f.ctrl.lockState();
      assert(pom2::restoreSession(decoded, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
      assert(slinky->ramAt(0xFFF00) == 31); }
}

void clockSessions()
{
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    auto card = std::make_unique<ClockCard>(1);
    auto* clock = card.get();
    Blob expected;
    {
        auto state = f.ctrl.lockState();
        clock->setCpuClock(pom2VideoTiming(state.memory().videoStandard()).cpuClockHz);
        state.memory().slotBus().plug(1, std::move(card));
        clock->deviceSelectWrite(0, 0x20); // TP 64 Hz, STB low
        clock->deviceSelectWrite(0, 0x24); // latch timer
        clock->deviceSelectWrite(0, 0x40); // enable IRQ
        clock->advanceCycles(18000);
        assert(clock->tpRateHz() == 64 && clock->slotIrqAsserted());
        clock->appendSnapshotState(expected);
    }
    const auto saved = f.capture();
    assert(saved.slots[1].kind == SessionArchive::Card::Clock);
    assert(saved.slots[1].state == expected);
    assert(saved.slots[1].identity.size() == (clock->takesC800() ? 2305u : 257u));
    Blob encoded;
    SessionArchive decoded;
    assert(pom2::encodeSessionArchive(saved, encoded, f.error));
    assert(pom2::decodeSessionArchive(encoded, decoded, f.error));
    {
        auto state = f.ctrl.lockState();
        clock->onReset();
        state.cpu().setProgramCounter(0x3333);
        assert(!clock->slotIrqAsserted());
        assert(pom2::restoreSession(decoded, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        Blob restored;
        clock->appendSnapshotState(restored);
        assert(restored == expected && clock->slotIrqAsserted());
        assert(state.cpu().getProgramCounter() == 0x2000);

        auto reject = [&](SessionArchive bad) {
            const auto pc = state.cpu().getProgramCounter();
            Blob before, after;
            clock->appendSnapshotState(before);
            assert(!pom2::restoreSession(bad, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
            clock->appendSnapshotState(after);
            assert(before == after && state.cpu().getProgramCounter() == pc);
        };
        auto bad = saved; bad.slots[1].identity[1] ^= 1; reject(bad);
        bad = saved; bad.slots[1].state[21] = 0x01; reject(bad); // impossible TP rate
        bad = saved;
        for (int i = 0; i < 8; ++i) bad.slots[1].state[13 + i] = 0x7F;
        reject(bad); // absurd time offset
        bad = saved; bad.slots[1].state[25] ^= 1; reject(bad); // timer/clock mismatch
    }
}

// Small synthetic tracks, including distinct quarter-tracks and raw FLUX
// deltas. No downloaded/protected software is needed for this regression.
Blob sessionWoz(bool v2, bool flux, uint8_t timing)
{
    Blob out{'W','O','Z',uint8_t(v2 ? '2' : '1'),0xFF,0x0A,0x0D,0x0A,0,0,0,0};
    auto chunk = [&](const char* id, const Blob& data) {
        out.insert(out.end(), id, id + 4);
        pom2::byteio::putU32(out, uint32_t(data.size()));
        out.insert(out.end(), data.begin(), data.end());
    };
    Blob info(60, 0); info[0] = flux ? 3 : (v2 ? 2 : 1);
    info[1] = 1; info[2] = 1; info[39] = timing;
    if (flux) { info[46] = 5; info[48] = 1; }
    chunk("INFO", info);
    Blob map(160, 0xFF);
    map[0] = 0; map[1] = 1; map[4] = 1; map[7] = 0; map[159] = 1;
    chunk("TMAP", map);
    Blob tracks(v2 ? 2304 : 2 * 6656, 0);
    for (size_t i = 0; i < 2; ++i) {
        const uint32_t bits = 1003 + uint32_t(i) * 28;
        const size_t offset = v2 ? 1280 + i * 512 : i * 6656;
        for (size_t b = 0; b < 160; ++b) tracks[offset + b] = uint8_t(0xD5 ^ (b * 17 + i * 39));
        if (v2) {
            tracks[i * 8] = uint8_t(3 + i); tracks[i * 8 + 2] = 1;
            for (int b = 0; b < 4; ++b) tracks[i * 8 + 4 + b] = uint8_t(bits >> (8 * b));
        } else {
            tracks[offset + 6646] = 160;
            tracks[offset + 6648] = uint8_t(bits);
            tracks[offset + 6649] = uint8_t(bits >> 8);
        }
    }
    if (flux) {
        const Blob deltas{31,17,255,255,9,32,10,6};
        for (size_t i = 0; i < deltas.size(); ++i) tracks[1792 + i] = deltas[i];
        tracks[12] = uint8_t(deltas.size()); tracks[13] = 0;
    }
    chunk("TRKS", tracks);
    if (flux) {
        assert(out.size() == 2560);
        Blob indices(160, 0xFF); indices[1] = indices[4] = indices[159] = 1;
        chunk("FLUX", indices);
    }
    // A legal opaque chunk exercises the v2 card limit beyond v1's 1 MiB.
    if (v2) chunk("META", Blob(1100 * 1024, 'x'));
    return out;
}

void wozSessions()
{
    for (int mode = 0; mode < 4; ++mode) {
        auto fixture = std::make_unique<Fixture>();
        auto& f = *fixture;
        const std::string path = pom2test::tempPath("pom2-session-source.woz");
        Blob raw = sessionWoz(mode != 0, mode == 2, mode == 1 ? 40 : 28);
        if (mode == 3) raw[22] = 0; // protection supplied by the host notch
        { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(raw.data()), raw.size()); assert(file.good()); }
        {
            auto state = f.ctrl.lockState();
            assert(f.disk->insertDisk(path));
            assert(f.disk->insertDisk(1, path)); // both drives embedded
            if (mode == 3) {
                f.disk->driveImage(0).setHostWriteProtected(true);
                f.disk->driveImage(1).setHostWriteProtected(true);
            }
            f.disk->deviceSelectRead(9); f.disk->advanceCycles(1000);
            f.disk->deviceSelectRead(0xC);
        }
        const auto original = f.capture();
        assert(original.version == 6);
        auto previous = original; previous.version = 2;
        Blob previousBytes;
        SessionArchive previousDecoded;
        assert(pom2::encodeSessionArchive(previous, previousBytes, f.error));
        assert(pom2::decodeSessionArchive(previousBytes, previousDecoded, f.error));
        { auto state = f.ctrl.lockState();
          assert(pom2::restoreSession(previousDecoded, state.cpu(), state.memory(), f.ctrl.machineId(), f.error)); }
        assert(pom2::saveSessionFile(f.ctrl, f.bundle, f.error));
        auto trace = [&] {
            Blob result;
            for (int i = 0; i < 256; ++i) {
                f.disk->advanceCycles(7);
                result.push_back(f.disk->deviceSelectRead(0xC));
            }
            return result;
        };
        Blob expected;
        auto reference = std::make_unique<DiskImage>();
        assert(reference->loadFile(path));
        { auto state = f.ctrl.lockState(); expected = trace(); assert(f.disk->ejectDisk()); assert(f.disk->ejectDisk(1)); }
        std::filesystem::remove(path);
        assert(pom2::loadSessionFile(f.ctrl, f.bundle, f.error));
        Blob before, after;
        assert(pom2::encodeSessionArchive(original, before, f.error));
        assert(pom2::encodeSessionArchive(f.capture(), after, f.error));
        assert(before == after);
        { auto state = f.ctrl.lockState(); assert(trace() == expected); }
        for (int drive = 0; drive < 2; ++drive) {
            const auto& restored = f.disk->driveImage(drive);
            assert(restored.isWoz() && restored.isFileWriteProtected());
            assert(restored.getPath() == "(session disk)");
            for (int qt = 0; qt < DiskImage::kQuarterTracks; ++qt) {
                assert(restored.trackBitLength(qt) == reference->trackBitLength(qt));
                assert(restored.trackPeriod(qt) == reference->trackPeriod(qt));
                assert(restored.fluxEvents(qt) == reference->fluxEvents(qt));
                for (int bit = 0; bit < reference->trackBitLength(qt); ++bit)
                    assert(restored.bitAt(qt, bit) == reference->bitAt(qt, bit));
            }
        }
        auto bad = original;
        bad.version = 1; // v1 must never silently gain WOZ semantics
        auto state = f.ctrl.lockState();
        const auto pc = state.cpu().getProgramCounter();
        assert(!pom2::restoreSession(bad, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        assert(state.cpu().getProgramCounter() == pc);
        // Invalid nested WOZ must be caught even with a valid outer CRC.
        bad = original;
        auto& card = bad.slots[6].state;
        uint32_t runtime = 0;
        for (int b = 0; b < 4; ++b) runtime |= uint32_t(card[b]) << (8 * b);
        card[4 + runtime + 4 + 4 + 4] ^= 1; // first WOZ sentinel byte
        Blob encoded;
        assert(pom2::encodeSessionArchive(bad, encoded, f.error));
        SessionArchive parsed;
        assert(pom2::decodeSessionArchive(encoded, parsed, f.error));
        assert(!pom2::restoreSession(parsed, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        assert(state.cpu().getProgramCounter() == pc);
    }
}

void writableWozSessions()
{
    for (bool flux : {false, true}) {
        auto fixture = std::make_unique<Fixture>(MockingboardCard::Variant::AC, true);
        auto& f = *fixture;
        const auto path = pom2test::tempPath("pom2-session-writable.woz");
        auto source = sessionWoz(true, flux, 32); source[22] = 0;
        { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(source.data()), source.size()); }
        auto state = f.ctrl.lockState();
        assert(f.disk->insertDisk(path)); f.disk->setWriteBackEnabled(true);
        const int qt = flux ? 1 : 0;
        const int64_t ticks[] = {16, 32, 48};
        f.disk->driveImage(0).writeFlux(qt, 0, 64, 3, ticks, 0);
        pom2::SessionArchive saved, again;
        const auto expected = f.disk->driveImage(0).fluxEvents(qt);
        assert(pom2::captureSession(saved, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        assert(f.disk->driveImage(0).hasUnsavedChanges());
        f.disk->driveImage(0).eject();
        std::filesystem::remove(path);
        if (!pom2::restoreSession(saved, state.cpu(), state.memory(), f.ctrl.machineId(), f.error)) {
            std::fprintf(stderr, "writable WOZ: %s\n", f.error.c_str()); assert(false);
        }
        assert(!f.disk->driveImage(0).isWriteProtected());
        assert(f.disk->driveImage(0).fluxEvents(qt) == expected);
        assert(pom2::captureSession(again, state.cpu(), state.memory(), f.ctrl.machineId(), f.error));
        Blob a, b; assert(pom2::encodeSessionArchive(saved, a, f.error));
        assert(pom2::encodeSessionArchive(again, b, f.error) && a == b);
        const int64_t more[] = {80, 96};
        f.disk->driveImage(0).writeFlux(qt, 64, 112, 2, more, 0);
        assert(f.disk->driveImage(0).hasUnsavedChanges());
        assert(f.disk->driveImage(0).saveDirty());
        assert(!std::filesystem::exists("(session disk)"));
    }
}

void diskFormats()
{
    for (bool nib : {false, true}) {
        const std::string path = pom2test::tempPath(nib ? "pom2-session-format.nib" : "pom2-session-format.d13");
        Blob bytes(nib ? size_t(DiskImage::kTracks) * DiskImage::kNibblesPerTrack
                       : DiskImage::kBytesPerImage13, 0xA6);
        { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); assert(f.good()); }
        auto original = std::make_unique<DiskImage>();
        auto restored = std::make_unique<DiskImage>();
        assert(original->loadFile(path));
        original->setHostWriteProtected(true);
        Blob state;
        std::string error;
        assert(original->appendReadOnlySession(state, error));
        std::filesystem::remove(path);
        assert(restored->loadReadOnlySession(state.data(), state.size(), error));
        assert(original->is13Sector() == restored->is13Sector());
        assert(original->isNib() == restored->isNib());
        for (int track = 0; track < DiskImage::kTracks; ++track) {
            for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i)
                assert(original->nibbleAt(track, i) == restored->nibbleAt(track, i));
            assert(original->trackBitLength(track * 4) == restored->trackBitLength(track * 4));
        }
    }
}
}
int main()
{
    for (bool bitLss : {false, true}) {
        roundtrip(MockingboardCard::Variant::AC, bitLss);
        roundtrip(MockingboardCard::Variant::SoundII, bitLss);
    }
    malformed();
    writableRefusal();
    slinkySessions();
    clockSessions();
    diskFormats();
    wozSessions();
    writableWozSessions();
    std::puts("session_snapshot: all ok");
}
