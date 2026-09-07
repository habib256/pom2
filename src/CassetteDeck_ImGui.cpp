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

// CassetteDeck_ImGui.cpp — procedural cassette deck widget. Geometry is
// expressed in a fixed 378×404 design canvas; a per-frame uniform scale
// factor maps it into the window's content region so the deck stays
// pixel-crisp at any window size. Buttons are drawn with ImDrawList and
// captured with InvisibleButton at their screen-space rects.

#include "CassetteDeck_ImGui.h"

#include "EmulationController.h"
#include "IconsFontAwesome6.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pom2 {
namespace {

// ─── Design canvas ────────────────────────────────────────────────────────
constexpr float kDesignW = 378.0f;
constexpr float kDesignH = 404.0f;

// ─── Palette — matte-black consumer tape deck, late 80s / early 90s ──────
constexpr ImU32 kChassis        = IM_COL32( 24,  24,  28, 255);
constexpr ImU32 kChassisEdgeLo  = IM_COL32(  6,   6,   8, 255);
constexpr ImU32 kChassisEdgeHi  = IM_COL32( 56,  56,  62, 255);
constexpr ImU32 kGlassDark      = IM_COL32(  8,   8,  12, 235);
constexpr ImU32 kGlassReflect   = IM_COL32(255, 255, 255,  18);
constexpr ImU32 kGlassEdgeDark  = IM_COL32(  0,   0,   0, 255);
constexpr ImU32 kBrandStrip     = IM_COL32(232, 232, 234, 255);
constexpr ImU32 kBrandStripEdge = IM_COL32(150, 150, 152, 255);
constexpr ImU32 kBrandText      = IM_COL32( 20,  20,  22, 255);
constexpr ImU32 kLabelText      = IM_COL32(198, 200, 204, 255);
constexpr ImU32 kLabelTextDim   = IM_COL32(128, 130, 134, 255);
constexpr ImU32 kButtonBody     = IM_COL32( 22,  22,  26, 255);
constexpr ImU32 kButtonEdge     = IM_COL32(  4,   4,   6, 255);
constexpr ImU32 kButtonDown     = IM_COL32(  8,   8,  10, 255);
constexpr ImU32 kButtonHi       = IM_COL32( 46,  46,  52, 255);
constexpr ImU32 kGlyph          = IM_COL32(214, 216, 220, 255);
constexpr ImU32 kGlyphDim       = IM_COL32( 90,  92,  96, 255);
constexpr ImU32 kGlyphRec       = IM_COL32(214, 216, 220, 255);
constexpr ImU32 kGlyphRecActive = IM_COL32(234,  60,  52, 255);
constexpr ImU32 kCounterBg      = IM_COL32( 10,   8,   4, 255);
constexpr ImU32 kCounterRim     = IM_COL32( 50,  50,  54, 255);
constexpr ImU32 kCounterDigit   = IM_COL32(232, 176,  72, 255);
constexpr ImU32 kBadgeBorder    = IM_COL32(200, 202, 206, 255);
constexpr ImU32 kBadgeText      = IM_COL32(210, 212, 216, 255);
constexpr ImU32 kCompartmentLip = IM_COL32(  4,   4,   6, 255);
constexpr ImU32 kHubDark        = IM_COL32(  2,   2,   4, 255);
constexpr ImU32 kHubMid         = IM_COL32( 40,  42,  48, 255);

// ─── Layout (design coordinates) ──────────────────────────────────────────
struct Rect { float x0, y0, x1, y1; };
constexpr Rect kBrandBadgeR { 306.0f,  30.0f, 360.0f,  56.0f };
constexpr Rect kCounterWinR { 148.0f,  26.0f, 212.0f,  60.0f };
constexpr Rect kRecLedR     {  22.0f,  22.0f,  46.0f,  46.0f };
constexpr Rect kCassetteR   {  18.0f,  80.0f, 360.0f, 242.0f };
constexpr Rect kBrandR      {  14.0f, 252.0f, 364.0f, 280.0f };
constexpr Rect kLabelsR     {  18.0f, 284.0f, 360.0f, 302.0f };

constexpr float kKeyW         = 47.0f;
constexpr float kKeyH         = 64.0f;
constexpr float kKeyRadius    = 5.0f;
constexpr float kKeysTop      = 308.0f;
constexpr float kKeyCenterXs[6] = { 44.0f, 102.0f, 160.0f, 218.0f, 276.0f, 334.0f };
constexpr float kCounterResetW = 10.0f;

constexpr double kCounterPlaySecPerTick = 1.5;
constexpr double kCounterWindSecPerTick = 0.15;
constexpr double kWindDurationSeconds   = 1.4;

inline ImVec2 P(ImVec2 p0, float s, float x, float y) {
    return ImVec2(p0.x + x * s, p0.y + y * s);
}
inline float S(float s, float v) { return v * s; }

void drawText(ImDrawList* dl, ImVec2 p0, float s, float x, float y,
              float fontPx, ImU32 col, const char* text)
{
    ImFont* font = ImGui::GetFont();
    if (!font || !text || !text[0]) return;
    const float fs = std::max(7.0f, fontPx * s);
    const ImVec2 pos = P(p0, s, x, y);
    dl->AddText(font, fs, pos, col, text);
}

void drawCenteredText(ImDrawList* dl, ImVec2 p0, float s, Rect r,
                      float fontPx, ImU32 col, const char* text)
{
    ImFont* font = ImGui::GetFont();
    if (!font || !text || !text[0]) return;
    const float fs = std::max(7.0f, fontPx * s);
    const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
    const float rw = (r.x1 - r.x0) * s;
    const float rh = (r.y1 - r.y0) * s;
    const ImVec2 pos(p0.x + r.x0 * s + (rw - sz.x) * 0.5f,
                     p0.y + r.y0 * s + (rh - sz.y) * 0.5f);
    dl->AddText(font, fs, pos, col, text);
}

void drawPlayGlyph(ImDrawList* dl, ImVec2 p0, float s, float cx, float cy, float h, ImU32 col) {
    const ImVec2 a = P(p0, s, cx - h * 0.65f, cy - h);
    const ImVec2 b = P(p0, s, cx - h * 0.65f, cy + h);
    const ImVec2 c = P(p0, s, cx + h * 0.85f, cy);
    dl->AddTriangleFilled(a, b, c, col);
}
void drawChevronGlyph(ImDrawList* dl, ImVec2 p0, float s, float cx, float cy,
                      float h, ImU32 col, bool pointRight) {
    const float dx = pointRight ? 1.0f : -1.0f;
    const float gap = h * 0.35f;
    for (int i = 0; i < 2; ++i) {
        const float ox = (i == 0 ? -gap : gap) * dx;
        const ImVec2 a = P(p0, s, cx + ox - dx * h * 0.5f, cy - h * 0.75f);
        const ImVec2 b = P(p0, s, cx + ox - dx * h * 0.5f, cy + h * 0.75f);
        const ImVec2 c = P(p0, s, cx + ox + dx * h * 0.6f, cy);
        dl->AddTriangleFilled(a, b, c, col);
    }
}
void drawStopGlyph(ImDrawList* dl, ImVec2 p0, float s, float cx, float cy, float h, ImU32 col) {
    const ImVec2 a = P(p0, s, cx - h * 0.75f, cy - h * 0.75f);
    const ImVec2 b = P(p0, s, cx + h * 0.75f, cy + h * 0.75f);
    dl->AddRectFilled(a, b, col);
}
void drawPauseGlyph(ImDrawList* dl, ImVec2 p0, float s, float cx, float cy, float h, ImU32 col) {
    const float bw = h * 0.28f;
    const float gap = h * 0.35f;
    dl->AddRectFilled(P(p0, s, cx - gap - bw, cy - h * 0.85f),
                      P(p0, s, cx - gap,       cy + h * 0.85f), col);
    dl->AddRectFilled(P(p0, s, cx + gap,       cy - h * 0.85f),
                      P(p0, s, cx + gap + bw,  cy + h * 0.85f), col);
}
void drawRecGlyph(ImDrawList* dl, ImVec2 p0, float s, float cx, float cy, float h, ImU32 col) {
    dl->AddCircleFilled(P(p0, s, cx, cy), S(s, h * 0.62f), col, 24);
}

} // namespace

