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

#include "LeChatMauve_ImGui.h"

#include "imgui.h"

#include <cstdio>

namespace pom2 {

namespace {

const char* modeLabel(LeChatMauveCard::RenderMode m)
{
    using Mode = LeChatMauveCard::RenderMode;
    switch (m) {
        case Mode::BW560:     return "BW560 (mono strict)";
        case Mode::Mixed:     return "Mixed (140C + 560M)";
        case Mode::Chunky160: return "Chunky 160 (Video-7)";
        case Mode::COL140:    return "COL140 (RGB 16 colors)";
    }
    return "?";
}

const char* modeShort(LeChatMauveCard::RenderMode m)
{
    using Mode = LeChatMauveCard::RenderMode;
    switch (m) {
        case Mode::BW560:     return "BW560";
        case Mode::Mixed:     return "Mixed";
        case Mode::Chunky160: return "Chunky";
        case Mode::COL140:    return "COL140";
    }
    return "?";
}

// ABGR → ImU32 (ImU32 is also AABBGGRR on little-endian, so this is a
// pass-through; kept as a function for documentation).
ImU32 toImU32(uint32_t abgr) { return abgr; }

const char* dhgrLabel(LeChatMauveCard::DhgrMode m)
{
    using D = LeChatMauveCard::DhgrMode;
    switch (m) {
        case D::BW560:     return "BW560";
        case D::COL140:    return "COL140";
        case D::Mixed:     return "Mixed 140C/560M";
        case D::Chunky160: return "160 chunky";
        case D::COL280A:   return "COL280A";
        case D::COL280B:   return "COL280B";
        case D::CP280:     return "CP280 (fg/bg)";
        case D::Blank:     return "blank";
    }
    return "?";
}

// The Eve's eight switches, in address order ($C0B0 + 2i = off, +1 = on).
const char* kEveSwitchNames[8] = {
    "ENHRCPREG", "HR1", "HR2", "HR3", "TXT16", "TXTGREEN", "LOCKCPREG", "LOCKRES",
};

// 6-colour HGR Chat Mauve palette duplicated locally so the panel can
// draw swatches without pulling in Apple2Display.h. Keep these in sync
// with kChatMauveHGR in Apple2Display.cpp (Feline capture: magenta, green,
// blue, orange).
constexpr uint32_t kHGRBank0[4] = {
    0xFF000000, 0xFFD11AAA, 0xFF2CE66F, 0xFFFFFFFF,
};
constexpr uint32_t kHGRBank1[4] = {
    0xFF000000, 0xFFB58A00, 0xFF4772FF, 0xFFFFFFFF,
};
const char* kHGRBank0Names[4] = { "blk", "vlt", "grn", "wht" };
const char* kHGRBank1Names[4] = { "blk", "blu", "org", "wht" };

void drawSwatch(ImDrawList* dl, ImVec2 pos, float size, uint32_t color,
                bool border = true)
{
    const ImVec2 a = pos;
    const ImVec2 b{ pos.x + size, pos.y + size };
    dl->AddRectFilled(a, b, toImU32(color));
    if (border) dl->AddRect(a, b, IM_COL32(0, 0, 0, 200), 0, 0, 1.0f);
}

} // namespace

LeChatMauve_ImGui::FrameResult LeChatMauve_ImGui::render(
    const char*     title,
    bool&           open,
    const Snapshot& snap)
{
    FrameResult r;
    if (!open) return r;

    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    // ─── Plug status + variant ───────────────────────────────────────────
    if (snap.plugged) {
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f),
                           "Plugged in slot %d (Péritel RGB)", snap.slot);
    } else {
        ImGui::TextColored(ImVec4(0.85f, 0.4f, 0.4f, 1.0f),
                           "Not plugged");
    }
    {
        using V = LeChatMauveCard::Variant;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo("Variant", LeChatMauveCard::variantLabel(snap.variant))) {
            for (int i = 0; i < LeChatMauveCard::kVariantCount; ++i) {
                const V v = static_cast<V>(i);
                if (ImGui::Selectable(LeChatMauveCard::variantLabel(v), v == snap.variant)) {
                    r.requestVariant = true;
                    r.variantTo      = v;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();

    // ─── Mode latch (F2 F1) + what the decoder makes of it ───────────────
    ImGui::Text("Mode latch : %s", modeLabel(snap.mode));
    ImGui::Text("FIFO bits  : [%d][%d]",
                (snap.fifoBits >> 1) & 1, snap.fifoBits & 1);
    ImGui::Text("DHGR decode: %s", dhgrLabel(snap.dhgrMode));
    // The classic trap: Extasie/Arlequin clock the Féline mixed mode, which
    // the Eve and the RVB Graph never had (Manuel Arlequin) — say so where
    // the user is looking instead of silently showing the COL140 fallback.
    if (snap.mode == LeChatMauveCard::RenderMode::Mixed &&
        (snap.variant == LeChatMauveCard::Variant::Eve ||
         snap.variant == LeChatMauveCard::Variant::RvbGraph)) {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
            "latch = MIXED, but this model has no mixed mode\n"
            "(COL140 fallback) — Extasie/Arlequin need the Feline");
    }

    // Soft-switch line indicators (data + clock). Useful when debugging
    // an Arlequin-style setup sequence — you can step the CPU and watch
    // the two lines toggle.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float  rad = 5.0f;
        const ImU32  on  = IM_COL32(80, 220, 80, 255);
        const ImU32  off = IM_COL32(60, 60,  60, 255);
        const ImU32  edge = IM_COL32(0, 0, 0, 255);

        dl->AddCircleFilled(ImVec2(p0.x + rad + 2,  p0.y + rad + 2), rad,
                            snap.eightyCol ? on : off);
        dl->AddCircle      (ImVec2(p0.x + rad + 2,  p0.y + rad + 2), rad,
                            edge, 0, 1.5f);
        dl->AddCircleFilled(ImVec2(p0.x + rad + 80, p0.y + rad + 2), rad,
                            snap.an3High ? on : off);
        dl->AddCircle      (ImVec2(p0.x + rad + 80, p0.y + rad + 2), rad,
                            edge, 0, 1.5f);
        ImGui::Dummy(ImVec2(160, rad * 2 + 4));
        ImGui::SameLine(28);
        ImGui::TextDisabled("80COL");
        ImGui::SameLine(106);
        ImGui::TextDisabled("AN3");
    }

    ImGui::Separator();

    // ─── HGR palette swatches ────────────────────────────────────────────
    // Visual reminder of which 6 colors the Chat Mauve emits in standard
    // HGR mode, split into the two MSB-selected banks. Helps debugging:
    // if a program looks "wrong" in Chat Mauve mode, the operator can
    // spot at a glance whether they're seeing bank 0 (violet/green) or
    // bank 1 (blue/orange).
    ImGui::TextDisabled("HGR palette (6 colors, MSB-banked):");
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float swSize = 18.0f;
        const float swGap  = 2.0f;
        const float labelH = ImGui::GetTextLineHeight();
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // Bank 0 (MSB=0) row.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y));
        ImGui::TextUnformatted("MSB=0:");
        for (int i = 0; i < 4; ++i) {
            const float x = origin.x + 70.0f + i * (swSize + swGap + 22.0f);
            const float y = origin.y - 1.0f;
            drawSwatch(dl, ImVec2(x, y), swSize, kHGRBank0[i]);
            dl->AddText(ImVec2(x + swSize + 2.0f, y + 1.0f),
                        IM_COL32(180, 180, 180, 255), kHGRBank0Names[i]);
        }
        ImGui::Dummy(ImVec2(0.0f, swSize + 2.0f));

        // Bank 1 (MSB=1) row.
        const ImVec2 row1 = ImGui::GetCursorScreenPos();
        ImGui::TextUnformatted("MSB=1:");
        for (int i = 0; i < 4; ++i) {
            const float x = row1.x + 70.0f + i * (swSize + swGap + 22.0f);
            const float y = row1.y - 1.0f;
            drawSwatch(dl, ImVec2(x, y), swSize, kHGRBank1[i]);
            dl->AddText(ImVec2(x + swSize + 2.0f, y + 1.0f),
                        IM_COL32(180, 180, 180, 255), kHGRBank1Names[i]);
        }
        ImGui::Dummy(ImVec2(0.0f, swSize + 2.0f));

        // Pair labels above (00 / 01 / 10 / 11).
        const ImVec2 lbl = ImGui::GetCursorScreenPos();
        ImGui::TextDisabled("        00     01     10     11");
        (void)lbl;
        (void)labelH;
    }

    ImGui::Separator();

    // ─── UI overrides — force a render mode ──────────────────────────────
    ImGui::TextDisabled("Force mode (UI override):");
    using Mode = LeChatMauveCard::RenderMode;
    const Mode modes[] = { Mode::BW560, Mode::Mixed, Mode::Chunky160, Mode::COL140 };
    for (Mode m : modes) {
        // PushID(mode): the label carries the "* " selection marker, so each
        // radio's ImGui ID changed as the selection moved between them — the
        // widget the user was pressing became a different widget mid-click.
        ImGui::PushID(static_cast<int>(m));
        const bool active = (snap.mode == m);
        char label[32];
        std::snprintf(label, sizeof(label), "%s%s",
                      active ? "* " : "  ", modeShort(m));
        if (ImGui::RadioButton(label, active)) {
            r.requestOverride = true;
            r.overrideTo      = m;
        }
        ImGui::PopID();
    }

    ImGui::Separator();

    if (ImGui::Button("Reset FIFO -> COL140")) {
        r.requestReset = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(matches Apple II RESET)");

    ImGui::Separator();

    // Dragon Wars compatibility toggle. The game encodes its DHGR Mixed
    // bit-7 selector with the opposite polarity to the Video-7 spec; this
    // checkbox XORs bit 7 at decode time, restoring the intended rendering.
    {
        bool inv = snap.invertBit7;
        if (ImGui::Checkbox("Invert bit 7 (Dragon Wars compat)", &inv)) {
            r.requestInvertBit7 = true;
            r.invertBit7To      = inv;
        }
    }

    // ─── Eve registers ($C0B0-$C0BF) ────────────────────────────────────
    if (snap.variant == LeChatMauveCard::Variant::Eve) {
        ImGui::Separator();
        ImGui::TextDisabled("Eve switches ($C0B0-$C0BF, click = STA $C0Bx):");
        for (int i = 0; i < 8; ++i) {
            const bool on = (snap.eveSwitches >> i) & 1u;
            bool v = on;
            char label[48];
            std::snprintf(label, sizeof(label), "%-9s $C0B%X/%X", kEveSwitchNames[i],
                          2 * i, 2 * i + 1);
            if (ImGui::Checkbox(label, &v)) {
                r.requestEveSwitch = true;
                r.eveSwitch        = static_cast<LeChatMauveCard::EveSwitch>(i);
                r.eveSwitchTo      = v;
            }
        }
        ImGui::Text("CPREG = $%02X  (dot %d, background %d)", snap.cpreg,
                    snap.cpreg & 0x0F, (snap.cpreg >> 4) & 0x0F);
        ImGui::TextDisabled("auto-write to aux: text %s, HGR %s",
                            snap.auxShadowText ? "ON" : "off",
                            snap.auxShadowHgr  ? "ON" : "off");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Notes:");
    ImGui::TextDisabled(" - HGR: LCM rule (2-bit cell, 3-bit window), no fringing");
    ImGui::TextDisabled(" - Lo-res: 16 colors with 2 distinct grays");
    ImGui::TextDisabled(" - DHGR: latch -> COL140 / BW560 / Mixed (160 = Video-7 only)");
    ImGui::TextDisabled(" - Feline / //c: AN3 off = HGR mono; no color text");
    ImGui::TextDisabled(" - Eve: table IX-1 (HR1-3), TXT16 hi=bg, CPREG -> aux");
    ImGui::TextDisabled(" - Video-7: F/B text + F/B HGR (hi=fg), 160 chunky");
    ImGui::TextDisabled(" - Mode actif via menu Display -> Le Chat Mauve");

    ImGui::End();
    return r;
}

} // namespace pom2
