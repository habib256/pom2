// POM2 — GPL-3.0-or-later
#include "Disk35Image.h"
#include "Sony35Drive.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "Woz35Fixture.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
using namespace pom2::test;
namespace {
std::vector<uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
std::vector<int64_t> ticks(const std::vector<uint8_t>& cells) {
    std::vector<int64_t> events;
    for (size_t i = 0; i < cells.size(); ++i) if (cells[i]) events.push_back(int64_t(i) * 16);
    return events;
}
void test(bool moof) {
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<uint8_t> payload(12 * 512);
    for (int s = 0; s < 12; ++s) fillBlock(payload.data() + s * 512, s);
    tracks[0] = encodeTrack(0, 0, payload.data());
    tracks[2] = encodeTrack(1, 0, payload.data());
    auto bytes = buildWoz(tracks);
    if (moof) {
        std::memcpy(bytes.data(), "MOOF\xFF\x0A\x0D\x0A", 8);
        bytes[20] = 1; bytes[24] = 16;
    }
    bytes[88 + 1] = 0; // Both heads alias descriptor 0; writing one must split it.
    const std::vector<uint8_t> metadata = {'M','E','T','A',5,0,0,0,'h','e','l','l','o',
                                          'X','T','R','A',3,0,0,0,0xA5,0x5A,0x69};
    bytes.insert(bytes.end(), metadata.begin(), metadata.end());
    const auto path = writeTemp(moof ? "pom2-write35.moof" : "pom2-write35.woz", bytes);
    pom2::Disk35Image image;
    assert(image.loadFile(path.string())); image.setWriteBackEnabled(true);
    assert(!image.isWriteProtected() && image.isBlockWriteProtected());
    uint8_t block[512] = {}; assert(!image.writeBlock(0, block));
    const auto original = *image.originalFluxTicks(0, 0);
    const auto farSide = *image.originalFluxTicks(0, 1);
    const auto neighbor = *image.originalFluxTicks(1, 0);
    const auto period = image.trackPeriod125ns(0, 0);
    const int64_t transitions[] = {int64_t(period) - 5, int64_t(period) + 7, int64_t(period) + 31};
    assert(image.writeFlux125ns(0, 0, period - 30, int64_t(period) + 40, transitions, 3, period));
    auto expected = original;
    expected.erase(std::remove_if(expected.begin(), expected.end(), [&](uint32_t t) { return t >= period - 30 || t < 40; }), expected.end());
    expected.insert(expected.end(), {period - 5, 7, 31}); std::sort(expected.begin(), expected.end());
    assert(*image.originalFluxTicks(0, 0) == expected);
    assert(*image.originalFluxTicks(0, 1) == farSide && *image.originalFluxTicks(1, 0) == neighbor);
    assert(image.saveDirty());
    const auto saved = read(path);
    assert(saved != bytes && std::search(saved.begin(), saved.end(), metadata.begin(), metadata.end()) != saved.end());
    pom2::Disk35Image loaded;
    assert(loaded.loadFile(path.string())); loaded.setWriteBackEnabled(true);
    assert(*loaded.originalFluxTicks(0, 0) == expected && loaded.trackPeriod125ns(0, 0) == period);
    assert(*loaded.originalFluxTicks(0, 1) == farSide && *loaded.originalFluxTicks(1, 0) == neighbor);
    // A second save/reload preserves sub-cell times from the first capture.
    const int64_t more[] = {333};
    assert(loaded.writeFlux125ns(0, 0, 320, 340, more, 1, period));
    assert(loaded.saveDirty());
    assert(std::filesystem::file_size(path) == saved.size()); // reuse the existing FLUX allocation
    pom2::Disk35Image again; assert(again.loadFile(path.string())); again.setWriteBackEnabled(true);
    assert(std::binary_search(again.originalFluxTicks(0, 0)->begin(), again.originalFluxTicks(0, 0)->end(), 7));
    assert(std::binary_search(again.originalFluxTicks(0, 0)->begin(), again.originalFluxTicks(0, 0)->end(), 333));
    const int64_t newTrack[] = {123};
    assert(again.writeFlux125ns(2, 1, 0, 1000, newTrack, 1, 1000000));
    assert(again.saveDirty());
    pom2::Disk35Image formatted; assert(formatted.loadFile(path.string()));
    assert(formatted.trackPeriod125ns(2, 1) == 1000000);
    assert(*formatted.originalFluxTicks(2, 1) == std::vector<uint32_t>{123});
    // Entire-track rewrite refreshes the block projection from actual GCR.
    std::fill(payload.begin(), payload.end(), 0x5A);
    const auto newCells = encodeTrack(0, 0, payload.data()); const auto newTicks = ticks(newCells);
    assert(newCells.size() == tracks[0].size());
    assert(again.writeFlux125ns(0, 0, 0, period, newTicks.data(), int(newTicks.size()), period));
    assert(again.readBlock(0, block));
    assert(std::all_of(std::begin(block), std::end(block), [](uint8_t b) { return b == 0x5A; }));
    // Real Sony/IWM write path updates capture data and reaches the file.
    pom2::Sony35Drive drive; drive.setImage(&again); drive.notifyMediaChange();
    const int64_t nativePeriod = drive.ticksPerRev();
    drive.writeFlux(0, nativePeriod, nullptr, 0, 0, 0);
    assert(again.originalFluxTicks(0, 0)->empty());
    assert(again.saveDirty());
    pom2::Disk35Image erased; assert(erased.loadFile(path.string()));
    assert(erased.originalFluxTicks(0, 0)->empty() && *erased.originalFluxTicks(0, 1) == farSide);
    // Header and host notches still refuse physical writes.
    erased.setWriteBackEnabled(true); erased.setHostWriteProtected(true);
    assert(!erased.writeFlux125ns(0, 0, 0, 100, more, 1, period));
    auto protectedBytes = saved; protectedBytes[22] = 1;
    std::fill(protectedBytes.begin() + 8, protectedBytes.begin() + 12, 0); // CRC omitted, as permitted by the format.
    const auto protectedPath = writeTemp(moof ? "pom2-write35-locked.moof" : "pom2-write35-locked.woz", protectedBytes);
    pom2::Disk35Image locked; assert(locked.loadFile(protectedPath.string())); locked.setWriteBackEnabled(true);
    assert(locked.isWriteProtected() && !locked.writeFlux125ns(0, 0, 0, 100, more, 1, period));
    std::filesystem::remove(path); std::filesystem::remove(protectedPath);
    if (!moof) {
        // Valid old short INFO can be read, but lacks the fields needed to
        // publish FLUX. Refusal must never become an empty-file flush.
        auto old = bytes; old[16] = 44;
        std::copy_n("JUNK", 4, old.begin() + 64);
        old[68] = 8;
        std::fill(old.begin() + 69, old.begin() + 80, 0);
        const auto shortPath = writeTemp("pom2-write35-short-info.woz", old);
        pom2::SmartPortCard card(5);
        auto unit = std::make_unique<pom2::SmartPort35Unit>();
        assert(unit->loadImage(shortPath.string()));
        auto& medium = unit->image(); medium.setWriteBackEnabled(true);
        assert(medium.writeFlux125ns(0, 0, 0, 100, more, 1, period));
        card.setUnit(0, std::move(unit));
        pom2::MountableMediaCard::PendingBayFlush pending; std::string error;
        assert(!card.prepareFlushBay(0, pending, error));
        assert(!error.empty() && medium.hasUnsavedChanges() && !pending.valid);
        assert(read(shortPath) == old);
        assert(!medium.saveDirty() && medium.hasUnsavedChanges());
        assert(read(shortPath) == old);
        // Explicitly discard this synthetic fixture's rejected dirty state.
        medium.setWriteBackEnabled(false); std::filesystem::remove(shortPath);
    }
}
}
int main() { test(false); test(true); }
