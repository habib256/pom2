// POM2 — GPL-3.0-or-later
#pragma once
#include <array>
#include <string>
class EmulationController;
namespace pom2 {
// UI-thread lifecycle for optional browser checkpoints. Unsupported or failed
// restores suspend writes, preserving the last checkpoint for recovery.
class BrowserSessionStore {
public:
    explicit BrowserSessionStore(std::string directory = "/persistent");
    void initialize(EmulationController& controller);
    /// A page opened from a link (?sNdM=) or embedded (?embed=1) asked for
    /// ITS setup: read the visitor's resume state without restoring it, and
    /// hold the checkpoints, so the saved machine is neither loaded over the
    /// link nor overwritten by it. "Retry" in the menu checkpoints the
    /// current machine on purpose (bug hunt 2026-11-19, round 2).
    void initializeWithoutRestore();
    bool setEnabled(EmulationController& controller, bool enabled);
    void checkpoint(EmulationController& controller, bool running);
    bool enabled() const { return enabled_; }
    bool suspended() const { return suspended_; }
    const std::string& error() const { return error_; }
private:
    std::array<std::string, 2> sessionPaths_;
    std::string markerPath_, error_;
    int active_ = -1;
    bool initialized_ = false, enabled_ = false, suspended_ = false;
};
}
