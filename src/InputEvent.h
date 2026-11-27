// POM2 — GPL-3.0-or-later
//
// One input from outside the machine — a key, a paste, a paddle, a mouse
// move, a reset, a change of speed — as a value. Host input recording
// (EmulationController_Input.cpp) stamps each one with the CPU cycle it was
// applied at, so a recording replays to the cycle; Memory's input
// forwarders hand them to an `InputTap` while a recording or a playback
// owns the machine's inputs.
#pragma once

#include <cstdint>
#include <string>

namespace pom2 {

struct InputEvent {
    enum class Kind : uint8_t {
        Key = 1,          // a = the key byte, as given to Memory::queueKey
        KeyHeld,          // a = host key token, b = down
        ReleaseHeld,
        Paste,            // text, Memory::pasteText
        PasteRaw,         // text, Memory::pasteRawKeys
        PasteStream,      // text, Memory::pasteKeyStream (a terminal)
        Paddle,           // a = index, b = value
        PaddleButton,     // a = index, b = down
        OpenApple,        // b = down
        SolidApple,       // b = down
        Shift,            // b = down
        Control,          // b = down
        Joyport,          // a = jack, b = contacts
        Mouse,            // a = x counter, b = y counter, c = button
        SoftReset,
        HardReset,
        ColdBoot,
        Clock,            // hz, stockHz, machineHz, ratio: an accelerator or speed change
        Checkpoint,       // hash: the machine at this cycle, for the replay to check
        End,              // text: why the recording ended
        Start,            // hash: the machine; when: the host's UTC second; a: its UTC offset (s)
    };
    Kind kind = Kind::Key;
    int32_t a = 0, b = 0, c = 0;
    double hz = 0.0, stockHz = 0.0, machineHz = 0.0, ratio = 0.0;
    uint64_t hash = 0;
    int64_t when = 0;
    std::string text;
};

/// An input and the CPU cycle it was applied at: one entry of a recording.
struct TimedInput {
    uint64_t cycle = 0;
    InputEvent event;
};

/// Receives the host inputs Memory would otherwise apply at once. Returns
/// true when it took the input (queued it for a slice boundary, or dropped
/// it during a playback): the caller then does nothing else.
class InputTap {
public:
    virtual ~InputTap() = default;
    virtual bool takeInput(const InputEvent& e) = 0;
};

} // namespace pom2
