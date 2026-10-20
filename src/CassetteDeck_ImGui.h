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

// CassetteDeck_ImGui — procedural late-80s consumer cassette deck widget,
// ported from POM1. Draws the chassis, counter window, cassette
// compartment, brand strip and piano-key transport buttons entirely with
// ImDrawList. Owns a small mechanical transport state machine
// (STOP / PLAY / REC / REW / FF + PAUSE overlay) with realistic interlock
// rules: REC alone acts as REC+PLAY, PAUSE only latches on Play/Rec,
// STOP releases everything, REW/FF release PLAY.
//
// Transport actions go through CassetteDevice. No EmulationSnapshot
// dependency — the deck reads CassetteDevice state directly each frame
// (under the caller's stateMutex).

#ifndef POM2_CASSETTE_DECK_IMGUI_H
#define POM2_CASSETTE_DECK_IMGUI_H

#include "imgui.h"

#include <cstdint>
#include <string>

class CassetteDevice;
class EmulationController;

namespace pom2 {

class CassetteDeck_ImGui {
public:
    enum class Transport {
        Stopped,
        Playing,
        Recording,
        Rewinding,
        FastForwarding,
    };

    struct FrameResult {
        std::string statusMessage;
    };

    /// Lock-free snapshot of CassetteDevice state, populated each frame
    /// from the caller (under stateMutex). Avoids holding the device lock
    /// while we redraw 30 widgets and call ImGui per-key.
    struct DeckSnapshot {
        bool   loadedTape           = false;
        bool   playbackActive       = false;
        bool   playbackArmed        = false;
        bool   rewinding            = false;
        bool   audioAvailable       = false;
        bool   playbackPaused       = false;
        bool   audioStreamMode      = false;
        double playbackPositionSec  = 0.0;
        double playbackTotalSec     = 0.0;
        size_t loadedTransitions    = 0;
        size_t recordedTransitions  = 0;
        float  volume               = 1.0f;
        std::string loadedTapePath;
        std::string loadInfo;
    };

    CassetteDeck_ImGui() = default;

    /// Draw the deck inside `title`. Closes the window when `open` flips
    /// to false. Returns user-facing events from this frame.
    FrameResult render(const char* title,
                       bool& open,
                       EmulationController* emulation,
                       const DeckSnapshot& snap,
                       float deltaSeconds);

    /// Reset visual state (STOP, counter → 000). Called on hard reset.
    void reset();

    // Tape load/save dialog state (same pattern as DiskController_ImGui).
    // Menu-bar "Load tape…" / "Save tape…" shortcuts flip these directly.
    bool        loadDialogOpen = false;
    bool        saveDialogOpen = false;
    std::string dialogPath;

private:
    Transport transport_   = Transport::Stopped;
    bool      paused_      = false;
    uint32_t  counter_     = 0;
    double    counterAccum_ = 0.0;
    float     hubAngle_    = 0.0f;
    double    rewEndsAt_   = 0.0;
    double    wallClock_   = 0.0;
    float     volume_      = 1.0f;
    bool      volumeSynced_ = false;
    bool      muted_       = false;
    float     preMuteVolume_ = 1.0f;

    void drawChassis      (ImDrawList* dl, ImVec2 p0, float s) const;
    void drawSlimLineBadge(ImDrawList* dl, ImVec2 p0, float s) const;
    enum class LampMode { Off, Armed, Data, Rec };
    void drawCounter      (ImDrawList* dl, ImVec2 p0, float s,
                           const char* resetId, bool& resetClicked,
                           LampMode lamp);
    void drawCassetteWindow(ImDrawList* dl, ImVec2 p0, float s,
                            const DeckSnapshot& snap) const;
    void drawBrandStrip   (ImDrawList* dl, ImVec2 p0, float s) const;
    void drawButtonLabels (ImDrawList* dl, ImVec2 p0, float s) const;
    bool drawPianoKey     (ImDrawList* dl, ImVec2 p0, float s,
                           float cx, float cy, const char* id,
                           const char* glyph, bool engaged, bool disabled,
                           bool* heldOut = nullptr);

    std::string onRecord (EmulationController* emu, const DeckSnapshot& snap);
    std::string onPlay   (EmulationController* emu, const DeckSnapshot& snap);
    std::string onRewind (EmulationController* emu, const DeckSnapshot& snap);
    std::string onFForward(EmulationController* emu, const DeckSnapshot& snap);
    std::string onStop   (EmulationController* emu);
    std::string onPause  (EmulationController* emu);
    std::string onEject  (EmulationController* emu, const DeckSnapshot& snap);

    bool playKeyEngaged() const {
        return transport_ == Transport::Playing || transport_ == Transport::Recording;
    }
    bool recKeyEngaged()   const { return transport_ == Transport::Recording; }
    bool rewKeyEngaged()   const { return transport_ == Transport::Rewinding; }
    bool ffKeyEngaged()    const { return transport_ == Transport::FastForwarding; }
    bool stopKeyEngaged()  const { return transport_ == Transport::Stopped && !paused_; }
    bool pauseKeyEngaged() const { return paused_; }

    void advanceCounter(float deltaSeconds, const DeckSnapshot& snap);
    void syncWithSnapshot(const DeckSnapshot& snap);
};

} // namespace pom2

#endif // POM2_CASSETTE_DECK_IMGUI_H
