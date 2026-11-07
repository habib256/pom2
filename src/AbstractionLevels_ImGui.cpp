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

// AbstractionLevels_ImGui — implementation. See the header for why this
// window exists; `docs/lle_vs_hle.md` is the reference for every level in
// the catalog below, and the two must be edited together.

#include "AbstractionLevels_ImGui.h"

#include "Pom2Theme.h"
#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace pom2 {

namespace {

// Colour per level. Deliberately NOT a good/bad ramp: H1 is the right answer
// for a card with no public ROM dump, and painting it red would be a lie
// about POM2's own decision rule. Green→blue→cyan walks *down* the stack,
// amber marks "the boundary is the service", grey means off-axis.
ImU32 levelColour(AbsLevel l)
{
    const Palette& p = palette();
    switch (l) {
        case AbsLevel::L0:   return p.ok;
        case AbsLevel::L1:   return p.info;
        case AbsLevel::L2:   return p.accent;
        case AbsLevel::H1:   return p.warn;
        case AbsLevel::H2:   return p.textDim;
        case AbsLevel::Host: return p.textDim;
    }
    return p.text;
}

// A filled chip rather than coloured text: the level is the column the eye
// scans, and at a glance a chip separates from prose in a way a coloured
// word does not. Drawn by hand because ImGui has no badge primitive.
void levelChip(AbsLevel l)
{
    const ImU32 col = levelColour(l);
    const char* txt = levelBadge(l);
    const ImVec2 sz = ImGui::CalcTextSize(txt);
    const ImVec2 pad(ImGui::GetStyle().FramePadding.x, 1.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + sz.x + pad.x * 2.0f, p0.y + sz.y + pad.y * 2.0f);

    ImVec4 f = ImGui::ColorConvertU32ToFloat4(col);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(f.x, f.y, f.z, 0.18f)),
                      3.0f);
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(
                            ImVec4(f.x, f.y, f.z, 0.55f)),
                3.0f);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), col, txt);
    ImGui::Dummy(ImVec2(p1.x - p0.x, p1.y - p0.y));
}

} // namespace

// ─── The window ──────────────────────────────────────────────────────────

