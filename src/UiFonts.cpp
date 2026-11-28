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

// Moved out of main.cpp unchanged (2026-11-28), so a probe loads the same
// fonts as the app. See UiFonts.h.

#include "UiFonts.h"

#include "IconsFontAwesome6.h"
#include "ResourcePaths.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace pom2 {

ImFont* loadUiFonts(ImGuiIO& io)
{
    // Base UI font — Proggy Clean (ImGui's default) is a bitmap that
    // only covers ASCII, so anything past U+007F (em-dash "—", en-dash
    // "–", curly quotes, accented letters used in POM2's localised
    // strings, ellipsis "…") renders as '?'. Try to load a real TTF
    // with Latin-1 Supplement + a handful of General Punctuation
    // glyphs so the menus / status text / tooltips read properly.
    //
    // Probe order: bundled font (none today), then a few common
    // distribution paths for DejaVu Sans (Debian/Ubuntu, Fedora,
    // Arch). Last resort = AddFontDefault → users see '?' for the
    // missing glyphs but the rest of the UI still works.
    //
    // The explicit SizePixels matters: since ImGui added the
    // ImFontFlags_ImplicitRefSize check, merging FontAwesome (which
    // requests 14.0f below) into a destination that uses implicit
    // ref size hard-asserts at AddFont. Both branches here pick an
    // explicit size for that reason.
    {
        const char* baseCandidates[] = {
            // POM2-bundled (drop a TTF into fonts/ to override).
            "fonts/DejaVuSans.ttf",
            "../fonts/DejaVuSans.ttf",
            "../../fonts/DejaVuSans.ttf",
            // System locations seen in the wild.
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/Library/Fonts/Arial Unicode.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
        };
        // findResource resolves the bundled "fonts/…" entries against the
        // executable-relative / FHS roots too (so an installed binary finds
        // its TTF), and returns the absolute system paths verbatim when
        // they exist. See ResourcePaths.h.
        std::string basePath;
        for (const char* c : baseCandidates) {
            std::string r = findResource(c);
            if (!r.empty()) { basePath = r; break; }
        }
        // Latin-1 Supplement + selected General Punctuation. Listed
        // pair-by-pair (ImGui ranges are inclusive [from, to] pairs
        // terminated with 0). Covers: ASCII printables + accented
        // letters (é, à, ç, ü, ö, …), en/em dash, curly single + double
        // quotes, ellipsis, non-breaking space.
        static const ImWchar baseRanges[] = {
            0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
            0x2013, 0x2014,   // – (en dash) — (em dash)
            0x2018, 0x201D,   // ' ' " "  (curly quotes)
            0x2022, 0x2022,   // • (bullet)
            0x2026, 0x2026,   // … (ellipsis)
            0x20AC, 0x20AC,   // € (euro sign — used in localised strings)
            0,
        };
        if (!basePath.empty()) {
            ImFontConfig baseCfg;
            baseCfg.SizePixels    = 14.0f;
            baseCfg.OversampleH   = 2;
            baseCfg.OversampleV   = 2;
            baseCfg.PixelSnapH    = false;
            if (!io.Fonts->AddFontFromFileTTF(basePath.c_str(), 14.0f,
                                              &baseCfg, baseRanges)) {
                // TTF load failure → fall back to default so the UI
                // still comes up.
                ImFontConfig defCfg;
                defCfg.SizePixels = 13.0f;
                io.Fonts->AddFontDefault(&defCfg);
            }
        } else {
            std::fprintf(stderr,
                "POM2: no Unicode TTF found (tried DejaVu Sans + system "
                "paths) — em-dashes, curly quotes and accented chars "
                "will render as '?'. Drop DejaVuSans.ttf into fonts/ to "
                "fix.\n");
            ImFontConfig defCfg;
            defCfg.SizePixels = 13.0f;
            io.Fonts->AddFontDefault(&defCfg);
        }
    }
    {
        const char* candidates[] = {
            "fonts/fa-solid-900.ttf",
            "../fonts/fa-solid-900.ttf",
            "../../fonts/fa-solid-900.ttf",
        };
        std::string fontPath;
        for (const char* c : candidates) {
            std::string r = findResource(c);
            if (!r.empty()) { fontPath = r; break; }
        }
        if (!fontPath.empty()) {
            ImFontConfig iconsConfig;
            iconsConfig.MergeMode        = true;
            iconsConfig.PixelSnapH       = true;
            iconsConfig.GlyphMinAdvanceX = 15.0f;
            static const ImWchar iconsRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
            if (!io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 14.0f,
                                              &iconsConfig, iconsRanges)) {
                std::fprintf(stderr,
                    "POM2: failed to load %s — deck icons will render as '?'\n",
                    fontPath.c_str());
            }
        } else {
            std::fprintf(stderr,
                "POM2: fa-solid-900.ttf not found in fonts/ — deck icons will render as '?'\n");
        }
    }
    // The fixed-pitch font for the debugger's columns (address, bytes,
    // mnemonic). Added AFTER the icon merge so the merge lands in the UI
    // font, and registered with Pom2Theme so a panel reaches it through
    // ScopedMonoFont. Missing → null, and those panels keep the UI font.
    {
        const char* monoCandidates[] = {
            "fonts/DejaVuSansMono.ttf",
            "../fonts/DejaVuSansMono.ttf",
            "../../fonts/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        };
        std::string monoPath;
        for (const char* c : monoCandidates) {
            std::string r = findResource(c);
            if (!r.empty()) { monoPath = r; break; }
        }
        ImFont* mono = nullptr;
        if (!monoPath.empty()) {
            ImFontConfig monoCfg;
            monoCfg.SizePixels  = 14.0f;
            monoCfg.OversampleH = 2;
            monoCfg.OversampleV = 2;
            mono = io.Fonts->AddFontFromFileTTF(monoPath.c_str(), 14.0f, &monoCfg);
        }
        if (!mono)
            std::fprintf(stderr,
                "POM2: DejaVuSansMono.ttf not found in fonts/ — the debugger's "
                "columns will not line up\n");
        return mono;
    }
}

}  // namespace pom2
