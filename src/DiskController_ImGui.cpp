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

#include "DiskController_ImGui.h"

#include "StatusLed.h"
#include "imgui.h"

#include <cstdio>

namespace pom2 {

DiskController_ImGui::FrameResult DiskController_ImGui::render(
    const char*          title,
    bool&                open,
    const DriveSnapshot& snap)
{
    FrameResult r;
    if (!open) return r;

    // Window pos/size are owned by the caller (MainWindow's curated startup
    // layout) — we don't fight it from here. A standalone caller can still
    // SetNextWindowSize before invoking us.
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    // ─── Header status LED ───────────────────────────────────────────────
    // grey empty / green disk inserted / red last-insert error.
    {
        const bool err = !snap.diskLoaded && !snap.lastError.empty();
        // Same read-only sense the guest sees: the medium's own WP flag OR
        // the host write-back opt-in being off. Hard-coding `wp=false` here
        // meant this was the one media panel whose LED could never go
        // amber, while the body text right below already said "Read-only".
        const bool wp = snap.fileWriteProtected || !snap.writeBackEnabled;
        pom2::statusLed(snap.diskLoaded, wp, err,
                        snap.diskLoaded ? snap.diskPath.c_str()
                        : err           ? snap.lastError.c_str()
                                        : "No disk inserted");
        ImGui::TextUnformatted(snap.diskLoaded ? "Disk inserted"
                               : err           ? "Insert failed"
                                               : "Empty");
    }

    // ─── PROM state ──────────────────────────────────────────────────────
    if (snap.bootRomLoaded) {
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f),
                           "Boot PROM loaded ($C600-$C6FF)");
    } else {
        ImGui::TextColored(ImVec4(0.85f, 0.4f, 0.4f, 1.0f),
                           "No boot PROM (place disk2.rom in roms/)");
    }
    ImGui::Separator();

    // ─── Drive 1 ─────────────────────────────────────────────────────────
    ImGui::Text("Drive 1");
    ImGui::SameLine();
    // Motor LED — solid red when spinning.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  radius = 6.0f;
        const ImU32  color  = snap.motorOn
            ? IM_COL32(220, 60, 60, 255)
            : IM_COL32(60, 60, 60, 255);
        dl->AddCircleFilled(ImVec2(p.x + radius + 4, p.y + radius + 2), radius, color);
        dl->AddCircle      (ImVec2(p.x + radius + 4, p.y + radius + 2), radius,
                            IM_COL32(0, 0, 0, 255), 0, 1.5f);
        ImGui::Dummy(ImVec2(radius * 2 + 12, radius * 2 + 4));
    }
    ImGui::SameLine();
    ImGui::TextDisabled(snap.motorOn ? "MOTOR" : "idle");

    if (snap.diskLoaded) {
        ImGui::Text("Track: %2d.%d (half-track %d)",
                    snap.track, (snap.halfTrack & 1) ? 5 : 0,
                    snap.halfTrack);
        ImGui::Text("Buffer pos: %d / 6656", snap.trackPos);
        ImGui::TextWrapped("Image: %s", snap.diskPath.c_str());
        // Say WHY the guest sees a read-only disk. Without this the app
        // just reports "disk is write-protected" and nothing connects that
        // to a host checkbox — Print Shop refusing to save its own Setup
        // looked like a bug in the emulator for exactly this reason.
        if (snap.fileWriteProtected) {
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                               "Read-only: the image itself is "
                               "write-protected (WOZ/2IMG flag)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Physical write-protect of the medium — "
                                  "honoured whatever the write-back setting "
                                  "is, exactly like the notch on a real "
                                  "sleeve.");
        } else if (!snap.writeBackEnabled) {
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                               "Read-only: write-back is off — the guest "
                               "will see a write-protected disk");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off by default so running a program "
                                  "never silently rewrites your image "
                                  "file.\nThe drive reports write-protect "
                                  "rather than accepting writes and "
                                  "discarding them on eject.\nTick "
                                  "\"Write-back\" below to let this disk be "
                                  "saved.");
        } else {
            ImGui::TextDisabled("Writable — changes are saved on eject.");
        }
    } else {
        ImGui::TextDisabled("No disk inserted.");
        // If a previous insert attempt failed, surface the underlying
        // reason here so the user can act on it (rename, convert, etc.)
        // rather than guessing why nothing happened.
        if (!snap.lastError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                IM_COL32(220, 70, 70, 255));
            ImGui::TextWrapped("Last error: %s", snap.lastError.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();

    // ─── Buttons ─────────────────────────────────────────────────────────
    if (ImGui::Button("Insert .dsk...")) {
        insertDialogOpen = true;
        if (dialogPath.empty()) dialogPath = "disks_5.4/";
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!snap.diskLoaded);
    if (ImGui::Button("Eject")) {
        r.requestEject = true;
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // Direct boot — bypasses any host-side weirdness in how PR#6 is
    // dispatched. Sets PC = $C600 and lets the boot PROM take over.
    ImGui::BeginDisabled(!snap.bootRomLoaded || !snap.diskLoaded);
    if (ImGui::Button("Boot disk (jump $C600)")) {
        r.requestBoot = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("or type PR#6 in Applesoft");

    // Auto-turbo: while the motor is spinning the host can crank the CPU
    // way past 1 MHz so a 15 s real-time DOS 3.3 boot collapses into ~1 s.
    // The LSS still serves one nibble per 32 emulated cycles, so the disk
    // "rotates" proportionally faster — no nibbles missed.
    bool turbo = snap.turboWhileMotor;
    if (ImGui::Checkbox("Turbo while motor spinning", &turbo)) {
        r.turboToggleChanged = true;
        r.turboNewValue      = turbo;
    }
    if (snap.turboActive) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "(active)");
    }

    // Write-back opt-in. OFF by default — turning it on lets the
    // emulator save modified sectors back to the source .dsk/.do/.po/
    // .nib file when the disk is ejected. Off → DOS sees a write-
    // protect error before scrambling the in-memory nibble buffer.
    bool writeBack = snap.writeBackEnabled;
    if (ImGui::Checkbox("Write-back (save on eject)", &writeBack)) {
        r.writeBackToggleChanged = true;
        r.writeBackNewValue      = writeBack;
    }
    if (snap.hasUnsavedChanges) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "(unsaved)");
    }

    // ─── Disk library ───────────────────────────────────────────────────
    // One-click insert + cold-boot for any .dsk found in disks_5.4/.
    ImGui::Separator();
    ImGui::TextUnformatted("Library:");
    ImGui::SameLine();
    ImGui::TextDisabled("(left-click: insert + boot — right-click: insert only)");

    if (snap.library.empty()) {
        ImGui::TextDisabled("  (drop .dsk / .do files into disks_5.4/ to populate)");
    } else {
        // 540 px (3× the original 180) — extended downward by half its
        // previous height (360 + 180). If the surrounding Disk II panel
        // is shorter than this child, the panel grows an outer scrollbar
        // — by design; nothing else in the panel layout is touched.
        ImGui::BeginChild("##disk_lib", ImVec2(0, 540), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& entry : snap.library) {
            const bool current = (entry.fullPath == snap.diskPath);
            // Currently inserted disk gets a visual marker; clicking it
            // again re-boots from track 0.
            const std::string label = (current ? "* " : "  ") + entry.displayName;
            // PushID on the full path, like the 3.5" panel: ImGui derives a
            // Selectable's ID from its LABEL, so two library entries with the
            // same basename in different folders shared one ID — hovering or
            // clicking either one drove the other's row.
            ImGui::PushID(entry.fullPath.c_str());
            if (ImGui::Selectable(label.c_str(), current)) {
                r.requestInsertAndBoot = entry.fullPath;
            }
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                r.requestInsertOnly = entry.fullPath;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::End();
    return r;
}

} // namespace pom2