void CassetteDeck_ImGui::reset()
{
    transport_   = Transport::Stopped;
    paused_      = false;
    counter_     = 0;
    counterAccum_ = 0.0;
    hubAngle_    = 0.0f;
    rewEndsAt_   = 0.0;
}

CassetteDeck_ImGui::FrameResult
CassetteDeck_ImGui::render(const char* title,
                           bool& open,
                           EmulationController* emulation,
                           const DeckSnapshot& snap,
                           float deltaSeconds)
{
    FrameResult out;
    if (!open) return out;

    ImGui::SetNextWindowSizeConstraints(
        ImVec2(kDesignW * 0.55f + 28.0f, kDesignH * 0.55f + 40.0f),
        ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    const bool visible = ImGui::Begin(title, &open, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    if (!visible) { ImGui::End(); return out; }

    wallClock_ += std::max(0.0f, deltaSeconds);
    syncWithSnapshot(snap);
    advanceCounter(deltaSeconds, snap);

    if ((transport_ == Transport::Rewinding || transport_ == Transport::FastForwarding)
         && wallClock_ >= rewEndsAt_ && !snap.rewinding) {
        transport_ = Transport::Stopped;
    }

    // ─── Header buttons ───────────────────────────────────────────────────
    constexpr float kActionBtnSize = 38.0f;
    constexpr float kActionIconScale = 1.45f;
    const ImVec2 actionSize(kActionBtnSize, kActionBtnSize);

    ImGui::SetWindowFontScale(kActionIconScale);
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "##DeckLoad", actionSize)) {
        loadDialogOpen = true;
        if (dialogPath.empty()) dialogPath = "cassettes/";
    }
    ImGui::SetWindowFontScale(1.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Load tape (ACI / WAV / MP3 / OGG / FLAC)");

    ImGui::SameLine();
    ImGui::SetWindowFontScale(kActionIconScale);
    if (ImGui::Button(ICON_FA_FILE_CIRCLE_PLUS "##DeckNew", actionSize)) {
        if (emulation) {
            emulation->ejectTape();
            emulation->clearTapeCapture();
        }
        transport_   = Transport::Stopped;
        paused_      = false;
        counter_     = 0;
        counterAccum_ = 0.0;
        out.statusMessage = "Nouvelle cassette (vide)";
    }
    ImGui::SetWindowFontScale(1.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Nouvelle cassette vierge\n"
                          "(ejecte la cassette courante + efface l'enregistrement)");

    ImGui::SameLine();
    ImGui::SetWindowFontScale(kActionIconScale);
    if (ImGui::Button(ICON_FA_FLOPPY_DISK "##DeckSave", actionSize)) {
        saveDialogOpen = true;
        if (dialogPath.empty()) dialogPath = "cassettes/recording.aci";
    }
    ImGui::SetWindowFontScale(1.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save captured tape (ACI / WAV)");

    ImGui::SameLine();
    ImGui::BeginDisabled(snap.recordedTransitions == 0);
    ImGui::SetWindowFontScale(kActionIconScale);
    if (ImGui::Button(ICON_FA_ERASER "##DeckClear", actionSize)) {
        if (emulation) emulation->clearTapeCapture();
        out.statusMessage = "Cassette capture cleared";
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(snap.recordedTransitions == 0
            ? "Clear capture (nothing recorded yet)"
            : "Clear captured output");
    }

    // VOL- / VOL+ stack.
    if (!volumeSynced_) { volume_ = snap.volume; volumeSynced_ = true; }
    ImGui::SameLine();
    constexpr float kVolBtnW = kActionBtnSize;
    constexpr float kVolBtnH = (kActionBtnSize - 4.0f) * 0.5f;
    const ImVec2 volSize(kVolBtnW, kVolBtnH);
    constexpr float kVolStep = 0.10f;
    constexpr float kVolMax  = 2.0f;
    ImGui::BeginGroup();
    ImGui::SetWindowFontScale(kActionIconScale * 0.7f);
    if (ImGui::Button(ICON_FA_VOLUME_HIGH "##DeckVolUp", volSize)) {
        muted_  = false;
        volume_ = std::min(kVolMax, volume_ + kVolStep);
        if (emulation) emulation->setCassetteVolume(volume_);
        char msg[64];
        std::snprintf(msg, sizeof(msg), "Cassette volume: %d%%",
                      static_cast<int>(std::round(volume_ * 100.0f)));
        out.statusMessage = msg;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Volume + 10%%");
    if (ImGui::Button(ICON_FA_VOLUME_LOW "##DeckVolDown", volSize)) {
        muted_  = false;
        volume_ = std::max(0.0f, volume_ - kVolStep);
        if (emulation) emulation->setCassetteVolume(volume_);
        char msg[64];
        std::snprintf(msg, sizeof(msg), "Cassette volume: %d%%",
                      static_cast<int>(std::round(volume_ * 100.0f)));
        out.statusMessage = msg;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Volume - 10%%");
    // The scale reset belongs HERE, after both buttons. It sat between VOL+
    // and VOL-, so the two halves of one control drew their icons at
    // different sizes.
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndGroup();

    // MUTE.
    ImGui::SameLine();
    const bool muteStylePushed = muted_;
    if (muteStylePushed) {
        ImGui::PushStyleColor(ImGuiCol_Button,        (ImVec4)ImColor(0.75f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor(0.88f, 0.25f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  (ImVec4)ImColor(0.62f, 0.14f, 0.14f, 1.0f));
    }
    ImGui::SetWindowFontScale(kActionIconScale);
    if (ImGui::Button(ICON_FA_VOLUME_XMARK "##DeckMute", actionSize)) {
        if (muted_) {
            muted_  = false;
            volume_ = preMuteVolume_;
            if (emulation) emulation->setCassetteVolume(volume_);
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Cassette unmuted (%d%%)",
                          static_cast<int>(std::round(volume_ * 100.0f)));
            out.statusMessage = msg;
        } else {
            preMuteVolume_ = volume_;
            muted_  = true;
            volume_ = 0.0f;
            if (emulation) emulation->setCassetteVolume(0.0f);
            out.statusMessage = "Cassette muted";
        }
    }
    ImGui::SetWindowFontScale(1.0f);
    if (muteStylePushed) ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(muted_ ? "Unmute cassette" : "Mute cassette");

    // Compact live status line.
    char headerInfo[256];
    std::snprintf(headerInfo, sizeof(headerInfo),
                  "in %zu tr  |  out %zu tr  |  audio %s  |  vol %d%%",
                  snap.loadedTransitions,
                  snap.recordedTransitions,
                  snap.audioAvailable ? "active" : "off",
                  static_cast<int>(std::round(volume_ * 100.0f)));
    ImGui::TextDisabled("%s", headerInfo);

    // ─── Big mode readout ────────────────────────────────────────────────
    const char* modeLabel;
    ImVec4 modeColor;
    if (!snap.loadedTape) {
        modeLabel = "NO TAPE";
        modeColor = ImVec4(0.58f, 0.58f, 0.62f, 1.0f);
    } else if (snap.audioStreamMode) {
        modeLabel = "AUDIO STREAM";
        modeColor = ImVec4(0.20f, 0.55f, 0.80f, 1.0f);
    } else {
        modeLabel = "PROGRAM TAPE";
        modeColor = ImVec4(0.85f, 0.55f, 0.15f, 1.0f);
    }
    constexpr float kModeScale = 1.6f;
    ImGui::SetWindowFontScale(kModeScale);
    const ImVec2 modeSize = ImGui::CalcTextSize(modeLabel);
    const float availW = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (availW - modeSize.x) * 0.5f));
    ImGui::TextColored(modeColor, "%s", modeLabel);
    ImGui::SetWindowFontScale(1.0f);

    // Armed banner — pulse-mode tape PLAY pressed but Apple II Monitor's
    // READ routine (at $FEFD) hasn't polled $C060 yet. As soon as the user
    // types e.g. `800.FFFR` and the routine starts reading, the flag drops.
    const bool armedWaiting = snap.loadedTape
                              && snap.playbackArmed
                              && !snap.audioStreamMode;
    if (armedWaiting) {
        const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(wallClock_) * 7.5f);
        const ImVec4 armedColor(0.95f, 0.28f, 0.22f, pulse);
        const char* kArmedText = "ARMED - waiting for READ";
        const ImVec2 armedSize = ImGui::CalcTextSize(kArmedText);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                             + std::max(0.0f, (availW - armedSize.x) * 0.5f));
        ImGui::TextColored(armedColor, "%s", kArmedText);
    }

    ImGui::Separator();

    // ─── Deck canvas ─────────────────────────────────────────────────────
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float sx = avail.x / kDesignW;
    const float sy = avail.y / kDesignH;
    const float s  = std::max(0.25f, std::min(sx, sy));
    const float canvasW = kDesignW * s;
    const float canvasH = kDesignH * s;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 p0(origin.x + (avail.x - canvasW) * 0.5f,
                    origin.y + (avail.y - canvasH) * 0.5f);
    ImGui::Dummy(ImVec2(avail.x, avail.y));

    ImDrawList* dl = ImGui::GetWindowDrawList();

    drawChassis(dl, p0, s);
    drawSlimLineBadge(dl, p0, s);

    bool counterResetClicked = false;
    LampMode lampMode = LampMode::Off;
    if (transport_ == Transport::Recording && !paused_) {
        lampMode = LampMode::Rec;
    } else if (armedWaiting) {
        lampMode = LampMode::Armed;
    } else if (snap.playbackActive
               && snap.loadedTape
               && !snap.audioStreamMode
               && !paused_) {
        lampMode = LampMode::Data;
    }
    drawCounter(dl, p0, s, "##CounterReset", counterResetClicked, lampMode);
    if (counterResetClicked) {
        counter_ = 0; counterAccum_ = 0.0;
        out.statusMessage = "Tape counter reset";
    }

    drawCassetteWindow(dl, p0, s, snap);
    drawBrandStrip(dl, p0, s);
    drawButtonLabels(dl, p0, s);

    // Piano keys — REC, PLAY, REW, FF, STOP, PAUSE.
    if (drawPianoKey(dl, p0, s, kKeyCenterXs[0], kKeysTop + kKeyH * 0.5f,
                     "##DeckKeyRec", "rec", recKeyEngaged(), false)) {
        std::string msg = onRecord(emulation, snap);
        if (!msg.empty()) out.statusMessage = std::move(msg);
    }
    if (drawPianoKey(dl, p0, s, kKeyCenterXs[1], kKeysTop + kKeyH * 0.5f,
                     "##DeckKeyPlay", "play", playKeyEngaged(), false)) {
        std::string msg = onPlay(emulation, snap);
        if (!msg.empty()) out.statusMessage = std::move(msg);
    }
    bool rewHeld = false;
    if (drawPianoKey(dl, p0, s, kKeyCenterXs[2], kKeysTop + kKeyH * 0.5f,
                     "##DeckKeyRew", "rew", rewKeyEngaged(), false, &rewHeld)) {
        std::string msg = onRewind(emulation, snap);
        if (!msg.empty()) out.statusMessage = std::move(msg);
    }
    if (rewHeld && snap.audioStreamMode && snap.loadedTape) {
        constexpr double kScrubSpeedX = 30.0;
        if (emulation) {
            emulation->stopTape();
            emulation->seekTapeRelative(-static_cast<double>(deltaSeconds) * kScrubSpeedX);
        }
        transport_ = Transport::Rewinding;
        paused_    = false;
        rewEndsAt_ = wallClock_ + 0.25;
    }

    bool ffHeld = false;
    if (drawPianoKey(dl, p0, s, kKeyCenterXs[3], kKeysTop + kKeyH * 0.5f,
                     "##DeckKeyFF", "ff", ffKeyEngaged(), false, &ffHeld)) {
        std::string msg = onFForward(emulation, snap);
        if (!msg.empty()) out.statusMessage = std::move(msg);
    }
    if (ffHeld && snap.audioStreamMode && snap.loadedTape) {
        constexpr double kScrubSpeedX = 30.0;
        if (emulation) {
            emulation->stopTape();
            emulation->seekTapeRelative(+static_cast<double>(deltaSeconds) * kScrubSpeedX);
        }
        transport_ = Transport::FastForwarding;
        paused_    = false;
        rewEndsAt_ = wallClock_ + 0.25;
    }

    if (drawPianoKey(dl, p0, s, kKeyCenterXs[4], kKeysTop + kKeyH * 0.5f,
                     "##DeckKeyStop", "stop", stopKeyEngaged(), false)) {
        std::string msg = onStop(emulation);
        if (!msg.empty()) out.statusMessage = std::move(msg);
    }
    if (drawPianoKey(dl, p0, s, kKeyCenterXs[5], kKeysTop + kKeyH * 0.5f,
                     "##DeckKeyPause", "pause", pauseKeyEngaged(), false)) {
        std::string msg = onPause(emulation);
        if (!msg.empty()) out.statusMessage = std::move(msg);
    }

    ImGui::End();
    return out;
}

// ─── Chassis ──────────────────────────────────────────────────────────────

void CassetteDeck_ImGui::drawChassis(ImDrawList* dl, ImVec2 p0, float s) const
{
    const ImVec2 a = P(p0, s, 0.0f, 0.0f);
    const ImVec2 b = P(p0, s, kDesignW, kDesignH);
    const float r = S(s, 14.0f);
    dl->AddRectFilled(ImVec2(a.x - S(s, 1.0f), a.y + S(s, 2.0f)),
                      ImVec2(b.x + S(s, 1.0f), b.y + S(s, 4.0f)),
                      IM_COL32(0, 0, 0, 90), r);
    dl->AddRectFilled(a, b, kChassis, r);
    dl->AddRect(a, b, kChassisEdgeHi, r, 0, S(s, 1.5f));
    dl->AddRect(ImVec2(a.x + S(s, 1.5f), a.y + S(s, 1.5f)),
                ImVec2(b.x - S(s, 1.5f), b.y - S(s, 1.5f)),
                kChassisEdgeLo, r * 0.85f, 0, S(s, 1.0f));
}

void CassetteDeck_ImGui::drawSlimLineBadge(ImDrawList* dl, ImVec2 p0, float s) const
{
    const Rect r = kBrandBadgeR;
    const ImVec2 a = P(p0, s, r.x0, r.y0);
    const ImVec2 b = P(p0, s, r.x1, r.y1);
    dl->AddRect(a, b, kBadgeBorder, S(s, 2.0f), 0, std::max(1.0f, S(s, 0.9f)));
    drawCenteredText(dl, p0, s, r, 12.0f, kBadgeText, "POM2");
}

void CassetteDeck_ImGui::drawCounter(ImDrawList* dl, ImVec2 p0, float s,
                                     const char* resetId, bool& resetClicked,
                                     LampMode lamp)
{
    drawText(dl, p0, s, 98.0f, 37.0f, 9.0f, kLabelTextDim, "COUNTER");

    const Rect r = kCounterWinR;
    const ImVec2 a = P(p0, s, r.x0, r.y0);
    const ImVec2 b = P(p0, s, r.x1, r.y1);
    const float round = S(s, 3.0f);
    dl->AddRectFilled(a, b, kCounterBg, round);
    dl->AddRect(a, b, kCounterRim, round, 0, std::max(1.0f, S(s, 0.9f)));

    char digits[8];
    std::snprintf(digits, sizeof(digits), "%03u", counter_ % 1000);
    ImFont* font = ImGui::GetFont();
    if (font) {
        const float fs = std::max(10.0f, S(s, 22.0f));
        const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, digits);
        const ImVec2 pos(a.x + ((b.x - a.x) - sz.x) * 0.5f,
                         a.y + ((b.y - a.y) - sz.y) * 0.5f - S(s, 0.5f));
        const ImU32 glow = IM_COL32(226, 158, 56, 40);
        const float off = std::max(1.0f, S(s, 0.8f));
        dl->AddText(font, fs, ImVec2(pos.x + off, pos.y), glow, digits);
        dl->AddText(font, fs, ImVec2(pos.x - off, pos.y), glow, digits);
        dl->AddText(font, fs, pos, kCounterDigit, digits);
    }

    // Reset button next to the counter.
    const float bx0 = r.x1 + 3.0f;
    const float bx1 = bx0 + kCounterResetW;
    const float by0 = r.y0 + 6.0f;
    const float by1 = r.y1 - 6.0f;
    const ImVec2 bp0 = P(p0, s, bx0, by0);
    const ImVec2 bp1 = P(p0, s, bx1, by1);
    ImGui::SetCursorScreenPos(bp0);
    ImGui::InvisibleButton(resetId, ImVec2(bp1.x - bp0.x, bp1.y - bp0.y));
    const bool hov = ImGui::IsItemHovered();
    const bool act = ImGui::IsItemActive();
    resetClicked = ImGui::IsItemClicked();
    const ImU32 col = act ? IM_COL32(120, 120, 124, 255)
                          : hov ? IM_COL32(200, 200, 204, 255)
                                : IM_COL32(160, 162, 166, 255);
    dl->AddRectFilled(bp0, bp1, col, S(s, 1.5f));
    dl->AddRect(bp0, bp1, IM_COL32(20,20,22,255), S(s, 1.5f), 0, 1.0f);
    if (hov) ImGui::SetTooltip("Reset tape counter");

    // Transport lamp: REC / CUE (armed) / DATA / OFF.
    const Rect lr = kRecLedR;
    const ImVec2 lc = P(p0, s, (lr.x0 + lr.x1) * 0.5f, (lr.y0 + lr.y1) * 0.5f);
    const float lrad = S(s, (lr.y1 - lr.y0) * 0.45f);
    const char* lampLabel = "REC";
    ImU32 lampOutline = IM_COL32(8, 8, 10, 255);
    switch (lamp) {
        case LampMode::Rec: {
            dl->AddCircleFilled(lc, lrad * 1.6f, IM_COL32(232, 56, 44, 40), 22);
            dl->AddCircleFilled(lc, lrad,        IM_COL32(232, 56, 44, 255), 22);
            lampLabel = "REC";
            break;
        }
        case LampMode::Armed: {
            const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(wallClock_) * 7.5f);
            const ImU32 coreA  = (ImU32)std::clamp((int)(255.0f * pulse), 60, 255);
            const ImU32 bloomA = (ImU32)std::clamp((int)(60.0f * pulse), 12, 60);
            dl->AddCircleFilled(lc, lrad * 1.6f, IM_COL32(240, 178, 32, bloomA), 22);
            dl->AddCircleFilled(lc, lrad,        IM_COL32(240, 178, 32, coreA), 22);
            lampLabel = "CUE";
            break;
        }
        case LampMode::Data: {
            const float pulse = 0.70f + 0.30f * std::sin(static_cast<float>(wallClock_) * 14.0f);
            const ImU32 coreA = (ImU32)std::clamp((int)(255.0f * pulse), 150, 255);
            dl->AddCircleFilled(lc, lrad * 1.6f, IM_COL32(48, 210, 96, 55), 22);
            dl->AddCircleFilled(lc, lrad,        IM_COL32(48, 210, 96, coreA), 22);
            lampLabel = "DATA";
            break;
        }
        case LampMode::Off:
        default:
            dl->AddCircleFilled(lc, lrad, IM_COL32(48, 14, 12, 255), 18);
            lampLabel = "REC";
            break;
    }
    dl->AddCircle(lc, lrad, lampOutline, 22, std::max(1.0f, S(s, 0.8f)));
    drawCenteredText(dl, p0, s,
                     Rect{ lr.x0 - 4.0f, lr.y1 + 0.5f, lr.x1 + 4.0f, lr.y1 + 10.5f },
                     8.5f, kLabelTextDim, lampLabel);
}

