// POM2 — GPL-3.0-or-later
#pragma once
#include "SessionArchive.h"

class M6502;
class Memory;
namespace pom2 {
// Core/card layer: call under the machine state lock, after the controller
// adapter checks cassette eligibility (SessionFiles). No I/O.
bool captureSession(SessionArchive& out, M6502& cpu, Memory& memory,
                    uint32_t machineId, std::string& error);
bool restoreSession(const SessionArchive& session, M6502& cpu, Memory& memory,
                    uint32_t machineId, std::string& error);
}
