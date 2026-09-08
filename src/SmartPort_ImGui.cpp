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

#include "SmartPort_ImGui.h"

#include "IconsFontAwesome6.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "StatusLed.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace pom2 {

namespace {

// Type combo: indexed entries map back to a SmartPortUnit::kindKey.
// Order is "[empty]" first so a freshly-plugged card defaults to empty
// units when no setting exists.
struct TypeEntry {
    const char*      label;
    std::string_view kindKey;
};
const TypeEntry kTypes[] = {
    { "[empty]",     {} },
    { "3.5\" 800K",  SmartPort35Unit::kKindKey },
    { "ProDOS HDV",  SmartPortHdvUnit::kKindKey },
};
constexpr int kTypeCount = static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0]));

int typeIndexFromKind(std::string_view k) {
    for (int i = 0; i < kTypeCount; ++i) {
        if (kTypes[i].kindKey == k) return i;
    }
    return 0;   // unknown → show as empty
}

std::string fmtBlocks(uint32_t blocks) {
    if (blocks == 0) return "—";
    char buf[32];
    const double kb = static_cast<double>(blocks) * 0.5;   // 512 B = 0.5 KB
    if (kb < 1024.0) std::snprintf(buf, sizeof(buf), "%.0f KB", kb);
    else             std::snprintf(buf, sizeof(buf), "%.1f MB", kb / 1024.0);
    return buf;
}

} // anon namespace

SmartPort_ImGui::Result SmartPort_ImGui::render(
    const char*         title,
    bool&               open,
    const CardSnapshot& snap)
{
    Result r;
    if (!open) return r;

    ImGui::SetNextWindowSize(ImVec2(620, 280), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    if (!snap.plugged) {
        ImGui::TextWrapped(
            "No SmartPort card plugged. Add one via Slot Configuration "
            "(set a slot's card type to \"SmartPort 3.5\\\"\").");
        ImGui::End();
        return r;
    }

    ImGui::Text("Card: slot %d  (Liron-class, %zu units max)",
                snap.slot, snap.units.size());
    ImGui::TextDisabled(
        "ProDOS sees unit 0 / 1 as drive 1 / 2 of slot %d.", snap.slot);
    ImGui::Separator();

    for (size_t k = 0; k < snap.units.size(); ++k) {
        const UnitSnapshot& u = snap.units[k];
        UnitAction&         a = r.units[k];
        ImGui::PushID(static_cast<int>(k));

        char hdr[32];
        std::snprintf(hdr, sizeof(hdr), "Unit %zu", k);
        ImGui::TextUnformatted(hdr);
        ImGui::SameLine();
        // Shared status LED: grey empty / green loaded / yellow write-protected.
        pom2::statusLed(u.loaded, u.writeProtected, /*error=*/false,
                        u.loaded ? u.path.c_str() : nullptr);
        ImGui::TextDisabled("(%s)", u.kind.empty() ? "no type"
                                                   : u.kindLabel.c_str());

        // ── Type selector ───────────────────────────────────────────────
        int curIdx = typeIndexFromKind(u.kind);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##type", kTypes[curIdx].label)) {
            for (int i = 0; i < kTypeCount; ++i) {
                const bool sel = (i == curIdx);
                if (ImGui::Selectable(kTypes[i].label, sel)) {
                    if (i != curIdx) {
                        if (i == 0) {
                            // empty
                            a.clearType = true;
                        } else {
                            a.setType = std::string(kTypes[i].kindKey);
                        }
                    }
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("size: %s", fmtBlocks(u.blockCount).c_str());

        // ── Path + Mount / Eject buttons ────────────────────────────────
        // First time we render a row, prime its buffer with the current
        // path so the user can edit-and-mount without re-typing it.
        if (pathBufs_[k][0] == '\0' && !u.path.empty()) {
            std::strncpy(pathBufs_[k].data(), u.path.c_str(),
                         pathBufs_[k].size() - 1);
        }
        ImGui::SetNextItemWidth(380.0f);
        ImGui::InputTextWithHint("##path", "/path/to/image.po or .hdv",
                                 pathBufs_[k].data(), pathBufs_[k].size());
        ImGui::SameLine();
        const bool typeAllowsMount = !u.kind.empty();
        if (!typeAllowsMount) ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " Mount")) {
            const char* p = pathBufs_[k].data();
            if (p[0] != '\0') a.mountPath = p;
        }
        if (!typeAllowsMount) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!u.loaded) ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_EJECT " Eject")) {
            a.eject = true;
        }
        if (!u.loaded) ImGui::EndDisabled();

        // ── Write-protect toggle ───────────────────────────────────────
        // Writable by default (2026-09-08); the tick is the visible opt-out.
        bool protect = !u.writeBackEnabled;
        if (!typeAllowsMount) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Write-protected (do not save changes)", &protect)) {
            a.writeBackChanged = true;
            a.writeBackOn      = !protect;
        }
        if (!typeAllowsMount) ImGui::EndDisabled();
        if (u.loaded) {
            ImGui::SameLine();
            if (u.writeProtected)
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f), "WRITE-PROTECTED");
            else
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "WRITABLE");
        }

        if (!u.lastError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                               "%s", u.lastError.c_str());
        } else if (u.loaded) {
            ImGui::TextDisabled("Mounted: %s", u.path.c_str());
        } else {
            ImGui::TextDisabled("(no media)");
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::End();
    return r;
}

} // namespace pom2