void CassetteDeck_ImGui::drawCassetteWindow(ImDrawList* dl, ImVec2 p0, float s,
                                            const DeckSnapshot& snap) const
{
    const Rect r = kCassetteR;
    const ImVec2 a = P(p0, s, r.x0, r.y0);
    const ImVec2 b = P(p0, s, r.x1, r.y1);
    const float round = S(s, 6.0f);

    dl->AddRectFilled(P(p0, s, r.x0 - 2.0f, r.y0 - 2.0f),
                      P(p0, s, r.x1 + 2.0f, r.y1 + 2.0f),
                      kCompartmentLip, round + S(s, 1.5f));

    dl->AddRectFilled(a, b, kGlassDark, round);
    dl->AddRect(a, b, kGlassEdgeDark, round, 0, std::max(1.0f, S(s, 0.9f)));

    if (snap.loadedTape) {
        const float pad = 10.0f;
        const ImVec2 ca = P(p0, s, r.x0 + pad, r.y0 + pad);
        const ImVec2 cb = P(p0, s, r.x1 - pad, r.y1 - pad);
        dl->AddRectFilled(ca, cb, IM_COL32(212, 210, 202, 255), S(s, 3.0f));
        dl->AddRect(ca, cb, IM_COL32(70, 68, 60, 255), S(s, 3.0f), 0, std::max(1.0f, S(s, 0.9f)));

        const float lpad = 18.0f;
        const Rect labelR { r.x0 + lpad, r.y0 + lpad,
                            r.x1 - lpad, r.y0 + (r.y1 - r.y0) * 0.55f };
        const ImVec2 la2 = P(p0, s, labelR.x0, labelR.y0);
        const ImVec2 lb2 = P(p0, s, labelR.x1, labelR.y1);
        dl->AddRectFilled(la2, lb2, IM_COL32(244, 242, 232, 255), S(s, 2.0f));
        dl->AddRect(la2, lb2, IM_COL32(150, 148, 136, 255), S(s, 2.0f), 0, 1.0f);

        // Filename — basename of the loaded path, truncated to fit.
        std::string name = snap.loadedTapePath;
        const size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        const float textRightLimit = labelR.x1 - 4.0f;
        const float textAvail = textRightLimit - (labelR.x0 + 4.0f);
        const size_t maxChars = std::max<size_t>(6, static_cast<size_t>(textAvail / 11.0f));
        if (name.size() > maxChars) name = name.substr(0, maxChars - 3) + "...";
        drawText(dl, p0, s, labelR.x0 + 4.0f, labelR.y0 + 4.0f, 22.0f,
                 IM_COL32(40, 40, 44, 255), name.c_str());

        char detail[96];
        if (snap.audioStreamMode) {
            drawText(dl, p0, s, labelR.x0 + 4.0f, labelR.y0 + 32.0f, 13.0f,
                     IM_COL32(50, 120, 160, 255), "AUDIO STREAM");
            if (!snap.loadInfo.empty()) {
                std::snprintf(detail, sizeof(detail), "Type %sR", snap.loadInfo.c_str());
            } else {
                const double total = snap.playbackTotalSec;
                if (total > 0.0) {
                    std::snprintf(detail, sizeof(detail), "%d:%02d",
                                  static_cast<int>(total) / 60,
                                  static_cast<int>(total) % 60);
                } else {
                    std::snprintf(detail, sizeof(detail), "streaming");
                }
            }
            drawText(dl, p0, s, labelR.x0 + 4.0f, labelR.y0 + 50.0f, 15.0f,
                     IM_COL32(96, 96, 100, 255), detail);
        } else {
            drawText(dl, p0, s, labelR.x0 + 4.0f, labelR.y0 + 32.0f, 13.0f,
                     IM_COL32(170, 110, 30, 255), "PROGRAM TAPE");
            if (!snap.loadInfo.empty()) {
                std::snprintf(detail, sizeof(detail), "Type %sR", snap.loadInfo.c_str());
            } else {
                std::snprintf(detail, sizeof(detail), "%zu transitions",
                              snap.loadedTransitions);
            }
            drawText(dl, p0, s, labelR.x0 + 4.0f, labelR.y0 + 50.0f, 15.0f,
                     IM_COL32(96, 96, 100, 255), detail);
        }

        // Two static hubs.
        const float hubY = r.y0 + (r.y1 - r.y0) * 0.68f;
        const float hubR = 19.0f;
        for (int i = 0; i < 2; ++i) {
            const float hx = (i == 0 ? r.x0 + (r.x1 - r.x0) * 0.28f
                                     : r.x0 + (r.x1 - r.x0) * 0.72f);
            dl->AddCircleFilled(P(p0, s, hx, hubY), S(s, hubR), kHubMid, 28);
            dl->AddCircleFilled(P(p0, s, hx, hubY), S(s, hubR * 0.55f), kHubDark, 20);
        }
    } else {
        drawCenteredText(dl, p0, s, r, 12.0f, IM_COL32(100, 100, 108, 200),
                         "NO TAPE - Load tape below");
    }

    if (snap.loadedTape) {
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 42), round);
        dl->AddRectFilledMultiColor(
            a, ImVec2(b.x, a.y + S(s, 14.0f)),
            IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 14),
            IM_COL32(255, 255, 255, 0),  IM_COL32(255, 255, 255, 0));
    }

    drawCenteredText(dl, p0, s,
                     Rect{ r.x0, r.y1 - 12.0f, r.x1, r.y1 + 2.0f },
                     9.0f, IM_COL32(220, 220, 224, 220),
                     "AC/BATTERY  FULL AUTO STOP");

    const ImVec2 ra = P(p0, s, r.x0 + 4.0f, r.y0 + 2.0f);
    const ImVec2 rb = P(p0, s, r.x1 - 4.0f, r.y0 + 14.0f);
    dl->AddRectFilledMultiColor(ra, rb,
                                kGlassReflect, kGlassReflect,
                                IM_COL32(255,255,255,0), IM_COL32(255,255,255,0));
}

