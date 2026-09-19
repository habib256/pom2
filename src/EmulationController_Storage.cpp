// POM2 — GPL-3.0-or-later
#include "EmulationController.h"
#include "Block512Backing.h"
#include "SlotBus.h"
#include "BlockWriteBackExecutor.h"

namespace {
void pollBlocks(Memory& memory)
{
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot)
        if (auto* card = memory.slotBus().peripheral(slot))
            for (auto* backing : card->blockBackings())
                if (backing) {
                    backing->setWriteBackExecutor(pom2::blockWriteBackExecutor());
                    backing->pollWriteBack();
                }
}
}  // namespace

template <class Fn>
void EmulationController::forEachFloppy(Memory& memory, Fn&& fn)
{
    // Slot 0 = the //c+ on-board Sony drives, which belong to no card.
    fn(0, 0, static_cast<pom2::AutosavedMedium&>(*image35Int));
    fn(0, 1, static_cast<pom2::AutosavedMedium&>(*image35Ext));
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* card = memory.slotBus().peripheral(slot);
        if (!card) continue;
        const auto media = card->autosavedMedia();
        for (size_t bay = 0; bay < media.size(); ++bay)
            if (media[bay]) fn(slot, int(bay), *media[bay]);
    }
}

void EmulationController::pollBlockWriteBacks()
{
    auto state = lockState();
    pollBlocks(state.memory());
}

void EmulationController::pollMediaWriteBacks()
{
    auto state = lockState();
    pollBlocks(state.memory());
    if (rewindScrubbing()) return;
    forEachFloppy(state.memory(), [](int, int, pom2::AutosavedMedium& m) {
        (void)m.pollAutosave(pom2::mediaCommitExecutor(), false);
    });
}

std::vector<EmulationController::BlockPersistence>
EmulationController::blockPersistence()
{
    std::vector<BlockPersistence> result;
    auto state = lockState();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* card = state.memory().slotBus().peripheral(slot);
        if (!card) continue;
        const auto backings = card->blockBackings();
        for (size_t bay = 0; bay < backings.size(); ++bay) {
            auto* b = backings[bay];
            if (!b || !b->isLoaded()) continue;
            b->setWriteBackExecutor(pom2::blockWriteBackExecutor());
            b->pollWriteBack();
            result.push_back({slot, int(bay), b->path(), b->persistenceState(),
                              b->persistenceError(), b->hasUnsavedChanges()});
        }
    }
    return result;
}

std::vector<EmulationController::BlockPersistence>
EmulationController::floppyPersistence()
{
    std::vector<BlockPersistence> result;
    auto state = lockState();
    const bool scrubbing = rewindScrubbing();
    forEachFloppy(state.memory(),
        [&](int slot, int bay, pom2::AutosavedMedium& m) {
            if (!scrubbing) (void)m.pollAutosave(pom2::mediaCommitExecutor(), false);   // publish completions
            const auto p = m.persistence();
            if (p.loaded)
                result.push_back({slot, bay, p.path, p.state, p.error, p.pending});
        });
    return result;
}

bool EmulationController::syncBlockMedia(std::string& error)
{
    struct Wait {
        std::string path;
        std::shared_ptr<pom2::Block512Backing::WriteBackOperation> future;
    };
    std::vector<Wait> writes;
    error.clear();
    auto fail = [&](const std::string& message) {
        if (!error.empty()) error += "; ";
        error += message;
    };
    {
        auto state = lockState();
        for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
            auto* card = state.memory().slotBus().peripheral(slot);
            if (!card) continue;
            for (auto* b : card->blockBackings()) {
                if (!b || !b->isLoaded() || b->isSynthVolume()) continue;
                if (b->hasUnsavedChanges() && !b->isWriteBackEnabled()) {
                    fail(b->path() + ": persistence disabled");
                    continue;
                }
                b->setWriteBackExecutor(pom2::blockWriteBackExecutor());
                auto future = b->pollWriteBack(true);
                if (future) writes.push_back({b->path(), std::move(future)});
                else if (!b->persistenceError().empty()) fail(b->path() + ": " + b->persistenceError());
            }
        }
    }
    // No borrowed card pointers cross this boundary: eject, mount and profile
    // switching may proceed while the immutable payloads are committed.
    for (auto& write : writes) {
        const auto result = write.future->wait();
        if (!result.ok) fail(write.path + ": " + result.error);
    }
    pollBlockWriteBacks(); // publish completion; later guest writes stay pending
    return error.empty();
}

bool EmulationController::syncFloppyMedia(std::string& error)
{
    struct Wait {
        std::string path;
        std::shared_ptr<pom2::MediaCommitOperation> op;
    };
    std::vector<Wait> writes;
    error.clear();
    auto fail = [&](const std::string& message) {
        if (!error.empty()) error += "; ";
        error += message;
    };
    {
        auto state = lockState();
        if (rewindScrubbing()) {
            error = "a rewind scrub owns the machine; floppy media not synced";
            return false;
        }
        forEachFloppy(state.memory(),
            [&](int, int, pom2::AutosavedMedium& m) {
                auto op = m.pollAutosave(pom2::mediaCommitExecutor(), true);
                const auto p = m.persistence();
                if (!p.loaded) return;
                if (op) writes.push_back({p.path, std::move(op)});
                else if (p.state == "disabled")
                    fail(p.path + ": persistence disabled");
                else if (!p.error.empty())
                    fail(p.path + ": " + p.error);
            });
    }
    // Same shape as the block sync: the payloads are immutable captures, so
    // the machine keeps running while they are committed.
    for (auto& write : writes) {
        const auto result = write.op->wait();
        if (!result.ok) fail(write.path + ": " + result.error);
    }
    {
        auto state = lockState();   // publish completion (retire dirty flags)
        forEachFloppy(state.memory(), [](int, int, pom2::AutosavedMedium& m) {
            (void)m.pollAutosave(pom2::mediaCommitExecutor(), false);
        });
    }
    return error.empty();
}
