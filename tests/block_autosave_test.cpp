// POM2 — GPL-3.0-or-later
#include "BlockWriteBackExecutor.h"
#include "EmulationController.h"
#include "ProDOSHardDiskCard.h"
#include "CffaCard.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

namespace fs = std::filesystem;
using B = pom2::Block512Backing;
static void writeFile(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary); f.write((const char*)data.data(), data.size());
    assert(f.good());
}
static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::array<uint8_t, 512> pattern(uint8_t v) {
    std::array<uint8_t, 512> bytes; bytes.fill(v); return bytes;
}
int main() {
    auto dir = fs::temp_directory_path() / ("pom2-autosave-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    auto path = dir / "disk.hdv";
    writeFile(path, std::vector<uint8_t>(2048));
    {
        B b;
        b.setWriteBackEnabled(true);
        b.setWriteBackExecutor(pom2::blockWriteBackExecutor());
        assert(b.loadImage(path.string()));
        assert(b.writeBlock(1, pattern(0x11).data()));
        // Hold a predecessor to make the in-flight window deterministic.
        auto held = b.takeWriteBack();
        b.restoreDirty(held.dirtyIndices);
        auto saving = b.pollWriteBack(true);
        assert(saving && !saving->ready());
        assert(b.hasUnsavedChanges());
        assert(std::string(b.persistenceState()) == "saving");
        assert(readFile(path)[512] == 0);
        assert(b.writeBlock(1, pattern(0x22).data()));
        std::string error;
        assert(B::commitWriteBack(std::move(held), error));
        assert(saving->wait().ok);
        b.pollWriteBack();
        assert(b.hasUnsavedChanges()); // completion must not clear 0x22
        assert(readFile(path)[512] == 0x11);
        assert(b.pollWriteBack(true)->wait().ok);
        b.pollWriteBack();
        assert(!b.hasUnsavedChanges());
        assert(readFile(path)[512] == 0x22);

        // An eject/explicit flush overtaking a worker must commit last.
        assert(b.writeBlock(1, pattern(0x33).data()));
        held = b.takeWriteBack(); b.restoreDirty(held.dirtyIndices);
        saving = b.pollWriteBack(true);
        assert(b.writeBlock(1, pattern(0x44).data()));
        auto finalPayload = b.takeWriteBack();
        auto final = pom2::blockWriteBackExecutor()->submit(std::move(finalPayload), {});
        assert(!final->ready());
        assert(B::commitWriteBack(std::move(held), error));
        assert(final->wait().ok);
        assert(readFile(path)[512] == 0x44);
        b.pollWriteBack();
        assert(!b.hasUnsavedChanges());

        // A failed host write stays dirty and visible; an explicit retry
        // saves the newest bytes and clears the error.
        assert(b.writeBlock(2, pattern(0x55).data()));
        fs::rename(path, dir / "hidden.hdv");
        assert(!b.pollWriteBack(true)->wait().ok);
        b.pollWriteBack();
        assert(b.hasUnsavedChanges());
        assert(std::string(b.persistenceState()) == "error");
        assert(!b.persistenceError().empty());
        fs::rename(dir / "hidden.hdv", path);
        assert(b.writeBlock(2, pattern(0x66).data()));
        assert(b.pollWriteBack(true)->wait().ok);
        b.pollWriteBack();
        assert(std::string(b.persistenceState()) == "saved");
        assert(b.persistenceError().empty());
        assert(readFile(path)[1024] == 0x66);

        // Automatic retry, with no explicit sync after the file returns.
        assert(b.writeBlock(2, pattern(0x67).data()));
        fs::rename(path, dir / "hidden.hdv");
        assert(!b.pollWriteBack(true)->wait().ok); b.pollWriteBack();
        fs::rename(dir / "hidden.hdv", path);
        const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
        while (b.hasUnsavedChanges()) {
            b.pollWriteBack();
            assert(std::chrono::steady_clock::now() < retryDeadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        assert(readFile(path)[1024] == 0x67);
        assert(b.persistenceError().empty());

        // Discarded captures cannot strand the commit chain.
        assert(b.writeBlock(3, pattern(0x77).data()));
        { auto abandoned = b.takeWriteBack(); b.restoreDirty(abandoned.dirtyIndices); }
        assert(b.pollWriteBack(true)->wait().ok);
        b.pollWriteBack();
        assert(readFile(path)[1536] == 0x77);
        b.setWriteBackEnabled(false);
        assert(b.writeBlock(3, pattern(0x88).data()));
        assert(!b.pollWriteBack(true));
        assert(std::string(b.persistenceState()) == "disabled");
        assert(readFile(path)[1536] == 0x77);
    }
    // Multiple mounts of one image must merge disjoint dirty blocks rather
    // than race their full-file read/modify/replace transactions.
    {
        B first, second;
        for (auto* b : {&first, &second}) {
            b->setWriteBackEnabled(true);
            b->setWriteBackExecutor(pom2::blockWriteBackExecutor());
            assert(b->loadImage(path.string()));
        }
        for (uint8_t n = 1; n <= 4; ++n) {
            assert(first.writeBlock(0, pattern(n).data()));
            assert(second.writeBlock(1, pattern(n + 10).data()));
            auto a = first.pollWriteBack(true), b = second.pollWriteBack(true);
            assert(a->wait().ok && b->wait().ok);
            first.pollWriteBack(); second.pollWriteBack();
            auto actual = readFile(path);
            assert(actual[0] == n && actual[512] == n + 10);
        }
    }
    // A 2IMG envelope is preserved by background writes too.
    auto two = dir / "disk.2mg";
    std::vector<uint8_t> envelope(64 + 1024 + 16, 0);
    envelope[0]='2'; envelope[1]='I'; envelope[2]='M'; envelope[3]='G';
    envelope[8]=64; envelope[12]=1; envelope[24]=64; envelope[29]=4;
    envelope.back()=0xAB;
    writeFile(two, envelope);
    {
        B b; b.setWriteBackEnabled(true);
        b.setWriteBackExecutor(pom2::blockWriteBackExecutor());
        assert(b.loadImage(two.string()));
        assert(b.writeBlock(0, pattern(0x99).data()));
        assert(b.pollWriteBack(true)->wait().ok); b.pollWriteBack();
        auto actual = readFile(two);
        assert(actual.size() == envelope.size());
        assert(std::equal(actual.begin(), actual.begin()+64, envelope.begin()));
        assert(actual[64] == 0x99 && actual.back() == 0xAB);
    }
    // All three controller routes persist while paused, without a sync,
    // eject, reset or destructor. The test inspects the host files directly.
    {
        EmulationController c;
        c.start(); c.stop();
        auto hdv = std::make_unique<ProDOSHardDiskCard>(5);
        auto cffa = std::make_unique<pom2::CffaCard>(7);
        auto sp = std::make_unique<pom2::SmartPortCard>(6);
        auto unit = std::make_unique<pom2::SmartPortHdvUnit>();
        auto ataPath = dir / "ata.hdv", spPath = dir / "smartport.hdv";
        writeFile(ataPath, std::vector<uint8_t>(2048));
        writeFile(spPath, std::vector<uint8_t>(2048));
        assert(hdv->loadImage(path.string())); hdv->setWriteBackEnabled(true);
        assert(cffa->loadImage(ataPath.string())); cffa->setWriteBackEnabled(true);
        assert(unit->loadImage(spPath.string())); unit->setWriteBackEnabled(true);
        assert(hdv->backing().writeBlock(0, pattern(0xB1).data()));
        assert(cffa->blockBacking()->writeBlock(0, pattern(0xB2).data()));
        assert(unit->writeBlock(0, pattern(0xB3).data()));
        sp->setUnit(0, std::move(unit));
        {
            auto st = c.lockState();
            st.memory().slotBus().plug(5, std::move(hdv));
            st.memory().slotBus().plug(7, std::move(cffa));
            st.memory().slotBus().plug(6, std::move(sp));
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (readFile(path)[0]!=0xB1 || readFile(ataPath)[0]!=0xB2 || readFile(spPath)[0]!=0xB3) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::string error;
        assert(c.syncBlockMedia(error));
        assert(error.empty());
        assert(c.blockPersistence().size() == 3);
        for (auto& status : c.blockPersistence()) assert(status.state == "saved");
        // A synchronizing API client waits outside stateMutex. Mutate the
        // guest while its host commit is deliberately blocked, then check
        // that the later bytes remain pending for the next synchronization.
        B::PendingWriteBack held;
        {
            auto st = c.lockState();
            auto* b = st.memory().slotBus().peripheral(5)->blockBackings()[0];
            assert(b->writeBlock(0, pattern(0xCC).data()));
            held = b->takeWriteBack(); b->restoreDirty(held.dirtyIndices);
        }
        auto sync = std::async(std::launch::async, [&] {
            std::string failure; return c.syncBlockMedia(failure);
        });
        bool captured = false;
        for (int attempt = 0; attempt < 100 && !captured; ++attempt) {
            {
                auto st = c.lockState();
                auto* b = st.memory().slotBus().peripheral(5)->blockBackings()[0];
                captured = std::string(b->persistenceState()) == "saving";
                if (captured) assert(b->writeBlock(0, pattern(0xDD).data()));
            }
            if (!captured) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(captured);
        assert(sync.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
        assert(B::commitWriteBack(std::move(held), error));
        assert(sync.get());
        assert(readFile(path)[0] == 0xCC);
        assert(c.syncBlockMedia(error));
        assert(readFile(path)[0] == 0xDD);

    }
    fs::remove_all(dir);
}