void CassetteDeck_ImGui::drawBrandStrip(ImDrawList* dl, ImVec2 p0, float s) const
{
    const Rect r = kBrandR;
    const ImVec2 a = P(p0, s, r.x0, r.y0);
    const ImVec2 b = P(p0, s, r.x1, r.y1);
    dl->AddRectFilled(a, b, kBrandStrip, S(s, 2.0f));
    dl->AddRect(a, b, kBrandStripEdge, S(s, 2.0f), 0, 1.0f);
    drawCenteredText(dl, p0, s, r, 14.0f, kBrandText, "POM2 - CASSETTE DECK");
}

void CassetteDeck_ImGui::drawButtonLabels(ImDrawList* dl, ImVec2 p0, float s) const
{
    static const char* labels[6] = {
        "RECORD", "PLAY", "REW/REV", "FF/CUE", "STOP/EJECT", "PAUSE"
    };
    dl->AddRectFilled(P(p0, s, kLabelsR.x0, kLabelsR.y0),
                      P(p0, s, kLabelsR.x1, kLabelsR.y1),
                      IM_COL32(14, 14, 16, 255));
    ImFont* font = ImGui::GetFont();
    if (!font) return;
    const float fs = std::max(8.0f, S(s, 9.5f));
    for (int i = 0; i < 6; ++i) {
        const float cx = kKeyCenterXs[i];
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, labels[i]);
        const ImVec2 pos(p0.x + cx * s - ts.x * 0.5f,
                         p0.y + (kLabelsR.y0 + 2.0f) * s);
        dl->AddText(font, fs, pos, kLabelText, labels[i]);
    }
}

