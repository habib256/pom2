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

#include "JoystickPanel_ImGui.h"

#include "imgui.h"

#include "JoystickInput.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace pom2 {

namespace {

void drawAxisBar(const char* label, float value, float deadzone)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(60);
    const ImVec2 size(180.0f, 14.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y),
                      IM_COL32(40, 40, 40, 255));
    const float cx = p0.x + size.x * 0.5f;
    dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p0.y + size.y),
                IM_COL32(90, 90, 90, 255));
    if (deadzone > 0.0f) {
        const float dx = size.x * 0.5f * deadzone;
        dl->AddRectFilled(ImVec2(cx - dx, p0.y),
                          ImVec2(cx + dx, p0.y + size.y),
                          IM_COL32(60, 60, 30, 255));
    }
    float clamped = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
    const float vx = cx + clamped * size.x * 0.5f;
    dl->AddRectFilled(ImVec2(vx - 2.0f, p0.y - 2.0f),
                      ImVec2(vx + 2.0f, p0.y + size.y + 2.0f),
                      IM_COL32(220, 200, 80, 255));
    ImGui::Dummy(size);
    ImGui::SameLine();
    ImGui::Text("%+.2f", value);
}

void drawButtonLed(const char* label, bool down)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(60);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 6.0f;
    const ImU32 col = down ? IM_COL32(220, 60, 60, 255)
                           : IM_COL32(60, 60, 60, 255);
    dl->AddCircleFilled(ImVec2(p.x + r + 2, p.y + r + 2), r, col);
    dl->AddCircle      (ImVec2(p.x + r + 2, p.y + r + 2), r,
                        IM_COL32(0, 0, 0, 255), 0, 1.5f);
    ImGui::Dummy(ImVec2(r * 2 + 8, r * 2 + 4));
    ImGui::SameLine();
    ImGui::TextDisabled(down ? "DOWN" : "up");
}

const JoystickPanel_ImGui::HostDevice*
findHost(const std::vector<JoystickPanel_ImGui::HostDevice>& hosts, int idx)
{
    for (const auto& h : hosts) if (h.index == idx) return &h;
    return nullptr;
}

std::string sourceLabel(int src)
{
    switch (src) {
    case JoystickInput::SrcNone:     return "None (centred)";
    case JoystickInput::SrcStick1X:  return "Stick 1 X";
    case JoystickInput::SrcStick1Y:  return "Stick 1 Y";
    case JoystickInput::SrcStick2X:  return "Stick 2 X";
    case JoystickInput::SrcStick2Y:  return "Stick 2 Y";
    case JoystickInput::SrcTriggerL: return "L2 trigger";
    case JoystickInput::SrcTriggerR: return "R2 trigger";
    default: break;
    }
    if (src >= JoystickInput::SrcRaw0 &&
        src < JoystickInput::SrcRaw0 + JoystickInput::kRawAxes)
        return "Raw axis " + std::to_string(src - JoystickInput::SrcRaw0);
    return "None (centred)";
}

} // namespace

