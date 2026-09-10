// POM2 — GPL-3.0-or-later
#include "Block512Backing.h"
#include "Logger.h"
#include <algorithm>

namespace pom2 {

void Block512Backing::collectWriteBack()
{
    if (!backgroundWrite_ || !backgroundWrite_->ready())
        return;
    const auto result = backgroundWrite_->wait();
    backgroundWrite_ = {};
    if (backgroundGeneration_ != mountGeneration_) return;
    if (result.ok) {
        for (auto [block, version] : backgroundVersions_)
            if (block < blockVersions_.size() && blockVersions_[block] == version)
                dirtyBlocks_[block] = false;
        anyDirty_ = std::find(dirtyBlocks_.begin(), dirtyBlocks_.end(), true)
                    != dirtyBlocks_.end();
        persistenceError_.clear();
    } else {
        // Dirty flags were kept throughout the commit. The next attempt
        // therefore includes every failed byte plus any subsequent edits.
        persistenceError_ = result.error;
        log().warn("HDV", "Background save failed: " + result.error);
    }
    backgroundVersions_.clear();
    nextWriteBack_ = std::chrono::steady_clock::now() +
        (result.ok ? std::chrono::seconds(1) : std::chrono::seconds(5));
}

const char* Block512Backing::persistenceState() const
{
    if (!loaded_) return "unmounted";
    if (synth_) return "manual";
    if (!persistenceError_.empty()) return "error";
    if (backgroundWrite_ && backgroundGeneration_ == mountGeneration_)
        return "saving";
    if (anyDirty_) return writeBack_ ? "pending" : "disabled";
    return "saved";
}

std::shared_ptr<Block512Backing::WriteBackOperation>
Block512Backing::pollWriteBack(bool force)
{
    collectWriteBack();
    // Host-folder synchronization has its own opt-in and conflict policy.
    if (!writeBackExecutor_ || !loaded_ || synth_ || !anyDirty_ || !writeBack_ ||
        isMediumLocked() || !supportsWriteBack_)
        return backgroundWrite_;
    const auto now = std::chrono::steady_clock::now();
    if (!force) {
        if (backgroundWrite_) return backgroundWrite_;
        if (nextWriteBack_ == std::chrono::steady_clock::time_point{})
            nextWriteBack_ = now + std::chrono::seconds(1);
        if (now < nextWriteBack_) return {};
    }
    const auto oldVersions = backgroundVersions_;
    const auto oldGeneration = backgroundGeneration_;
    auto pending = takeWriteBack();
    // Unlike eject, an autosave keeps dirty state until the file has landed.
    // Per-block versions prevent completion from clearing a newer write to
    // the same block. A later eject captures these bytes too and commits
    // after this transaction through PendingWriteBack's ordering barrier.
    restoreDirty(pending.dirtyIndices);
    backgroundVersions_.clear();
    for (auto block : pending.dirtyIndices)
        backgroundVersions_.emplace_back(block, blockVersions_[block]);
    backgroundGeneration_ = mountGeneration_;
    try {
        backgroundWrite_ = writeBackExecutor_->submit(std::move(pending), backgroundWrite_);
    } catch (const std::exception& e) {
        backgroundVersions_ = oldVersions;
        backgroundGeneration_ = oldGeneration;
        persistenceError_ = e.what();
        nextWriteBack_ = now + std::chrono::seconds(5);
        // No dirty bytes were retired, even if thread creation failed.
        return {};
    }
    return backgroundWrite_;
}

} // namespace pom2