bool CassetteDeck_ImGui::drawPianoKey(ImDrawList* dl, ImVec2 p0, float s,
                                      float cx, float cy, const char* id,
                                      const char* glyph, bool engaged, bool disabled,
                                      bool* heldOut)
{
    const float hw = kKeyW * 0.5f;
    const float hh = kKeyH * 0.5f;
    const ImVec2 a = P(p0, s, cx - hw, cy - hh);
    const ImVec2 b = P(p0, s, cx + hw, cy + hh);
    const float round = S(s, kKeyRadius);

    ImGui::SetCursorScreenPos(a);
    const ImVec2 size(b.x - a.x, b.y - a.y);
    ImGui::InvisibleButton(id, size);

    const bool hov     = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();

    const float latchOffset = (engaged || active) ? 2.0f : 0.0f;
    const ImVec2 capA(a.x, a.y + S(s, latchOffset));
    const ImVec2 capB(b.x, b.y + S(s, latchOffset));

    dl->AddRectFilled(a, ImVec2(b.x, b.y + S(s, 3.0f)), kButtonEdge, round);
    dl->AddRectFilled(capA, capB,
                      (engaged || active) ? kButtonDown : kButtonBody, round);
    if (!engaged && !active) {
        const float by = std::floor(capA.y + S(s, 1.0f)) + 0.5f;
        const float bh = std::max(1.0f, S(s, 0.9f));
        dl->AddRectFilled(ImVec2(capA.x + round, by),
                          ImVec2(capB.x - round, by + bh),
                          kButtonHi);
    }
    dl->AddRect(capA, capB, kButtonEdge, round, 0, std::max(1.0f, S(s, 0.8f)));
    if (hov && !disabled) {
        dl->AddRect(capA, capB, IM_COL32(255, 255, 255, 28), round, 0,
                    std::max(1.0f, S(s, 1.2f)));
    }

    const float gx = cx;
    const float gy = cy + latchOffset;
    const float gh = 9.0f;
    ImU32 col = disabled ? kGlyphDim : kGlyph;
    if      (std::strcmp(glyph, "rec")   == 0) { col = engaged ? kGlyphRecActive : kGlyphRec; drawRecGlyph(dl, p0, s, gx, gy, gh, col); }
    else if (std::strcmp(glyph, "play")  == 0)   drawPlayGlyph(dl, p0, s, gx, gy, gh, col);
    else if (std::strcmp(glyph, "rew")   == 0)   drawChevronGlyph(dl, p0, s, gx, gy, gh, col, false);
    else if (std::strcmp(glyph, "ff")    == 0)   drawChevronGlyph(dl, p0, s, gx, gy, gh, col, true);
    else if (std::strcmp(glyph, "stop")  == 0)   drawStopGlyph(dl, p0, s, gx, gy, gh, col);
    else if (std::strcmp(glyph, "pause") == 0)   drawPauseGlyph(dl, p0, s, gx, gy, gh, col);

    if (heldOut) *heldOut = active && !disabled;
    return clicked && !disabled;
}