JoystickPanel_ImGui::FrameResult JoystickPanel_ImGui::render(
    const char* title, bool& open, const Snapshot& snap)
{
    FrameResult r;
    r.hostIdx    = snap.hostIdx;
    r.deadzone   = snap.deadzone;
    r.invert     = snap.invert;
    r.squareGate = snap.squareGate;
    r.source     = snap.source;

    if (!open) { learning_ = -1; return r; }

    ImGui::SetNextWindowSize(ImVec2(380, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    ImGui::TextDisabled(
        "Default: stick 1 = the Apple II joystick, PDL(0/1); stick 2 ="
        " PDL(2/3). Each paddle's source can be changed below."
        " Buttons feed PB0/PB1/PB2.");
    ImGui::Separator();

    // ─── Device combo ────────────────────────────────────────────────────
    auto previewLabel = [&](int idx) -> std::string {
        if (idx < 0) return "(none — joystick disabled)";
        const auto* h = findHost(snap.hosts, idx);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "#%d %s",
                      idx + 1, h ? h->name.c_str() : "(disconnected)");
        return buf;
    };

    if (ImGui::BeginCombo("Joystick", previewLabel(snap.hostIdx).c_str())) {
        if (ImGui::Selectable("(none — joystick disabled)",
                              snap.hostIdx < 0)) {
            r.hostIdx  = -1;
            r.changed  = true;
        }
        for (const auto& h : snap.hosts) {
            char item[192];
            std::snprintf(item, sizeof(item), "#%d %s",
                          h.index + 1, h.name.c_str());
            if (ImGui::Selectable(item, h.index == snap.hostIdx)) {
                r.hostIdx = h.index;
                r.changed = true;
            }
        }
        ImGui::EndCombo();
    }

    if (snap.hosts.empty()) {
        ImGui::TextDisabled("No host pad detected. Plug one in;"
                            " GLFW will pick it up automatically.");
    }

    ImGui::Spacing();

    // ─── Live preview of the bound device ────────────────────────────────
    const HostDevice* bound =
        snap.hostIdx >= 0 ? findHost(snap.hosts, snap.hostIdx) : nullptr;
    if (bound) {
        drawAxisBar("Stick 1 X", bound->axis[0], snap.deadzone);
        drawAxisBar("Stick 1 Y", bound->axis[1], snap.deadzone);
        drawAxisBar("Stick 2 X", bound->axis[2], snap.deadzone);
        drawAxisBar("Stick 2 Y", bound->axis[3], snap.deadzone);
        drawAxisBar("L2", bound->trigger[0], 0.0f);
        drawAxisBar("R2", bound->trigger[1], 0.0f);
        if (ImGui::TreeNode("Raw host axes")) {
            for (int k = 0; k < bound->rawCount && k < kRawAxes; ++k) {
                char label[16];
                std::snprintf(label, sizeof(label), "Raw %d", k);
                drawAxisBar(label, bound->raw[k], 0.0f);
            }
            if (bound->rawCount == 0) ImGui::TextDisabled("(no axes)");
            ImGui::TreePop();
        }
        ImGui::Spacing();
        drawButtonLed("Btn 0", bound->buttons[0]);
        drawButtonLed("Btn 1", bound->buttons[1]);
        drawButtonLed("Btn 2", bound->buttons[2]);
    } else {
        ImGui::TextDisabled("Joystick disabled — paddles read 128, buttons up.");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ─── Paddle sources ──────────────────────────────────────────────────
    // "Detect" learns a source from a move: the raw axis that travels
    // furthest from where it sat when the button was clicked (> 0.5) wins.
    if (!bound) learning_ = -1;
    if (learning_ >= 0 && bound) {
        int best = -1; float bestDelta = 0.5f;
        for (int k = 0; k < bound->rawCount && k < kRawAxes; ++k) {
            const float delta = std::fabs(bound->raw[k] - learnBase_[k]);
            if (delta > bestDelta) { bestDelta = delta; best = k; }
        }
        if (best >= 0) {
            r.source[learning_] = JoystickInput::SrcRaw0 + best;
            r.changed = true;
            learning_ = -1;
        }
    }
    ImGui::TextUnformatted("Paddle sources:");
    static const int kChoices[] = {
        JoystickInput::SrcStick1X, JoystickInput::SrcStick1Y,
        JoystickInput::SrcStick2X, JoystickInput::SrcStick2Y,
        JoystickInput::SrcTriggerL, JoystickInput::SrcTriggerR,
        JoystickInput::SrcNone };
    for (int i = 0; i < kAxes; ++i) {
        ImGui::PushID(i);
        char label[16];
        std::snprintf(label, sizeof(label), "PDL(%d)", i);
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::BeginCombo(label, sourceLabel(r.source[i]).c_str())) {
            for (int c : kChoices)
                if (ImGui::Selectable(sourceLabel(c).c_str(), r.source[i] == c)) {
                    r.source[i] = c; r.changed = true;
                }
            for (int k = 0; k < kRawAxes; ++k) {
                const int c = JoystickInput::SrcRaw0 + k;
                if (ImGui::Selectable(sourceLabel(c).c_str(), r.source[i] == c)) {
                    r.source[i] = c; r.changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (learning_ == i) {
            if (ImGui::SmallButton("Cancel")) learning_ = -1;
            ImGui::SameLine();
            ImGui::TextDisabled("move the stick or trigger…");
        } else if (bound && ImGui::SmallButton("Detect")) {
            learning_ = i;
            learnBase_ = bound->raw;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Restore defaults")) {
        r.source = { JoystickInput::SrcStick1X, JoystickInput::SrcStick1Y,
                     JoystickInput::SrcStick2X, JoystickInput::SrcStick2Y };
        r.changed = true;
        learning_ = -1;
    }
    ImGui::Spacing();
    ImGui::Separator();

    // ─── Tuning ──────────────────────────────────────────────────────────
    if (ImGui::SliderFloat("Deadzone", &r.deadzone, 0.0f, 0.5f, "%.2f"))
        r.changed = true;
    // Inversion is per PADDLE, whatever source feeds it.
    if (ImGui::Checkbox("Invert PDL(0)", &r.invert[0])) r.changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert PDL(1)", &r.invert[1])) r.changed = true;
    if (ImGui::Checkbox("Invert PDL(2)", &r.invert[2])) r.changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert PDL(3)", &r.invert[3])) r.changed = true;

    if (ImGui::Checkbox("Square gate (Apple II corners)", &r.squareGate))
        r.changed = true;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Modern sticks ride a ROUND gate, so a full diagonal only\n"
            "reaches ~217/217 and the extreme corners are unreachable.\n"
            "The real Apple II stick rode a SQUARE gate: full X and full Y\n"
            "at once (255/255). Required to take off in Wings of Fury.");
    }

    // ─── Apple II side mirror ────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextUnformatted("Apple II side (live):");
    ImGui::Text(" PADL(0)=%-3u  PADL(1)=%-3u",
                snap.appleIIPaddle[0], snap.appleIIPaddle[1]);
    ImGui::Text(" PADL(2)=%-3u  PADL(3)=%-3u",
                snap.appleIIPaddle[2], snap.appleIIPaddle[3]);
    ImGui::Text(" PB0=%s  PB1=%s  PB2=%s",
                snap.appleIIButton[0] ? "DOWN" : "up",
                snap.appleIIButton[1] ? "DOWN" : "up",
                snap.appleIIButton[2] ? "DOWN" : "up");

    ImGui::End();
    return r;
}

} // namespace pom2
