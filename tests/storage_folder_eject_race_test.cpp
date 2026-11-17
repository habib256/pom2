// POM2 — GPL-3.0-or-later
// Exercise the real coordinator/card/backing/ProDOS decoder. The existing
// commit barrier gives a deterministic phase-2 interleaving, with no runtime
// test hook, worker scheduling assumption or sleep.
#include "EmulationController.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "Settings.h"
#include "StorageCoordinator.h"
#include "TestTempPath.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Backing = pom2::Block512Backing;
namespace {
std::string readNote(const fs::path& folder)
{
    std::ifstream f(folder / "note.txt", std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
class Interleave final : public Backing::CommitBarrier {
public:
    std::shared_ptr<Backing::CommitBarrier> previous;
    std::function<void()> inject;
    void wait() override { if (previous) previous->wait(); inject(); }
    void complete() override { if (previous) previous->complete(); }
};
class RacingFolder final : public ProDOSHardDiskCard {
public:
    RacingFolder() : ProDOSHardDiskCard(5) {}
    std::function<void(int)> duringCommit;
    int captures = 0;
    bool dirtyAtEject = false;
    double finalEjectMs = 0;
    bool prepareEjectBay(int bay, Backing::PendingWriteBack& out, std::string& error) override
    {
        const bool result = ProDOSHardDiskCard::prepareEjectBay(bay, out, error);
        if (result && out.valid) {
            const int round = ++captures;
            auto barrier = std::make_shared<Interleave>();
            if (out.commitTicket) barrier->previous = out.commitTicket->barrier;
            barrier->inject = [this, round] { duringCommit(round); };
            // Replacing the ticket would complete its predecessor too early.
            if (!out.commitTicket) out.commitTicket = std::make_shared<Backing::CommitTicket>();
            out.commitTicket->barrier = std::move(barrier);
        }
        return result;
    }
    bool ejectBay(int bay) override
    {
        dirtyAtEject = bayInfo(bay).hasUnsavedChanges;
        const auto start = std::chrono::steady_clock::now();
        const bool result = ProDOSHardDiskCard::ejectBay(bay);
        finalEjectMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        return result;
    }
};
void exercise(int racingWrites, bool all = false)
{
    const fs::path folder = pom2test::tempPath(std::string(all ? "pom2-folder-eject-all-" : "pom2-folder-eject-race-") + std::to_string(racingWrites));
    fs::create_directories(folder);
    { std::ofstream f(folder / "note.txt", std::ios::binary); f << "00000"; }
    std::vector<uint8_t> image;
    const auto built = pom2::buildVolumeFromFolder(folder.string(), "HOST", image);
    assert(built.ok && built.filesIncluded == 1);
    EmulationController controller(AudioDevice::OutputMode::Offline);
    pom2::StorageCoordinator storage;
    pom2::Settings settings; settings.setReadOnly(true);
    auto card = std::make_unique<RacingFolder>();
    auto* live = card.get();
    {
        auto state = controller.lockState();
        state.memory().slotBus().plug(5, std::move(card));
    }
    assert(storage.mountBlockBytes(controller, settings, 5, std::move(image),
        "[host folder] " + folder.string(), folder.string()).ok);
    assert(storage.setMediaBayWriteBack(controller, settings, 5, 0, true).ok);
    size_t data = 0;
    {
        auto state = controller.lockState();
        // Directory block 2, after its volume header; one seedling file.
        const size_t entry = 2 * 512 + 4 + 39;
        assert((live->backing().readByte(entry) & 15) == 4);
        data = (live->backing().readByte(entry + 0x11) |
            (live->backing().readByte(entry + 0x12) << 8)) * 512u;
        live->backing().writeByte(data, 'A');
    }
    live->duringCommit = [&](int round) {
        // A locked commit is a bug, not a timing-dependent test deadlock.
        std::unique_lock<std::mutex> state(controller.stateMutex(), std::try_to_lock);
        assert(state.owns_lock() && "phase 2 must release the machine lock");
        assert(!live->bayInfo(0).hasUnsavedChanges);
        if (round <= racingWrites) {
            live->backing().writeByte(data + round, static_cast<uint8_t>('A' + round));
            assert(live->bayInfo(0).hasUnsavedChanges);
        }
    };
    std::string notice;
    if (all) {
        const auto result = storage.ejectAllMedia(controller, settings);
        assert(result.changed && result.failures.empty());
    } else {
        const auto result = storage.ejectMediaBay(controller, settings, 5, 0);
        assert(result.ok); notice = result.notice;
    }
    assert(live->captures == (all ? 1 : racingWrites < 4 ? racingWrites + 1 : 4));
    assert(live->dirtyAtEject == (all ? racingWrites > 0 : racingWrites == 4));
    assert(!live->bayInfo(0).loaded && !live->bayInfo(0).hasUnsavedChanges);
    std::string expected = "A0000";
    for (int i = 1; i <= racingWrites; ++i) expected[i] = static_cast<char>('A' + i);
    const auto actual = readNote(folder);
    if (actual != expected) {
        std::fprintf(stderr, "phase-2 writes=%d: expected %s, got %s (notice: %s)\n",
            racingWrites, expected.c_str(), actual.c_str(), notice.c_str());
        std::abort();
    }
    assert(!fs::exists(folder / ".pom2-conflicts"));
    std::printf("phase-2 writes=%d: %s, captures=%d, final locked eject=%.3f ms\n",
        racingWrites, actual.c_str(), live->captures, live->finalEjectMs);
    fs::remove_all(folder);
}
}
int main() { for (int writes : {0, 1, 3, 4}) exercise(writes); exercise(1, true); }