// ─── Transport state machine ──────────────────────────────────────────────

std::string CassetteDeck_ImGui::onRecord(EmulationController* emu,
                                         const DeckSnapshot& /*snap*/)
{
    if (emu) emu->clearTapeCapture();
    transport_ = Transport::Recording;
    paused_    = false;
    rewEndsAt_ = 0.0;
    return "Cassette: REC+PLAY engaged (output capture armed)";
}

std::string CassetteDeck_ImGui::onPlay(EmulationController* emu,
                                       const DeckSnapshot& snap)
{
    if (transport_ == Transport::Recording) { paused_ = false; return "Cassette: REC+PLAY"; }
    if (!snap.loadedTape) {
        transport_ = Transport::Playing;
        paused_    = false;
        return "Cassette: PLAY engaged (no tape loaded)";
    }
    if (emu) emu->playTape();
    transport_ = Transport::Playing;
    paused_    = false;
    rewEndsAt_ = 0.0;
    return "Cassette: PLAY - tape rolling";
}

std::string CassetteDeck_ImGui::onRewind(EmulationController* emu,
                                         const DeckSnapshot& snap)
{
    if (emu) {
        emu->pauseTape(false);
        emu->rewindTape();
    }
    transport_ = Transport::Rewinding;
    paused_    = false;
    rewEndsAt_ = wallClock_ + kWindDurationSeconds;
    if (!snap.audioStreamMode) return "Cassette: REW - tape rewinding...";
    return "Cassette: REW - tape rewound to start";
}

