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

// MainWindow — what Help ▸ Report a bug says about the session without
// being asked (BugReportUrl.h builds the issue from it). One concern: turn
// the machine's state into "key: value" facts, most useful first.
//
// The questions a report always needed a round trip for: which machine and
// video standard, which cards in which slots, which disks, at what speed,
// stopped or running, where the CPU was and what was paged in, on which GPU.
// Disks are named by FILE NAME only — a path names the user's folders, and
// the report is about to be posted in public.

#include "MainWindow.h"

#include "Apple2Display.h"
#include "BeamPosition.h"
#include "BugReportUrl.h"
#include "CpuClock.h"
#include "DebugAddress.h"
#include "DriveRack_ImGui.h"
#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SystemProfile.h"

#include "imgui.h"
#include "Pom2GL.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

std::vector<pom2::BugReportFact> MainWindow::bugReportFacts()
{
    std::vector<pom2::BugReportFact> facts;
    char text[160];
    const auto add = [&facts](const char* key, const std::string& value) {
        if (!value.empty()) facts.emplace_back(key, value);
    };

    // ── The machine, under one short lock ────────────────────────────────
    std::string slots, cpu, video, memory, beam;
    bool pal = false, beamView = false;
    {
        auto st = controller->lockState();
        const Memory& mem = st.memory();
        const M6502&  c   = st.cpu();
        pal      = mem.videoStandard() == VideoStandard::PAL;
        beamView = mem.beamView();
        for (int slot = 1; slot <= 7; ++slot) {
            const SlotPeripheral* card = mem.slotBus().peripheral(slot);
            if (!card) continue;
            std::snprintf(text, sizeof text, "%s%d %.*s", slots.empty() ? "" : ", ", slot,
                          static_cast<int>(card->name().size()), card->name().data());
            slots += text;
        }
        const uint16_t pc = c.getProgramCounter();
        std::snprintf(text, sizeof text, "PC $%04X (%s) A $%02X X $%02X Y $%02X SP $%02X P $%02X", pc,
                      mem.debugBank(pc).text().c_str(), c.getAccumulator(), c.getXRegister(),
                      c.getYRegister(), c.getStackPointer(), c.getStatusRegister());
        cpu = text;
        const Memory::DisplayState v = mem.getDisplayState();
        std::snprintf(text, sizeof text, "%s %s %s%s%s%s%s%s", v.textMode ? "TEXT" : "GRAPHICS",
                      v.hiRes ? "HIRES" : "LORES", v.page2 ? "PAGE2" : "PAGE1",
                      v.mixedMode ? " MIXED" : "", v.eightyCol ? " 80COL" : "", v.dhgr ? " DHGR" : "",
                      v.eightyStore ? " 80STORE" : "", v.altChar ? " ALTCHAR" : "");
        video = text;
        std::snprintf(text, sizeof text, "ZP %s, $0200 %s, $2000 %s, $C300 %s, $D000 %s, $E000 %s",
                      mem.debugBank(0x0000).text().c_str(), mem.debugBank(0x0200).text().c_str(),
                      mem.debugBank(0x2000).text().c_str(), mem.debugBank(0xC300).text().c_str(),
                      mem.debugBank(0xD000).text().c_str(), mem.debugBank(0xE000).text().c_str());
        memory = text;
        const pom2::BeamPosition b = pom2::beamPositionAt(mem.videoNow(), mem.videoStandard());
        std::snprintf(text, sizeof text, "V %d H %d", b.line, b.h);
        beam = text;
    }

    // ── Most useful first: the URL keeps only what fits ──────────────────
    const VideoTiming& vt = pom2VideoTiming(pal ? VideoStandard::PAL : VideoStandard::NTSC);
    const int budget = controller->getCyclesPerFrame();
    if (budget == 1'000'000) {
        add("Speed", "MAX (uncapped)");
    } else {
        std::snprintf(text, sizeof text, "%.4g %% of the %s clock",
                      100.0 * static_cast<double>(budget) / vt.cyclesPerFrame, pal ? "PAL" : "NTSC");
        add("Speed", text);
    }
    add("State", controller->getMode() == EmulationController::Mode::Running ? "running" : "stopped");
    add("Slots", slots.empty() ? "none" : slots);

    std::string media;
    for (const pom2::DrivePicture& drive : collectDrivePictures()) {
        if (!drive.loaded) continue;
        const std::string name = std::filesystem::path(drive.path).filename().string();
        std::snprintf(text, sizeof text, "%sS%dD%d %s%s", media.empty() ? "" : "; ", drive.slot,
                      drive.bay + 1, name.c_str(), drive.writeProtected ? " (write-protected)" : "");
        media += text;
    }
    add("Media", media.empty() ? "none" : media);

    add("CPU", cpu);
    add("Video switches", video);
    add("Memory", memory);
    add("Beam", beam + (beamView ? ", beam view on" : ""));

#ifndef __EMSCRIPTEN__
    // The GPU and its driver: half of the display reports are about one.
    const auto glString = [](unsigned name) {
        const unsigned char* s = glGetString(name);
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
    };
    add("GPU", glString(GL_RENDERER));
    add("OpenGL", glString(GL_VERSION));
#endif
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    std::snprintf(text, sizeof text, "%.0f x %.0f, UI scale %.2f, DPI scale %.2f",
                  vp->Size.x, vp->Size.y, uiScale_, dpiScale_);
    add("Window", text);
    return facts;
}
