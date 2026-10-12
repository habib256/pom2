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

// Keyboard_ImGui — the Apple //e keyboard as a clickable picture.
//
// A photo of the real thing (`pic/Keyboard_AppleIIe.jpeg`) with one hotspot
// per cap, measured off the image itself rather than drawn as a grid of
// buttons. The point is that the legends are the REAL legends — a user
// hunting for the key that types `[` on a //e finds it where it is on the
// machine, including the ones a modern host keyboard has nowhere to put
// (Open-Apple, Solid-Apple, the //e's own Reset).
//
// Two jobs, and only two: hit-test the pointer against the layout, and hold
// the modifier latches. What a key MEANS to the emulated machine belongs to
// MainWindow — this class returns a `KeyHotspot*` and the latch state and
// stops there, so it needs no emulator headers and no lock.
//
// Latches, not chords: a mouse has one pointer, so Ctrl+Reset or
// Open-Apple+Ctrl+Reset cannot be clicked simultaneously. Shift / Control /
// Caps Lock / Open-Apple / Solid-Apple therefore TOGGLE and stay down until
// used or clicked again — the same contract as every on-screen keyboard and
// as the //e's own mechanically-latching Caps Lock.

#ifndef POM2_KEYBOARD_IMGUI_H
#define POM2_KEYBOARD_IMGUI_H

#include "AppleIIeKeyboardLayout.h"
#include "KeyboardSkin.h"

#include <string>

namespace pom2 {

class Keyboard_ImGui
{
public:
    /// Which latches are down. Shift and Control are one-shot (cleared by
    /// the next character); Caps Lock and the two Apple keys stay until
    /// clicked again — Open-Apple must be holdable across a Reset click,
    /// which is the whole point of the //e's Open-Apple+Ctrl+Reset boot.
    struct Latches {
        bool shift      = false;
        bool control    = false;
        bool caps       = true;   ///< Down at boot, like the real machine's.
        bool openApple  = false;
        bool solidApple = false;
    };

    /// What happened this frame. `key` is null when nothing was clicked.
    struct Event {
        const KeyHotspot* key = nullptr;
        Latches           latches;   ///< State AT the moment of the click.
    };

    /// Draw the window.
    /// @param open        caller's show-flag (title-bar close clears it).
    /// @param texture     GL texture id of the keyboard photo, 0 if it could
    ///                    not be loaded (the window then explains itself
    ///                    instead of drawing nothing).
    /// @param texW,texH   pixel size of that texture, for the aspect ratio.
    /// @param loadError   why the photo is missing, shown when texture == 0.
    Event render(bool* open, unsigned int texture, int texW, int texH,
                 const std::string& loadError, const KeyboardSkin& skin);

    /// The latches, for a caller that mirrors them elsewhere (the Apple keys
    /// are machine state: they must be pushed to $C061/$C062 every frame,
    /// not only when clicked).
    const Latches& latches() const { return latches_; }

    /// Clear the one-shot latches. Called by the host after a character key
    /// has consumed them.
    void clearOneShots() { latches_.shift = false; latches_.control = false; }

    /// Drop every latch — used when the window closes, so a latched
    /// Open-Apple cannot outlive the window that shows it as down.
    void releaseAll() { latches_.openApple = latches_.solidApple = false;
                        latches_.shift = latches_.control = false; }

private:
    Latches     latches_;
    std::string lastPressId_;   ///< For the flash-on-click highlight.
    double      lastPressTime_ = 0.0;
    bool        showHitboxes_  = false;
};

} // namespace pom2

#endif // POM2_KEYBOARD_IMGUI_H
