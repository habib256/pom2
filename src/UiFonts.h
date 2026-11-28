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

// UiFonts — the fonts POM2's Dear ImGui UI is drawn with, loaded in ONE
// place: main.cpp at start-up, and the probes that screenshot a window off
// screen (tests/heatmap_ui_probe.cpp) so their PNGs show what the user sees
// — with the FontAwesome icons merged, not "?" (2026-11-28: the heatmap
// probe's toolbar read "? Clear", "? Freeze").

#ifndef POM2_UI_FONTS_H
#define POM2_UI_FONTS_H

struct ImFont;
struct ImGuiIO;

namespace pom2 {

/// Add to `io.Fonts`, in this order: the UI font (DejaVu Sans, Latin-1 and
/// some punctuation; ImGui's default when no TTF is found), the FontAwesome
/// 6 solid icons merged into it, then the fixed-pitch DejaVu Sans Mono.
/// Returns the mono font (null when missing) for `setMonoFont`. Missing
/// files are reported on stderr and leave the UI usable.
ImFont* loadUiFonts(ImGuiIO& io);

}  // namespace pom2

#endif  // POM2_UI_FONTS_H