std::string CassetteDeck_ImGui::onFForward(EmulationController* emu,
                                           const DeckSnapshot& snap)
{
    if (emu) emu->pauseTape(false);
    if (snap.audioStreamMode && snap.loadedTape) {
        if (emu) emu->seekTapeRelative(+5.0);
        transport_ = Transport::FastForwarding;
        paused_    = false;
        rewEndsAt_ = wallClock_ + kWindDurationSeconds;
        return "Cassette: FF +5s";
    }
    transport_ = Transport::FastForwarding;
    paused_    = false;
    rewEndsAt_ = wallClock_ + kWindDurationSeconds;
    return "Cassette: FF (virtual tape has no seek - decorative)";
}

std::string CassetteDeck_ImGui::onStop(EmulationController* emu)
{
    if (emu) emu->stopTape();
    transport_ = Transport::Stopped;
    paused_    = false;
    rewEndsAt_ = 0.0;
    return "Cassette: STOP";
}

std::string CassetteDeck_ImGui::onPause(EmulationController* emu)
{
    if (transport_ != Transport::Playing && transport_ != Transport::Recording) return {};
    paused_ = !paused_;
    if (emu) emu->pauseTape(paused_);
    return paused_ ? "Cassette: PAUSE" : "Cassette: resume";
}

