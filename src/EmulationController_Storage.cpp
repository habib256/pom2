// POM2 — GPL-3.0-or-later
#include "EmulationController.h"
#include "Block512Backing.h"
#include "SlotBus.h"
#include "BlockWriteBackExecutor.h"

void EmulationController::pollBlockWriteBacks()
{
    auto state = lockState();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot)
        if (auto* card = state.memory().slotBus().peripheral(slot))
            for (auto* backing : card->blockBackings())
                if (backing) {
                    backing->setWriteBackExecutor(pom2::blockWriteBackExecutor());
                    backing->pollWriteBack();
                }
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
