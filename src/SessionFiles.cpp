// POM2 — GPL-3.0-or-later
#include "SessionFiles.h"
#include "AtomicFileReplace.h"
#include "EmulationController.h"
#include "SessionSnapshot.h"
#include "SnapshotIO.h"

#include <filesystem>
#include <system_error>

namespace pom2 {
namespace {
bool controllerDevicesSupported(EmulationController& ctrl, std::string& error)
{
    for (int drive = 0; drive < 3; ++drive)
        if (ctrl.disk35(drive).isLoaded()) {
            error = "sessions do not capture onboard 3.5-inch media; eject it first";
            return false;
        }
    if (ctrl.cassette().hasLoadedTape() || ctrl.cassette().hasRecordedTape() ||
        ctrl.cassette().isRecordingArmed()) {
        error = "sessions do not capture cassette tapes; eject/clear the tape first";
        return false;
    }
    return true;
}
}
bool saveSessionFile(EmulationController& ctrl, const std::string& path, std::string& error)
{
    for (unsigned char c : path)
        if (c < 32 || c == 127) { error = "session path contains a control byte"; return false; }
    // Check the canonical destination, not a symlink's reassuring suffix.
    std::error_code ec;
    const auto target = std::filesystem::weakly_canonical(path, ec);
    if (ec || target.extension() != ".pom2session") {
        error = "session path must resolve to a .pom2session file"; return false;
    }
    SessionArchive captured;
    {
        auto state = ctrl.lockState();
        if (!controllerDevicesSupported(ctrl, error) ||
            !captureSession(captured, state.cpu(), state.memory(), ctrl.machineId(), error)) return false;
    }
    std::vector<uint8_t> bytes;
    if (!encodeSessionArchive(captured, bytes, error)) return false;
    if (!writeFileAtomic(target.string(), bytes.data(), bytes.size(), ec)) {
        error = "session write failed: " + ec.message(); return false;
    }
    return true;
}
bool loadSessionFile(EmulationController& ctrl, const std::string& path, std::string& error)
{
    std::vector<uint8_t> bytes;
    SessionArchive prepared;
    if (!readSnapshotFileBytes(path, bytes, error) || !decodeSessionArchive(bytes, prepared, error)) return false;
    auto state = ctrl.lockState();
    if (!controllerDevicesSupported(ctrl, error) ||
        !restoreSession(prepared, state.cpu(), state.memory(), ctrl.machineId(), error)) return false;
    ctrl.noteTimeJump();
    ctrl.rewind().clear();
    return true;
}
}