std::string CassetteDeck_ImGui::onEject(EmulationController* emu,
                                        const DeckSnapshot& /*snap*/)
{
    if (emu) emu->ejectTape();
    transport_ = Transport::Stopped;
    paused_    = false;
    return "Cassette: EJECT - tape removed";
}

// ─── Counter / sync ───────────────────────────────────────────────────────

void CassetteDeck_ImGui::advanceCounter(float deltaSeconds, const DeckSnapshot& snap)
{
    if (deltaSeconds <= 0.0f) return;

    if (snap.audioStreamMode && snap.loadedTape) {
        const double ticks = snap.playbackPositionSec / kCounterPlaySecPerTick;
        counter_ = static_cast<uint32_t>(ticks) % 1000;
        counterAccum_ = 0.0;
        hubAngle_ = std::fmod(hubAngle_ + deltaSeconds * 4.0f, 6.2831853f);
        return;
    }

    double secPerTick = 0.0;
    switch (transport_) {
        case Transport::Playing:
        case Transport::Recording:
            if (paused_) return;
            if (snap.playbackArmed && !snap.audioStreamMode) return;
            secPerTick = kCounterPlaySecPerTick;
            break;
        case Transport::Rewinding:
        case Transport::FastForwarding:
            secPerTick = kCounterWindSecPerTick;
            break;
        default:
            return;
    }
    counterAccum_ += static_cast<double>(deltaSeconds) / secPerTick;
    if (counterAccum_ >= 1.0) {
        const uint32_t ticks = static_cast<uint32_t>(counterAccum_);
        if (transport_ == Transport::Rewinding) {
            const uint32_t dec = ticks % 1000;
            counter_ = (counter_ + 1000 - dec) % 1000;
        } else {
            counter_ = (counter_ + ticks) % 1000;
        }
        counterAccum_ -= ticks;
    }
    hubAngle_ = std::fmod(hubAngle_ + deltaSeconds * 4.0f, 6.2831853f);
}

void CassetteDeck_ImGui::syncWithSnapshot(const DeckSnapshot& snap)
{
    if (transport_ == Transport::Playing && !snap.playbackActive
        && !snap.playbackArmed && !snap.rewinding && snap.loadedTape) {
        if (counterAccum_ > 0.0 || counter_ > 0) {
            transport_ = Transport::Stopped;
            paused_    = false;
        }
    }
    if (!snap.loadedTape
        && (transport_ == Transport::Playing || transport_ == Transport::Recording)) {
        if (transport_ == Transport::Playing) {
            transport_ = Transport::Stopped;
            paused_    = false;
        }
    }
}

} // namespace pom2
