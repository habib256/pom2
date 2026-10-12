// POM2 — GPL-3.0-or-later
// Drives must identify fixed HD mechanisms even empty or smaller than 800K,
// and the notch icon must describe the disk rather than the process policy.
#include "CffaCard.h"
#include "LironCard.h"
#include "ProDOSHardDiskCard.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

int main()
{
    namespace fs = std::filesystem;
    const auto scratch = fs::temp_directory_path() / ("pom2_drive_rack_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(scratch);
    auto image = [&](const char* name, std::size_t bytes) {
        const auto path = scratch / name;
        const std::vector<char> data(bytes, 0);
        std::ofstream stream(path, std::ios::binary);
        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        assert(stream.good());
        return path.string();
    };
    const auto hd = image("small.hdv", 64 * 512);
    const auto floppy = image("floppy.po", 819200);

    ProDOSHardDiskCard hardDisk(7);
    pom2::CffaCard cffa(4);
    for (pom2::MountableMediaCard* card : {
            static_cast<pom2::MountableMediaCard*>(&hardDisk),
            static_cast<pom2::MountableMediaCard*>(&cffa)}) {
        for (int bay = 0; bay < card->bayCount(); ++bay) {
            auto info = card->bayInfo(bay);
            assert(!info.loaded);
            assert(info.typeKey == "hdv" && !info.kindLabel.empty());
            std::string error;
            assert(card->mountBay(bay, hd, error));
            info = card->bayInfo(bay);
            assert(info.loaded && info.blockCount == 64);
            assert(info.typeKey == "hdv");
            card->setBayHostWriteProtected(bay, true);
            assert(card->bayInfo(bay).fileWriteProtected);
            card->setBayHostWriteProtected(bay, false);
            assert(!card->bayInfo(bay).fileWriteProtected);
            assert(card->ejectBay(bay));
            assert(card->bayInfo(bay).typeKey == "hdv");
        }
    }

    pom2::SmartPortCard smart(5);
    smart.setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    smart.setUnit(1, std::make_unique<pom2::SmartPortHdvUnit>());
    assert(smart.bayInfo(0).typeKey == "35");
    assert(smart.bayInfo(1).typeKey == "hdv");
    for (int bay = 0; bay < 2; ++bay) {
        std::string error;
        assert(smart.mountBay(bay, bay == 0 ? floppy : hd, error));
        smart.setBayWriteBack(bay, false);
        auto info = smart.bayInfo(bay);
        // The guest is protected by policy, but there is no sticker on
        // either disk. Clicking the notch must ADD protection, not clear it.
        assert(info.writeProtected);
        assert(!info.fileWriteProtected);
        smart.setBayHostWriteProtected(bay, true);
        assert(smart.bayInfo(bay).fileWriteProtected);
        smart.setBayHostWriteProtected(bay, false);
        assert(!smart.bayInfo(bay).fileWriteProtected);
        assert(smart.bayInfo(bay).writeProtected);
        assert(smart.ejectBay(bay));
    }
    pom2::LironCard liron(5);
    std::string error;
    assert(liron.mountBay(0, floppy, error));
    liron.setBayWriteBack(0, false);
    assert(liron.bayInfo(0).writeProtected);
    assert(!liron.bayInfo(0).fileWriteProtected);
    liron.setBayHostWriteProtected(0, true);
    assert(liron.bayInfo(0).fileWriteProtected);
    liron.setBayHostWriteProtected(0, false);
    assert(!liron.bayInfo(0).fileWriteProtected);
    assert(liron.ejectBay(0));
    fs::remove_all(scratch);
}
