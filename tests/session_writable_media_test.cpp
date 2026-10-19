// POM2 — GPL-3.0-or-later
#include "DiskImage.h"
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "Block512Backing.h"
#include "MediaAutosave.h"
#include "SessionSnapshot.h"
#include "SystemProfile.h"
#include "Sony35Drive.h"
#include "TestTempPath.h"
#include "Woz35Fixture.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace {
using Blob = std::vector<uint8_t>;
struct NoHostCommits final : pom2::MediaCommitExecutor {
    int calls = 0;
    std::shared_ptr<pom2::MediaCommitOperation> submit(std::function<pom2::MediaCommitResult()>) override {
        ++calls; return {};
    }
};
Blob read(const std::string& path) {
    std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}};
}
std::string makeDisk(const char* name, size_t size) {
    const auto path = pom2test::tempPath(name);
    Blob bytes(size, 0xAA);
    std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return path;
}
void framing() {
    const auto path = makeDisk("pom2-session-framing.dsk", DiskImage::kBytesPerImage);
    const Blob original = read(path);
    auto image = std::make_unique<DiskImage>(), restored = std::make_unique<DiskImage>();
    image->setWriteBackEnabled(true); assert(image->loadFile(path));
    const int64_t ticks[] = {0, 8, 24, 40, 56, 64, 80, 96, 112, 120, 136, 152, 160, 176, 192};
    image->writeFlux(0, 0, 29, 15, ticks, 0); // capture in the middle of a nibble/cell
    const auto epoch = pom2::mediaWriteEpoch().load();
    Blob saved; std::string error;
    assert(image->appendSessionMedia(saved, error));
    assert(pom2::mediaWriteEpoch().load() == epoch && read(path) == original);
    assert(!restored->loadSessionMedia(saved.data(), saved.size(), error, false));
    assert(restored->loadSessionMedia(saved.data(), saved.size(), error, true));
    assert(restored->isSessionMedia() && !restored->isWriteProtected());
    image->writeFlux(0, 29, 208, 15, ticks, 0);
    restored->writeFlux(0, 29, 208, 15, ticks, 0);
    for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i) assert(image->nibbleAt(0, i) == restored->nibbleAt(0, i));
    Blob a, b; assert(image->appendSessionMedia(a, error)); assert(restored->appendSessionMedia(b, error));
    assert(a == b);
    DiskImage::PendingWriteBack pending;
    assert(restored->takeWriteBack(pending) && !pending.valid);
    assert(restored->saveDirty() && read(path) == original);
    NoHostCommits executor; assert(!restored->pollAutosave(executor, true) && !executor.calls);
    assert(restored->persistence().state == "session");
    assert(!std::filesystem::exists("(session disk)"));
    // Malformed framing and unknown flag bits must leave the loaded medium intact.
    const auto value = restored->nibbleAt(0, 0);
    auto bad = saved; bad[4] = 0x80;
    assert(!restored->loadSessionMedia(bad.data(), bad.size(), error, true));
    bad = saved; bad.back() = 0x7F;
    assert(!restored->loadSessionMedia(bad.data(), bad.size(), error, true));
    assert(restored->nibbleAt(0, 0) == value && restored->isSessionMedia());
    std::filesystem::remove(path);
}
void diskII() {
    const auto path = makeDisk("pom2-session-writable.dsk", DiskImage::kBytesPerImage);
    const auto original = read(path);
    auto ctrl = std::make_unique<EmulationController>();
    ctrl->setMachineId(pom2::snapshotMachineId(pom2::SystemProfile::AppleIIe));
    std::string error;
    auto state = ctrl->lockState(); state.memory().setIIEMode(true);
    auto card = std::make_unique<DiskIICard>(6); auto* disk = card.get();
    card->setWriteBackEnabled(true); assert(card->insertDisk(path));
    state.memory().slotBus().plug(6, std::move(card));
    disk->driveImage(0).writeNibbleAt(0, 25, 0xD5);
    state.cpu().setProgramCounter(0x2345);
    pom2::SessionArchive session;
    assert(pom2::captureSession(session, state.cpu(), state.memory(), ctrl->machineId(), error));
    assert(session.version == 7 && read(path) == original);
    const auto pc = state.cpu().getProgramCounter();
    assert(!pom2::restoreSession(session, state.cpu(), state.memory(), ctrl->machineId(), error));
    assert(state.cpu().getProgramCounter() == pc); // don't discard outgoing dirty host media
    disk->driveImage(0).eject(); // test deliberately drops its temporary unsaved source
    if (!pom2::restoreSession(session, state.cpu(), state.memory(), ctrl->machineId(), error)) {
        std::fprintf(stderr, "writable Disk II restore: %s\n", error.c_str()); assert(false);
    }
    assert(!disk->driveImage(0).isWriteProtected() && disk->driveImage(0).isSessionMedia());
    assert(disk->driveImage(0).nibbleAt(0, 25) == 0xD5);
    disk->driveImage(0).writeNibbleAt(0, 25, 0xAB);
    pom2::SessionArchive changed;
    assert(pom2::captureSession(changed, state.cpu(), state.memory(), ctrl->machineId(), error));
    assert(pom2::restoreSession(session, state.cpu(), state.memory(), ctrl->machineId(), error));
    assert(disk->driveImage(0).nibbleAt(0, 25) == 0xD5);
    assert(pom2::restoreSession(changed, state.cpu(), state.memory(), ctrl->machineId(), error));
    assert(disk->driveImage(0).nibbleAt(0, 25) == 0xAB);
    pom2::SessionArchive recaptured;
    assert(pom2::captureSession(recaptured, state.cpu(), state.memory(), ctrl->machineId(), error));
    Blob a, b; assert(pom2::encodeSessionArchive(changed, a, error));
    assert(pom2::encodeSessionArchive(recaptured, b, error) && a == b);
    assert(disk->driveImage(0).saveDirty() && read(path) == original);
    auto legacy = changed; legacy.version = 4;
    assert(!pom2::restoreSession(legacy, state.cpu(), state.memory(), ctrl->machineId(), error));
    std::filesystem::remove(path);
}
void raw35() {
    const auto path = makeDisk("pom2-session-writable35.po", pom2::Disk35Image::kBytesPerImage);
    const auto original = read(path);
    pom2::Disk35Image image, restored;
    image.setWriteBackEnabled(true); assert(image.loadFile(path));
    uint8_t data[512]; std::memset(data, 0x56, sizeof(data)); assert(image.writeBlock(17, data));
    Blob saved; std::string error;
    assert(image.appendSessionMedia(saved, error) && image.hasUnsavedChanges());
    assert(!restored.loadSessionMedia(saved.data(), saved.size(), error, false));
    assert(restored.loadSessionMedia(saved.data(), saved.size(), error, true));
    uint8_t got[512]; assert(restored.readBlock(17, got) && !std::memcmp(data, got, 512));
    assert(restored.isSessionMedia() && !restored.isWriteProtected());
    std::memset(data, 0xA7, sizeof(data)); assert(restored.writeBlock(17, data));
    assert(restored.saveDirty() && !restored.takeWriteBack().valid && read(path) == original);
    NoHostCommits executor; assert(!restored.pollAutosave(executor, true) && !executor.calls);
    assert(restored.persistence().state == "session");
    assert(restored.appendSessionMedia(saved = {}, error));
    assert(image.sessionReplaceable() == false);
    image.eject(); assert(image.loadSessionMedia(saved.data(), saved.size(), error, true));
    assert(image.readBlock(17, got) && !std::memcmp(data, got, 512));
    std::filesystem::remove(path);
}
void captured35() {
    std::vector<std::vector<uint8_t>> tracks(160);
    Blob payload(12 * 512, 0x5A);
    tracks[0] = pom2::test::encodeTrack(0, 0, payload.data());
    const auto bytes = pom2::test::buildWoz(tracks);
    const auto path = pom2::test::writeTemp("pom2-writable-session35.woz", bytes);
    pom2::Disk35Image image, restored; image.setWriteBackEnabled(true);
    assert(image.loadFile(path.string()));
    const int64_t ticks[] = {101, 125};
    assert(image.writeFlux125ns(0, 0, 100, 140, ticks, 2, 0));
    Blob media; std::string error;
    assert(image.appendSessionMedia(media, error));
    assert(restored.loadSessionMedia(media.data(), media.size(), error, true));
    assert(*image.originalFluxTicks(0, 0) == *restored.originalFluxTicks(0, 0));
    assert(!restored.isWriteProtected());
    const int64_t more[] = {333}; assert(restored.writeFlux125ns(0, 0, 320, 340, more, 1, 0));
    assert(restored.saveDirty() && read(path.string()) == bytes);
    Blob another; assert(restored.appendSessionMedia(another, error));
    pom2::Disk35Image third; assert(third.loadSessionMedia(another.data(), another.size(), error, true));
    assert(*third.originalFluxTicks(0, 0) == *restored.originalFluxTicks(0, 0));
    std::filesystem::remove(path);
}
void sonyPartialWrite() {
    const auto path = makeDisk("pom2-session-sony-write.po", pom2::Disk35Image::kBytesPerImage);
    pom2::Disk35Image image, restored;
    image.setWriteBackEnabled(true); assert(image.loadFile(path));
    pom2::Sony35Drive drive, next;
    drive.setImage(&image); drive.notifyMediaChange();
    drive.seekPhaseW(2); drive.seekPhaseW(10); drive.seekPhaseW(2); // MotorOn
    drive.writeStart();
    const int64_t ticks[] = {0, 14, 42, 70, 98, 126};
    drive.writeFlux(0, 64, ticks, 6, 0, 14);
    Blob media, mechanism, writeState; std::string error;
    assert(image.appendSessionMedia(media, error));
    drive.appendSnapshotState(mechanism); drive.appendSessionWriteState(writeState);
    assert(next.loadSessionWriteState(writeState.data(), writeState.size(), false));
    assert(restored.loadSessionMedia(media.data(), media.size(), error, true));
    next.setImage(&restored);
    assert(next.loadSnapshotState(mechanism.data(), mechanism.size()));
    assert(next.loadSessionWriteState(writeState.data(), writeState.size()));
    assert(drive.debugCellStream() == next.debugCellStream());
    drive.writeFlux(64, 144, ticks, 6, 0, 14);
    next.writeFlux(64, 144, ticks, 6, 0, 14);
    assert(drive.debugCellStream() == next.debugCellStream());
    Blob a, b; drive.appendSessionWriteState(a); next.appendSessionWriteState(b);
    assert(a == b);
    auto bad = writeState; bad.back() = 2;
    assert(!next.loadSessionWriteState(bad.data(), bad.size()));
    Blob after; next.appendSessionWriteState(after); assert(after == b);
    std::filesystem::remove(path);
}
}
int main() {
    framing(); diskII(); raw35(); captured35(); sonyPartialWrite();
    std::puts("writable session media: all passed");
}