AbstractionLevels_ImGui::Request
AbstractionLevels_ImGui::render(bool* open, const Snapshot& snap)
{
    Request req;
    if (!open || !*open) return req;

    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_LAYER_GROUP " Abstraction Levels (LLE / HLE)"
                      "###abstractionLevels", open)) {
        ImGui::End();
        return req;
    }

    const Palette& pal = palette();
    const auto u32 = ImGui::ColorConvertU32ToFloat4;

    // ── Legende : les cinq niveaux, tout le discours en infobulle ───────
    // La fenetre etait deux paragraphes, une section de commutateurs, trois
    // filtres et une table de prose. Elle est maintenant UNE table ; le
    // raisonnement complet vit dans docs/lle_vs_hle.md et les infobulles.
    {
        static const AbsLevel kAll[] = { AbsLevel::L0, AbsLevel::L1,
                                         AbsLevel::L2, AbsLevel::H1,
                                         AbsLevel::H2 };
        static const char* kTips[] = {
            "Silicon. Internal state machine + cycle timing modelled; a real\n"
            "ROM/firmware dump executes on top and cannot tell the difference.",
            "Chip-faithful. Full register/protocol model at bus timing;\n"
            "firmware-invisible internals deliberately skipped.",
            "Real firmware, host device. The card's real ROM executes, but\n"
            "what it drives is a host implementation.",
            "Synthetic firmware. POM2 hand-assembles a slot ROM: the guest\n"
            "6502 code is real, the register protocol behind it is invented.",
            "Host function. No guest-visible hardware at all."
        };
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine(0.0f, 10.0f);
            levelChip(kAll[i]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kTips[i]);
        }
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextDisabled("LLE = the chip's pins, HLE = the service. "
                            "Full doc: docs/lle_vs_hle.md");
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS " Filter...",
                             search_, sizeof(search_));

    // Lignes vivantes indexees une fois par frame.
    std::unordered_map<std::string, const Row*> live;
    for (const Row& r : snap.rows) live[r.id] = &r;

    // Les quatre frontieres commutables, indexees par id de catalogue :
    // quand une ligne appartient a une paire, son bouton vit DANS la ligne.
    struct PairSide { const ToggleState* t; int idx; };
    std::unordered_map<std::string, PairSide> pair;
    for (const ToggleState& t : snap.toggles) {
        const char* lo = nullptr; const char* hi = nullptr;
        switch (t.id) {
            case AbsToggle::MouseCard:      lo = "mouse";    hi = "mouseaw";     break;
            case AbsToggle::BlockStorage:   lo = "cffa";     hi = "hdv";         break;
            case AbsToggle::PrinterIface:   lo = "grappler"; hi = "printercard"; break;
            case AbsToggle::SerialFirmware: lo = "ccs7710";  hi = "ccs7710_pom2"; break;
            case AbsToggle::Dan2Firmware:   lo = "dan2";     hi = "dan2_pom2";   break;
            case AbsToggle::CompositeVideo: lo = "oe";       hi = "lut";         break;
            case AbsToggle::None:           break;
        }
        if (lo) { pair[lo] = { &t, 0 }; pair[hi] = { &t, 1 }; }
    }

    auto matches = [&](const AbsEntry& e) {
        if (!search_[0]) return true;
        auto hay = std::string(e.subsystem) + " " + e.modelled + " " +
                   e.group + " " + e.files;
        std::string needle(search_);
        auto lower = [](std::string v) {
            for (char& c : v) c = static_cast<char>(std::tolower(
                                      static_cast<unsigned char>(c)));
            return v;
        };
        return lower(hay).find(lower(needle)) != std::string::npos;
    };

    int degraded = 0;

    if (ImGui::BeginTable("##abs", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Level",  ImGuiTableColumnFlags_WidthFixed,
                                3.0f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("Subsystem", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Now",    ImGuiTableColumnFlags_WidthFixed,
                                7.0f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        const char* group = nullptr;
        for (const AbsEntry& e : abstractionCatalog()) {
            auto it = live.find(e.id);
            const Row* r = (it != live.end()) ? it->second : nullptr;
            if (!matches(e)) continue;
            if (r && r->live == Live::Degraded) ++degraded;

            if (!group || std::strcmp(group, e.group) != 0) {
                group = e.group;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(u32(pal.accentDim), "%s", group);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            levelChip(e.level);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(e.subsystem);
            if (ImGui::IsItemHovered()) {
                // Toute la prose de l'ancienne table vit ici.
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(30.0f * ImGui::GetFontSize());
                ImGui::TextColored(u32(levelColour(e.level)), "%s — %s",
                                   levelBadge(e.level), levelName(e.level));
                ImGui::Separator();
                ImGui::TextWrapped("%s", e.modelled);
                ImGui::Spacing();
                ImGui::TextWrapped("Why not lower: %s", e.whyNot);
                ImGui::Spacing();
                ImGui::TextColored(u32(pal.textDim), "%s", e.files);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(2);
            if (!r || r->live == Live::NotApplicable ||
                r->live == Live::Active) {
                ImGui::TextColored(u32(pal.ok), ICON_FA_CIRCLE_CHECK " live");
            } else if (r->live == Live::Degraded) {
                // La raison d'etre de cette colonne : un chemin degrade
                // n'est PAS une panne, c'est pour ca qu'il faut le dire.
                ImGui::TextColored(u32(pal.warn),
                                   ICON_FA_TRIANGLE_EXCLAMATION " %s now",
                                   levelBadge(r->actual));
            } else {
                ImGui::TextColored(u32(pal.textDim), "not plugged");
            }
            if (r && !r->detail.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", r->detail.c_str());

            // ── Le bouton de bascule, quand l'autre niveau existe ───────
            ImGui::TableSetColumnIndex(3);
            auto pit = pair.find(e.id);
            if (pit != pair.end()) {
                const ToggleState& t   = *pit->second.t;
                const int          idx = pit->second.idx;
                const ToggleOption& me = (idx == 0) ? t.low : t.high;
                const bool switchable  = (t.selected >= 0 ||
                                          t.id == AbsToggle::CompositeVideo);
                if (t.selected == idx) {
                    ImGui::TextColored(u32(pal.ok), ICON_FA_CHECK " in use");
                    if (ImGui::IsItemHovered() && !me.why.empty())
                        ImGui::SetTooltip("%s", me.why.c_str());
                } else {
                    ImGui::PushID(e.id);
                    ImGui::BeginDisabled(!me.available || !switchable);
                    char label[96];
                    std::snprintf(label, sizeof(label),
                                  ICON_FA_RIGHT_LEFT " use %s",
                                  levelBadge(me.level));
                    if (ImGui::SmallButton(label)) {
                        req.toggle = t.id;
                        req.option = idx;
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(
                            ImGuiHoveredFlags_AllowWhenDisabled)) {
                        // AllowWhenDisabled : l'explication du grisage est
                        // exactement ce qu'on veut pouvoir lire.
                        if (!switchable)
                            ImGui::SetTooltip("Nothing to switch — neither "
                                              "side is plugged. Add the card "
                                              "in Slot Configuration first.");
                        else if (!me.available && !me.blockedBy.empty())
                            ImGui::SetTooltip("Unavailable — %s",
                                              me.blockedBy.c_str());
                        else
                            ImGui::SetTooltip("%s%s", me.why.c_str(),
                                              t.needsRestart
                                              ? "\n\nSwitching RESTARTS the "
                                                "machine." : "");
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndTable();
    }

    if (degraded > 0) {
        ImGui::TextColored(u32(pal.warn),
                           ICON_FA_TRIANGLE_EXCLAMATION
                           " %d running above its catalogued level "
                           "(a ROM dump is missing — see ROM Status).",
                           degraded);
    }

    ImGui::End();
    return req;
}

} // namespace pom2
