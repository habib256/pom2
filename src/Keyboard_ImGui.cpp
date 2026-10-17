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

// Keyboard_ImGui — implementation. Hit-testing and latches only; see the
// header for the split with MainWindow.

#include "Keyboard_ImGui.h"

#include "Pom2Theme.h"
#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cstdint>   // intptr_t for the ImTextureID cast — libc++ pulls it
                     // in transitively, libstdc++ does not (CI, GCC).
#include <cstring>

namespace pom2 {

namespace {

// Does this hotspot describe a latch the panel holds down?
bool isLatch(const KeyHotspot& k)
{
    return k.kind == KeyKind::Modifier;
}

} // namespace

Keyboard_ImGui::Event
Keyboard_ImGui::render(bool* open, unsigned int texture, int texW, int texH,
                       const std::string& loadError, const KeyboardSkin& skin)
{
    Event ev;
    ev.latches = latches_;
    if (!open || !*open) return ev;

    // Sized to the photo's own 2578x908 (~2.84:1) so the default window opens
    // on an undistorted keyboard rather than on a letterboxed sliver.
    ImGui::SetNextWindowSize(ImVec2(980, 430), ImGuiCond_FirstUseEver);
    const std::string title = std::string(ICON_FA_KEYBOARD " ") + skin.title + "###appleKeyboard";
    const bool visible = ImGui::Begin(title.c_str(), open);
    if (!visible) {
        ImGui::End();
        return ev;
    }

    const Palette& pal = palette();
    const auto u32 = ImGui::ColorConvertU32ToFloat4;

    if (texture == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, u32(pal.warn));
        ImGui::TextWrapped("%s could not be loaded%s%s", skin.image,
                           loadError.empty() ? "" : " — ", loadError.c_str());
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "The window needs the photo: every hotspot is positioned as a "
            "fraction of that image, so there is nothing to click without it. "
            "It ships in the package (packaging/bundle.manifest) and lives in "
            "pic/ in a source tree.");
        ImGui::End();
        return ev;
    }

    // ── Latch row ───────────────────────────────────────────────────────
    // The same five latches are clickable on the picture; they are repeated
    // here because a latch you have to spot on a photo is a latch you will
    // forget is down — and a stuck Open-Apple looks exactly like a broken
    // emulator to the guest.
    {
        auto chip = [&](const char* label, bool* flag, const char* tip) {
            const ImVec4 col = u32(*flag ? pal.accent : pal.textDim);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(col.x, col.y, col.z, *flag ? 0.28f : 0.10f));
            if (ImGui::Button(label)) *flag = !*flag;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::SameLine();
        };
        chip("SHIFT", &latches_.shift,
             "One-shot: released by the next character key.");
        chip("CONTROL", &latches_.control,
             "One-shot: released by the next character key.\n"
             "Ctrl+letter sends $01..$1A — and Ctrl+Reset is how an\n"
             "Apple II resets, so latch this before clicking Reset.");
        chip("CAPS LOCK", &latches_.caps,
             "Stays down until clicked again, like the //e's mechanical\n"
             "latch. Down = letters arrive uppercase, which is what\n"
             "Applesoft and every II/II+-era program expects.");
        chip("OPEN-APPLE", &latches_.openApple,
             "Held down at $C061 bit 7 for as long as it is latched, so\n"
             "Open-Apple+Ctrl+Reset (the //e self-test / cold boot) can be\n"
             "clicked one key at a time.");
        chip(skin.compact ? "OPTION" : "SOLID-APPLE", &latches_.solidApple,
             "Held down at $C062 bit 7 while latched.");
        // The //c photograph excludes the separate Reset key.
        if (skin.compact) {
            if (ImGui::Button("RESET")) {
                static const KeyHotspot reset{"reset", "Reset", KeyKind::Special,
                    KeyAction::Reset, 0, 0, 0, 0, 0, 0};
                ev.key = &reset;
                ev.latches = latches_;
            }
            ImGui::SameLine();
        }
        ImGui::Checkbox("Show hitboxes", &showHitboxes_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Outline every mapped cap. The boxes come from measuring the\n"
                "photo (tools/gen_keyboard_layout.py), so this is also how you\n"
                "check the mapping still lines up if the picture is replaced.");
    }

    // ── The keyboard ────────────────────────────────────────────────────
    const ImVec2 regionStart = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // Replacement //c photos may have another pixel ratio. Keep the same
    // displayed proportions as the //e, including the fractional hitboxes.
    const float aspect = skin.displayAspect > 0.0f ? 1.0f / skin.displayAspect
        : static_cast<float>(texH) / static_cast<float>(texW);
    // Keep the picture inside the space below the controls and center it in
    // both directions. The same origin also anchors every key hotspot.
    if (avail.x <= 0.0f || avail.y <= 0.0f) {
        ImGui::End();
        return ev;
    }
    const float drawW = std::min(avail.x, avail.y / aspect);
    const float drawH = drawW * aspect;
    const ImVec2 p0(regionStart.x + (avail.x - drawW) * 0.5f,
                    regionStart.y + (avail.y - drawH) * 0.5f);
    ImGui::SetCursorScreenPos(p0);
    const ImVec2 size(drawW, drawH);
    ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(texture)), size);
    const bool imageHovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const double now = ImGui::GetTime();

    auto rectOf = [&](const KeyHotspot& k) {
        return ImVec4(p0.x + k.x0 * size.x, p0.y + k.y0 * size.y,
                      p0.x + k.x1 * size.x, p0.y + k.y1 * size.y);
    };
    auto inside = [&](const ImVec4& r) {
        return mouse.x >= r.x && mouse.x <= r.z &&
               mouse.y >= r.y && mouse.y <= r.w;
    };

    // Pass 1: find what is under the pointer. Done before drawing so the
    // L-shaped Return highlights BOTH arms when either is hovered — the
    // hover test is per rect, the highlight is per key id.
    const KeyHotspot* hovered = nullptr;
    if (imageHovered) {
        for (const KeyHotspot& k : skin.keys)
            if (inside(rectOf(k))) { hovered = &k; break; }
    }

    // Pass 2: draw. Latched modifiers stay lit; the last key pressed flashes
    // for 150 ms, which is the only feedback a click otherwise gets (the
    // guest may print nothing at all).
    for (const KeyHotspot& k : skin.keys) {
        const ImVec4 r = rectOf(k);
        const ImVec2 a(r.x, r.y), b(r.z, r.w);
        const bool sameKey = hovered && std::strcmp(hovered->id, k.id) == 0;

        bool lit = false;
        ImU32 tint = pal.accent;
        if (isLatch(k)) {
            switch (k.action) {
                case KeyAction::Shift:      lit = latches_.shift;      break;
                case KeyAction::Control:    lit = latches_.control;    break;
                case KeyAction::CapsLock:   lit = latches_.caps;       break;
                case KeyAction::OpenApple:  lit = latches_.openApple;  break;
                case KeyAction::SolidApple: lit = latches_.solidApple; break;
                default: break;
            }
        }
        if (!lit && std::strcmp(k.id, lastPressId_.c_str()) == 0 &&
            now - lastPressTime_ < 0.15) {
            lit  = true;
            tint = pal.ok;
        }
        if (k.action == KeyAction::Reset) tint = pal.danger;

        const ImVec4 t = u32(tint);
        if (lit)
            dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(
                                        ImVec4(t.x, t.y, t.z, 0.45f)), 6.0f);
        if (sameKey) {
            dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(
                                        ImVec4(t.x, t.y, t.z, 0.22f)), 6.0f);
            dl->AddRect(a, b, ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(t.x, t.y, t.z, 0.95f)), 6.0f, 0, 2.0f);
        } else if (showHitboxes_) {
            dl->AddRect(a, b, ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(t.x, t.y, t.z, 0.35f)), 4.0f);
        }
    }

    // ── Click ───────────────────────────────────────────────────────────
    if (hovered) {
        ImGui::SetTooltip("%s", hovered->label);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            lastPressId_   = hovered->id;
            lastPressTime_ = now;
            if (isLatch(*hovered)) {
                switch (hovered->action) {
                    case KeyAction::Shift:
                        latches_.shift = !latches_.shift; break;
                    case KeyAction::Control:
                        latches_.control = !latches_.control; break;
                    case KeyAction::CapsLock:
                        latches_.caps = !latches_.caps; break;
                    case KeyAction::OpenApple:
                        latches_.openApple = !latches_.openApple; break;
                    case KeyAction::SolidApple:
                        latches_.solidApple = !latches_.solidApple; break;
                    default: break;
                }
                ev.latches = latches_;
            } else {
                // Report the latch state AS IT WAS at the click: the host
                // clears the one-shots after consuming them, and it must see
                // the Shift that produced the character, not the cleared one.
                ev.latches = latches_;
                ev.key     = hovered;
            }
        }
    }

    ImGui::End();
    return ev;
}

} // namespace pom2
