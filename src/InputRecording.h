// POM2 — GPL-3.0-or-later
//
// A recording of host input as a FILE (`.pom2rec`): the session the machine
// stood in when it started (its disks embedded, as a `.pom2session` holds
// them) and every input stamped with its CPU cycle, with the machine's
// fingerprint every 15 frames. Played back, it re-runs the same machine
// from the same state with the same inputs at the same cycles, and checks
// that it does. The mechanics are the controller's
// (EmulationController_Input.cpp); this file captures and loads the
// session around them and reads and writes the file — unlocked.
//
// Uses: a bug report that reproduces itself; a regression test made by
// playing a game; a demo shown without the machine.
#pragma once

#include "InputEvent.h"

#include <cstdint>
#include <string>
#include <vector>

class EmulationController;

namespace pom2 {

struct InputRecordingFile {
    std::vector<uint8_t> session;          // an encoded SessionArchive
    std::vector<TimedInput> entries;
};

bool encodeInputRecording(const InputRecordingFile& file, std::vector<uint8_t>& out);
bool decodeInputRecording(const std::vector<uint8_t>& bytes, InputRecordingFile& out,
                          std::string& error);

/// Start recording now. The session is captured in the same lock hold as
/// the recording starts, so they describe the same cycle.
bool startInputRecording(EmulationController& controller, std::string& error);

/// End the recording (or collect one that ended by itself) and write it to
/// `path`. `why` says how it ended. False with `error` when there was none
/// or the file could not be written.
bool saveInputRecording(EmulationController& controller, const std::string& path,
                        std::string& why, std::string& error);

/// Load the recording's session and play it back from there; the machine
/// runs. The outcome is in controller.inputJournalStatus() as it goes.
bool playInputRecording(EmulationController& controller, const std::string& path,
                        std::string& error);

} // namespace pom2
