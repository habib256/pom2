// POM2 — GPL-3.0-or-later
#include "BrowserSessionStore.h"
#include "AtomicFileReplace.h"
#include "EmulationController.h"
#include "PersistentFs.h"
#include "SessionFiles.h"
#include <filesystem>
#include <fstream>
#include <utility>

namespace pom2 {
BrowserSessionStore::BrowserSessionStore(std::string directory)
{
    sessionPaths_[0] = (std::filesystem::path(directory) / "browser-resume.pom2session").string();
    sessionPaths_[1] = (std::filesystem::path(directory) / "browser-resume-next.pom2session").string();
    markerPath_ = (std::filesystem::path(directory) / "browser-resume.enabled").string();
}
void BrowserSessionStore::initialize(EmulationController& ctrl)
{
    if (initialized_) return;
    initialized_ = true;
    std::error_code ec;
    if (!std::filesystem::exists(markerPath_, ec) && !ec) return;
    enabled_ = true;
    std::ifstream marker(markerPath_, std::ios::binary);
    std::array<char, 16> data{};
    marker.read(data.data(), data.size());
    const std::string text(data.data(), size_t(marker.gcount()));
    if (marker.bad() || (text != "running\n0\n" && text != "running\n1\n" &&
                        text != "stopped\n0\n" && text != "stopped\n1\n")) {
        error_ = "Invalid browser resume marker.";
        suspended_ = true;
        return;
    }
    active_ = text[text.size() - 2] - '0';
    if (!loadSessionFile(ctrl, sessionPaths_[active_], error_)) {
        // The marked generation is unreadable (damaged, or refused after a
        // firmware or format change): fall back to the other one, which is
        // the previous good checkpoint. Either way the next checkpoint must
        // overwrite the generation that FAILED — "Retry" used to write
        // 1 - active_, i.e. replace the one generation never tried.
        const int other = active_ == 0 ? 1 : 0;
        std::string olderError;
        std::error_code oec;
        if (std::filesystem::exists(sessionPaths_[other], oec) &&
            loadSessionFile(ctrl, sessionPaths_[other], olderError)) {
            active_ = other;
            error_.clear();
        } else {
            active_ = other;
            suspended_ = true;
            return;
        }
    }
    ctrl.setMode(text[0] == 'r' ? EmulationController::Mode::Running : EmulationController::Mode::Stopped);
}
bool BrowserSessionStore::setEnabled(EmulationController& ctrl, bool enabled)
{
    // A click that beats the startup restore (main.cpp waits out the boot
    // countdown first) used to mark the store initialised WITHOUT reading the
    // marker: the stored machine was never restored, and with active_ still
    // -1 the next checkpoint overwrote the COMMITTED generation, breaking the
    // write-the-inactive-file-first rule. Read it first.
    if (!initialized_) initialize(ctrl);
    initialized_ = true;
    if (!enabled) {
        std::error_code ec;
        std::filesystem::remove(markerPath_, ec);
        if (ec) { error_ = "Cannot disable browser resume: " + ec.message(); return false; }
        enabled_ = false; suspended_ = false; error_.clear();
        markPersistentStateDirty();
        return true;
    }
    enabled_ = true; suspended_ = false; error_.clear();
    checkpoint(ctrl, ctrl.getMode() == EmulationController::Mode::Running);
    return !suspended_;
}
void BrowserSessionStore::checkpoint(EmulationController& ctrl, bool running)
{
    if (!initialized_ || !enabled_ || suspended_) return;
    // Write the inactive generation, then atomically publish its mode and
    // index. A failed marker write cannot replace the last committed session.
    const int next = active_ == 0 ? 1 : 0;
    // Collect a floppy/block autosave the executor has already finished. The
    // browser's executor completes inline, but the medium stays "write-back
    // pending" until the next poll, and the capture refuses a pending medium:
    // a heartbeat or pagehide landing in that one-frame window suspended
    // automatic resume for good. Twice: a poll may start a capture as well.
    ctrl.pollMediaWriteBacks();
    ctrl.pollMediaWriteBacks();
    if (!saveSessionFile(ctrl, sessionPaths_[next], error_)) { suspended_ = true; return; }
    const std::string mode = std::string(running ? "running\n" : "stopped\n") + char('0' + next) + '\n';
    std::error_code ec;
    if (!writeFileAtomic(markerPath_, reinterpret_cast<const uint8_t*>(mode.data()), mode.size(), ec)) {
        error_ = "Cannot store browser resume marker: " + ec.message();
        suspended_ = true;
    } else active_ = next;
    markPersistentStateDirty();
}
}
