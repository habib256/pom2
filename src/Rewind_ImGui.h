// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Rewind_ImGui — transport bar for the MicroM8-style rewind feature.
//
// Surfaces the RewindBuffer ring as a scrubbable timeline: a Record toggle,
// a position slider, step-back/step-forward, a press-and-hold "rewind live"
// button (the signature MicroM8 gesture — the screen replays backwards while
// held), and Play/Resume. All state changes go through the Emulation
// controller's rewind transport (which parks the worker so restores aren't
// overrun). The panel owns only view state (cursor + scrub flag); the ring
// and the machine live in the controller.

#ifndef POM2_REWIND_IMGUI_H
#define POM2_REWIND_IMGUI_H

#include <cstddef>
#include <string>

class EmulationController;

namespace pom2 {

class Rewind_ImGui {
public:
    struct FrameResult {
        std::string statusMessage;   // shown in the status bar when non-empty
        /// Non-zero when the user moved the "history (s)" slider this frame.
        /// The panel has no Settings of its own; the host persists it under
        /// `rewind_history_seconds` and re-applies it on the next launch.
        /// Without that the ring silently reset to its 30 s default every
        /// session, which is the one rewind setting a user actually tunes.
        int historySecondsChanged = 0;
    };

    Rewind_ImGui() = default;

    /// Draw the rewind bar inside `title`. Closes the window when `open`
    /// flips to false. `deltaSeconds` paces the hold-to-rewind speed.
    FrameResult render(const char* title, bool& open,
                       EmulationController& ctrl, float deltaSeconds);

    /// Force-exit any in-progress scrub (e.g. on reset / profile switch).
    void cancelScrub() { scrubbing_ = false; }

    bool isScrubbing() const { return scrubbing_; }

    /// Drive a live rewind step from outside the panel (e.g. a held hotkey).
    /// Begins scrubbing if needed and steps the cursor back by `frames`.
    void holdRewind(EmulationController& ctrl, size_t frames);
    /// Release the external hold: resume live from the current cursor.
    void releaseHold(EmulationController& ctrl);

private:
    void beginScrubIfNeeded(EmulationController& ctrl);
    void seekTo(EmulationController& ctrl, long index);
    /// Re-read the controller's scrub state before acting on our own copy.
    /// The panel is not the only way out of a scrub — see
    /// `EmulationController::rewindScrubbing`.
    void syncScrub(EmulationController& ctrl);

    bool   scrubbing_   = false;
    size_t cursor_      = 0;     // viewed frame index while scrubbing
    int    rewindSpeed_ = 1;     // history frames per render frame when held
};

}  // namespace pom2

#endif  // POM2_REWIND_IMGUI_H
