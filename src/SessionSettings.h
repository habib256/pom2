// POM2 — GPL-3.0-or-later
#pragma once
// What a quit writes to state.cfg from the live machine's serial ports and
// No-Slot Clock — a frontend policy (it reads Settings), kept out of
// SessionMachine, which is runtime.
class SlotBus;
namespace pom2 {
class Settings;
class NoSlotClock;
// Writes the serial ports' (per slot + the legacy primary-port keys) and the
// No-Slot Clock's settings from the live machine. Writes NOTHING when the
// machine is a loaded session's: its serial ports and clock come from the
// session file, and quitting used to put them in state.cfg in place of the
// user's own (telnet listener, port, clock) — the rule the slot and media
// keys already follow (docs/session-format.md).
void persistSerialAndClockSettings(Settings& settings, SlotBus& bus,
                                   const NoSlotClock& clock, bool sessionMachineActive);
}
