// POM2 — GPL-3.0-or-later
#pragma once
#include "SessionArchive.h"

class M6502;
class Memory;
class SlotPeripheral;
namespace pom2 {
// Core/card layer: call under the machine state lock, after the controller
// adapter checks cassette eligibility (SessionFiles). No I/O.
bool captureSession(SessionArchive& out, M6502& cpu, Memory& memory,
                    uint32_t machineId, std::string& error);
bool restoreSession(const SessionArchive& session, M6502& cpu, Memory& memory,
                    uint32_t machineId, std::string& error);
/// The refusal for a card in the //e AUXILIARY slot (SlotBus slot 0 — the
/// Premium SoftCard //e is the only one). Shared with SessionMachine's
/// outgoing check, which has to refuse BEFORE the teardown: `SlotBus::clear()`
/// empties slot 0 too, so `restoreSession`'s own guard ran on a bus the card
/// had already been destroyed on and passed — the documented "no unsupported
/// device is silently omitted" (docs/session-format.md) was broken for exactly
/// the one device the refusal names.
std::string slotZeroRefusal(SlotPeripheral* card);
}
