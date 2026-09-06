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

// MainWindow_Chrome — the window furniture: the menu bar, the status bar and
// the command palette.
//
// Everything here is a VIEW over state owned elsewhere, plus one dispatcher.
// `runCommand` is the single place a palette id becomes an action, and the
// panel registry (PanelCatalog.h / PanelRegistry.h) is the single list of
// panels the menus and the palette both derive from — neither enumerates
// panels itself, which is what stopped the two drifting apart.

#include "MainWindow.h"

#include "AudioCoordinator.h"
#include "AudioDevice.h"
#include "CassetteDevice.h"
#include "CommandPalette_ImGui.h"
#include "DebugCoordinator.h"
#include "Debugger.h"
#include "DevicePanelCoordinator.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "Disk35Controller_ImGui.h"
#include "EmulationController.h"
#include "HdvController_ImGui.h"
#include "ImageWriter.h"
#include "MouseCoordinator.h"
#include "PrinterCoordinator.h"
#include "ProDOSHardDiskCard.h"
#include "SlotConfigurationCoordinator.h"
#include "StorageCoordinator.h"
#include "IconsFontAwesome6.h"
#include "Logger.h"
#include "Memory.h"
#include "PanelCatalog.h"
#include "PanelRegistry.h"
#include "Pom2Theme.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "StatusLed.h"
#include "SystemProfile.h"

#include "imgui.h"
#include "imgui_internal.h"   // BeginViewportSideBar, ImMax
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

void MainWindow::noteLibraryRecent(const std::string& path)
{
    if (path.empty()) return;
    auto it = std::find(libraryRecents_.begin(), libraryRecents_.end(), path);
    if (it != libraryRecents_.end()) libraryRecents_.erase(it);
    libraryRecents_.insert(libraryRecents_.begin(), path);
    if (libraryRecents_.size() > kMaxLibraryRecents)
        libraryRecents_.resize(kMaxLibraryRecents);
}

void MainWindow::openCommandPalette()
{
    if (cmdPalette) cmdPalette->open();
}

void MainWindow::renderCommandPalette()
{
    if (!cmdPalette || !cmdPalette->isOpen()) return;

    using Cmd = pom2::CommandPalette_ImGui::Command;
    std::vector<Cmd> cmds;
    cmds.reserve(80);

    auto add = [&cmds](const char* id, const char* cat, std::string label,
                       const char* shortcut = "", bool enabled = true,
                       bool checked = false) {
        Cmd c;
        c.id       = id;
        c.category = cat;
        c.label    = std::move(label);
        c.shortcut = shortcut;
        c.enabled  = enabled;
        c.checked  = checked;
        cmds.push_back(std::move(c));
    };

    const auto mode = controller->getMode();

    // ── Machine ──────────────────────────────────────────────────────────
    add("machine.run",      "Machine", "Run", "",
        mode != EmulationController::Mode::Running);
    add("machine.pause",    "Machine", "Pause", "",
        mode == EmulationController::Mode::Running);
    add("machine.step",     "Machine", "Step one instruction", "",
        mode != EmulationController::Mode::Running);
    add("machine.reset",    "Machine", "Reset (Ctrl-Reset)", "F11");
    add("machine.hardreset","Machine", "Hard reset", "F12");
    add("machine.coldboot", "Machine", "Cold boot (wipe RAM)");
    add("machine.screenshot","Machine","Save screenshot", "F9");
    add("printer.dumpscreen","Machine","Print screen (dump to printer)");
    add("view.kiosk", "View",
        kiosk_ ? "Leave full screen (kiosk)" : "Full screen (kiosk)",
        "Ctrl+Alt+F / F10", true, kiosk_);
    add("view.mousegrab", "View",
        mouseGrabbed_ ? "Release mouse capture" : "Capture mouse",
        "Ctrl+Alt+G", mouseCoordinator_->capture().plugged(),
        mouseGrabbed_);

    // Speed buckets, labelled with the real clock so "2x" isn't abstract.
    {
        const VideoTiming& vt = pom2VideoTiming(controller->getVideoStandard());
        const int cur = controller->getCyclesPerFrame();
        const double mhz = static_cast<double>(vt.cyclesPerFrame) * vt.refreshHz / 1e6;
        char buf[64];
        std::snprintf(buf, sizeof buf, "Speed 1x (%.2f MHz)", mhz);
        add("speed.1x", "Machine", buf, "", true, cur == vt.cyclesPerFrame);
        std::snprintf(buf, sizeof buf, "Speed 2x (%.2f MHz)", mhz * 2);
        add("speed.2x", "Machine", buf, "", true, cur == vt.cyclesPerFrame * 2);
        std::snprintf(buf, sizeof buf, "Speed 4x (%.2f MHz)", mhz * 4);
        add("speed.4x", "Machine", buf, "", true, cur == vt.cyclesPerFrame * 4);
        add("speed.max", "Machine", "Speed MAX (uncapped)", "", true,
            cur == 1'000'000);
    }

    // ── Profiles ─────────────────────────────────────────────────────────
    for (pom2::SystemProfile p : pom2::allProfiles()) {
        const auto& cfg = pom2::profileConfig(p);
        add(("profile." + std::to_string(static_cast<int>(p))).c_str(),
            "Profile", std::string(cfg.displayName), "", true,
            p == activeProfile);
    }

    // ── Display ──────────────────────────────────────────────────────────
    {
        const auto cur = display->getHiResMode();
        auto pipe = [&](const char* id, const char* label,
                        Apple2Display::HiResMode m, bool enabled = true) {
            add(id, "Display", label, "", enabled, cur == m);
        };
        pipe("disp.ntsc",     "NTSC (MAME)",  Apple2Display::HiResMode::ColorNTSC);
        pipe("disp.ntscmed",  "NTSC (MAME) medium", Apple2Display::HiResMode::ColorCompMedium);
        pipe("disp.ntsc4bit", "NTSC (MAME) 4-bit square", Apple2Display::HiResMode::ColorComp4Bit);
        pipe("disp.oegpu",    "Composite (OpenEmulator, GPU)", Apple2Display::HiResMode::ColorCompositeOE);
        pipe("disp.oecpu",    "Composite (OpenEmulator, CPU)", Apple2Display::HiResMode::ColorCompositeOECpu);
        pipe("disp.applewin", "AppleWin NTSC (TV line blur)", Apple2Display::HiResMode::ColorAppleWin);
        pipe("disp.rgb",      "RGB card - Le Chat Mauve", Apple2Display::HiResMode::ChatMauveRGB,
             devicePanelCoordinator_->captureInventory().chatMauvePlugged());
        pipe("disp.mono",     "Monochrome white", Apple2Display::HiResMode::MonoWhite);
        pipe("disp.green",    "Monochrome green (P31)", Apple2Display::HiResMode::MonoGreen);
        pipe("disp.amber",    "Monochrome amber", Apple2Display::HiResMode::MonoAmber);
        add("disp.aspect.square",  "Display", "Aspect: square pixels", "", true,
            aspectMode == AspectMode::Square);
        add("disp.aspect.crt43",   "Display", "Aspect: 4:3 CRT shape", "", true,
            aspectMode == AspectMode::Crt43);
        add("disp.aspect.integer", "Display", "Aspect: integer scale", "", true,
            aspectMode == AspectMode::Integer);
        add("disp.crttoggle", "Display", "Toggle CRT effects", "", true,
            crtEffectsEnabled);
    }

    // ── Layout + interface ───────────────────────────────────────────────
    add("layout.reset",     "Layout", "Reset to default layout");
    add("layout.emulation", "Layout", "Emulation layout");
    add("layout.debug",     "Layout", "Debug layout");
    add("layout.audio",     "Layout", "Audio layout");
    {
        std::size_t n = 0;
        const pom2::UiAccent* accents = pom2::allAccents(n);
        for (std::size_t i = 0; i < n; ++i) {
            add((std::string("accent.") + pom2::accentKey(accents[i])).c_str(),
                "Interface", std::string("Accent: ") +
                pom2::accentLabel(accents[i]), "", true,
                uiAccent_ == accents[i]);
        }
        add("ui.zoomin",  "Interface", "Zoom in");
        add("ui.zoomout", "Interface", "Zoom out");
        add("ui.zoom100", "Interface", "Zoom reset to 100%");
    }

    // ── Panels ───────────────────────────────────────────────────────────
    // One line, and it is the point of the exercise: this list used to be 38
    // hand-written entries here and 38 more in runCommand's dispatch table,
    // with a third copy of the same names in the menus. All three are now
    // views of PanelCatalog.h + registerPanels().
    forEachPanelCommand([&add](const char* id, const std::string& label,
                               const char* shortcut, bool enabled, bool checked) {
        add(id, "Panel", label, shortcut, enabled, checked);
    });

    // ── Media ────────────────────────────────────────────────────────────
    add("media.ejectall", "Media", "Eject all disks");

    cmdPalette->setCommands(std::move(cmds));
    const auto r = cmdPalette->render();
    if (r.executed) runCommand(r.commandId);
}

void MainWindow::runCommand(const std::string& id)
{
    auto toggle = [](bool& f) { f = !f; };

    // Machine
    if (id == "machine.run")        { controller->setMode(EmulationController::Mode::Running); return; }
    if (id == "machine.pause")      { controller->setMode(EmulationController::Mode::Stopped); return; }
    if (id == "machine.step")       { controller->requestStep(); return; }
    if (id == "machine.reset")      { controller->softReset();  return; }
    if (id == "machine.hardreset")  { controller->hardReset();  return; }
    if (id == "machine.coldboot")   { controller->coldBoot();   return; }
    if (id == "machine.screenshot") { saveScreenshot();         return; }
    if (id == "printer.dumpscreen") { dumpScreenToPrinter();    return; }
    if (id == "view.kiosk")        { toggleKioskMode();        return; }
    if (id == "view.mousegrab")    { toggleMouseGrab();        return; }

    if (id.rfind("speed.", 0) == 0) {
        const VideoTiming& vt = pom2VideoTiming(controller->getVideoStandard());
        if      (id == "speed.1x")  controller->setCyclesPerFrame(vt.cyclesPerFrame);
        else if (id == "speed.2x")  controller->setCyclesPerFrame(vt.cyclesPerFrame * 2);
        else if (id == "speed.4x")  controller->setCyclesPerFrame(vt.cyclesPerFrame * 4);
        else if (id == "speed.max") controller->setCyclesPerFrame(1'000'000);
        return;
    }

    if (id.rfind("profile.", 0) == 0) {
        const int idx = std::atoi(id.c_str() + 8);
        for (pom2::SystemProfile p : pom2::allProfiles())
            if (static_cast<int>(p) == idx) { applyProfile(p); return; }
        return;
    }

    // Display
    if (id == "disp.ntsc")     { display->setHiResMode(Apple2Display::HiResMode::ColorNTSC); return; }
    if (id == "disp.ntscmed")  { display->setHiResMode(Apple2Display::HiResMode::ColorCompMedium); return; }
    if (id == "disp.ntsc4bit") { display->setHiResMode(Apple2Display::HiResMode::ColorComp4Bit); return; }
    if (id == "disp.oegpu")    { display->setHiResMode(Apple2Display::HiResMode::ColorCompositeOE); return; }
    if (id == "disp.oecpu")    { display->setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu); return; }
    if (id == "disp.applewin") {
        display->setAppleWinSubMode(Apple2Display::AppleWinSubMode::Tv);
        display->setHiResMode(Apple2Display::HiResMode::ColorAppleWin);
        return;
    }
    if (id == "disp.rgb")   { display->setHiResMode(Apple2Display::HiResMode::ChatMauveRGB); return; }
    if (id == "disp.mono")  { display->setHiResMode(Apple2Display::HiResMode::MonoWhite); return; }
    if (id == "disp.green") { display->setHiResMode(Apple2Display::HiResMode::MonoGreen); return; }
    if (id == "disp.amber") { display->setHiResMode(Apple2Display::HiResMode::MonoAmber); return; }
    if (id == "disp.aspect.square")  { aspectMode = AspectMode::Square;  return; }
    if (id == "disp.aspect.crt43")   { aspectMode = AspectMode::Crt43;   return; }
    if (id == "disp.aspect.integer") { aspectMode = AspectMode::Integer; return; }
    if (id == "disp.crttoggle")      { toggle(crtEffectsEnabled);        return; }

    // Layout + interface
    if (id == "layout.reset")     { pendingDockLayout_ = DockLayout::Reset;     dockLayoutRequested_ = true; return; }
    if (id == "layout.emulation") { pendingDockLayout_ = DockLayout::Emulation; dockLayoutRequested_ = true; return; }
    if (id == "layout.debug")     { pendingDockLayout_ = DockLayout::Debug;     dockLayoutRequested_ = true; return; }
    if (id == "layout.audio")     { pendingDockLayout_ = DockLayout::Audio;     dockLayoutRequested_ = true; return; }
    if (id.rfind("accent.", 0) == 0) {
        uiAccent_ = pom2::accentFromKey(id.c_str() + 7);
        applyUiTheme();
        return;
    }
    if (id == "ui.zoomin")  { uiScale_ = std::clamp(uiScale_ + pom2::kUiScaleStep * 2.0f, pom2::kUiScaleMin, pom2::kUiScaleMax); applyUiTheme(); return; }
    if (id == "ui.zoomout") { uiScale_ = std::clamp(uiScale_ - pom2::kUiScaleStep * 2.0f, pom2::kUiScaleMin, pom2::kUiScaleMax); applyUiTheme(); return; }
    if (id == "ui.zoom100") { uiScale_ = 1.0f; applyUiTheme(); return; }

    // Panels: one lookup in the registry. This used to be a 38-row table of
    // id → &showXxx, kept in step by hand with the 38 rows that BUILT those
    // commands 80 lines above and with the menu rows that toggle the same
    // flags. `runPanelCommand` returns false for a non-panel id, so the
    // handling below it is unchanged.
    if (runPanelCommand(id)) return;

    if (id == "media.ejectall") { ejectAllDisks(); return; }
}

void MainWindow::renderMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        panelMenuItem(pom2::PanelId::DiskLibrary);
        ImGui::Separator();
        // Disk II (slot 6) — frequent action, lifted out of the old
        // Hardware kitchen-sink. Panel still exposes its own insert/eject
        // buttons; this is the keyboard-friendly path.
        ImGui::BeginDisabled(primaryDiskII() == nullptr);
        if (ImGui::MenuItem("Insert disk image (.dsk / .do / .po / .nib / .woz)...")) {
            diskPanel->insertDialogOpen = true;
            if (diskPanel->dialogPath.empty()) diskPanel->dialogPath = "disks_5.4/";
        }
        if (ImGui::MenuItem("Eject disk", nullptr, false,
                            primaryDiskII() && primaryDiskII()->isDiskLoaded())) {
            // Through the coordinator so the persisted disk_path_slotN key is
            // cleared with the medium. Ejecting from this menu used to leave
            // the path behind, and the next launch re-mounted the disk the
            // user had just ejected.
            const int slot = primaryDiskII()->getSlot();
            const auto r = storageCoordinator_->ejectDiskII(
                *controller, *settings, slot, 0);
            tapeStatusMessage = r.ok ? "Disk ejected"
                                     : "Disk eject failed: " + r.error;
            tapeStatusUntil   = lastFrameTime + 4.0;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(primaryHdvCard() == nullptr);
        if (ImGui::MenuItem("Mount HDV image (.hdv / .2mg)...")) {
            hdvPanel->mountDialogOpen = true;
            if (hdvPanel->dialogPath.empty()) hdvPanel->dialogPath = "hdv/";
        }
        if (ImGui::MenuItem("Eject HDV", nullptr, false,
                            primaryHdvCard() && primaryHdvCard()->isImageLoaded())) {
            const int slot = primaryHdvCard()->getSlot();
            const auto r = storageCoordinator_->ejectMediaBay(
                *controller, *settings, slot, 0);
            tapeStatusMessage = r.ok ? "HDV ejected"
                                     : "HDV eject failed: " + r.error;
            tapeStatusUntil   = lastFrameTime + 4.0;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!primaryHdvCard() || !primaryHdvCard()->isImageLoaded());
        // Label reflects where the user actually has the card plugged.
        const std::string bootHdvLabel = "Boot HDV (slot " +
            std::to_string(primaryHdvCard() ? primaryHdvCard()->getSlot() : 5) + ")";
        if (ImGui::MenuItem(bootHdvLabel.c_str())) {
            bootHdvImage();
        }
        ImGui::EndDisabled();
#ifndef __EMSCRIPTEN__
        // Reload ROM re-reads the ROM file from disk. Useful on native (swap
        // a roms/ file, reload without restarting); pointless under WASM,
        // where the ROM is baked into POM2.data and cannot be replaced — so
        // it is hidden in the browser build.
        ImGui::Separator();
        if (ImGui::MenuItem("Reload ROM")) {
            bool ok = false;
            std::string err;
            // Read the file with NO lock held first. The install below still
            // opens it by path (that is Memory's only entry point), but this
            // pass turns that open into a warm-cache memcpy and catches an
            // unreadable dump — a ROM on a slow or vanished network share —
            // before anything is blocked behind `stateMutex`.
            bool readable = false;
            {
                std::ifstream probe(romPath, std::ios::binary);
                if (probe) {
                    probe.seekg(0, std::ios::end);
                    readable = probe.tellg() > 0;
                }
            }
            if (!readable) {
                err = "cannot read " + romPath;
            } else {
                // Must hold the emulation lock: loadAppleIIRom rewrites
                // $D000-$FFFF and can race with the CPU thread otherwise.
                auto st = controller->lockState();
                ok = st.memory().loadAppleIIRom(romPath.c_str());
                if (!ok) err = st.memory().getLastError();
            }
            // hardReset() re-acquires stateMutex internally, so it MUST run
            // outside the lock_guard scope above — calling it while the lock
            // is held self-deadlocks the non-recursive mutex (mirrors the
            // coldBoot/bootFromSlot call sites elsewhere in this file).
            if (ok) {
                controller->hardReset();
                romStatus = std::string("loaded: ") + romPath;
                romLoaded_ = true;
            } else {
                romStatus = err;
                romLoaded_ = false;
            }
        }
        // Quit is a no-op in the browser: the frame loop is driven by
        // emscripten_set_main_loop_arg (main.cpp), which ignores
        // glfwWindowShouldClose, and a canvas cannot close its own tab.
        // Hide the entry under WASM so the menu stays honest.
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) {
            if (window) glfwSetWindowShouldClose(window, 1);
        }
#endif
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Paste from clipboard", "Ctrl+V"))
            pasteFromClipboard();
#ifndef __EMSCRIPTEN__
        // "Paste from file" reads a host text file; the browser build has no
        // host filesystem (only the read-only bundled MEMFS), and nothing
        // pasteable ships in it. Clipboard paste above still works in WASM.
        if (ImGui::MenuItem("Paste from file..."))
            showPasteFileDialog = true;
#endif
        ImGui::Separator();
        const size_t pending = controller->memory().pendingPasteSize();
        ImGui::BeginDisabled(pending == 0);
        if (ImGui::MenuItem("Cancel pending paste")) {
            controller->memory().cancelPaste();
            tapeStatusMessage = "Paste cancelled";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::MenuItem("Auto-uppercase pasted text", nullptr, &pasteAutoUppercase);
        if (pending > 0) {
            ImGui::Separator();
            ImGui::TextDisabled("(%zu chars pending)", pending);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Machine")) {
        const auto m = controller->getMode();
        if (ImGui::MenuItem("Run", nullptr, m == EmulationController::Mode::Running)) {
            controller->setMode(EmulationController::Mode::Running);
        }
        if (ImGui::MenuItem("Pause", nullptr, m == EmulationController::Mode::Stopped)) {
            controller->setMode(EmulationController::Mode::Stopped);
        }
        if (ImGui::MenuItem("Step (one instr)")) controller->requestStep();
        ImGui::Separator();
        if (ImGui::MenuItem("Reset (Ctrl-Reset)",     "F11")) controller->softReset();
        if (ImGui::MenuItem("Hard reset",             "F12")) controller->hardReset();
        if (ImGui::MenuItem("Cold boot (wipe RAM)"))          controller->coldBoot();
        ImGui::Separator();
        if (ImGui::BeginMenu("Profile")) {
            // 5 canonical Apple II profiles. Selecting one triggers a
            // full cold-reset via `applyProfile()`: new ROM, new char ROM,
            // RAM wiped, slot cards re-plugged, CPU type reset to the
            // profile default (overridable in Machine → CPU). Disk and HDV
            // mounts persist across the switch so the user can test the
            // same software stack under different models.
            for (pom2::SystemProfile p : pom2::allProfiles()) {
                const auto& cfg = pom2::profileConfig(p);
                const bool selected = (activeProfile == p);
                // ImGui's MenuItem 3rd-arg `selected` draws the native
                // checkmark on the right of the row — that's enough; no
                // need to append an extra "✓" to the label (the double
                // mark looked wrong, 2026-05-15). string_view → string
                // for guaranteed null-termination.
                const std::string label(cfg.displayName);
                if (ImGui::MenuItem(label.c_str(), nullptr, selected)) {
                    applyProfile(p);
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("Profile = full cold reset.");
            ImGui::TextDisabled("Mounted disks survive the switch.");
            ImGui::EndMenu();
        }
        // CPU type selector. Three settings:
        //   * Auto (profile default) — NMOS for II/II+, CMOS for IIe/IIc/IIc+
        //   * NMOS 6502 — force NMOS regardless of profile (e.g. test
        //     IIe NMOS-unenhanced behaviour)
        //   * 65C02 — force CMOS (e.g. run NMOS-era software on 65C02)
        // Persisted to settings as `cpu_mode_override` so the choice
        // survives a relaunch. A profile switch re-applies the override.
        M6502::CpuMode curCpu;
        { auto st = controller->lockState(); curCpu = st.cpu().getCpuMode(); }
        const std::string curOverride = settings->getString("cpu_mode_override", "auto");
        if (ImGui::BeginMenu("CPU")) {
            const auto& cfg = pom2::profileConfig(activeProfile);
            // CMOS-only machines (//c, //c+, enhanced //e, PAL variants) have
            // a 65C02 soldered in — an NMOS override is physically impossible
            // AND freezes their 65C02 ROMs (KIL opcodes). resolveCpuMode()
            // clamps it; mirror that here so the menu can't re-arm the freeze.
            const bool cmosOnly = (cfg.defaultCpu == M6502::CpuMode::CMOS);
            const char* profileLabel = cmosOnly ? "65C02" : "NMOS 6502";
            char autoLabel[64];
            std::snprintf(autoLabel, sizeof(autoLabel),
                "Auto (profile default: %s)", profileLabel);
            if (ImGui::MenuItem(autoLabel, nullptr, curOverride == "auto")) {
                settings->setString("cpu_mode_override", "auto");
                settings->save();
                auto st = controller->lockState();
                st.cpu().setCpuMode(cfg.defaultCpu);
            }
            ImGui::BeginDisabled(cmosOnly);
            // On a CMOS-only profile the NMOS override is inert (clamped), so
            // never show it checked there — the running CPU is 65C02.
            if (ImGui::MenuItem("NMOS 6502", nullptr,
                                !cmosOnly &&
                                (curOverride == "nmos" ||
                                 (curOverride == "auto" && curCpu == M6502::CpuMode::NMOS
                                  && curOverride != "65c02")))) {
                settings->setString("cpu_mode_override", "nmos");
                settings->save();
                auto st = controller->lockState();
                st.cpu().setCpuMode(M6502::CpuMode::NMOS);
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("65C02 (CMOS)", nullptr,
                                curOverride == "65c02" ||
                                (curOverride == "auto" && curCpu == M6502::CpuMode::CMOS
                                 && curOverride != "nmos"))) {
                settings->setString("cpu_mode_override", "65c02");
                settings->save();
                auto st = controller->lockState();
                st.cpu().setCpuMode(M6502::CpuMode::CMOS);
            }
            ImGui::Separator();
            ImGui::TextDisabled("NMOS = original 1975. Disables");
            ImGui::TextDisabled("STZ/BRA/PHX/etc. and SMB/RMB/");
            ImGui::TextDisabled("BBR/BBS extensions.");
            ImGui::TextDisabled("Override persists across profile");
            ImGui::TextDisabled("switches (NMOS ignored on 65C02-");
            ImGui::TextDisabled("only models: //c, //c+, enh. //e).");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        panelMenuItem(pom2::PanelId::SlotConfig);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Devices")) {
        // One flat 17-item list became hard to scan (audit 2026-05-31), so it
        // is grouped under SeparatorText headers. What used to follow was 25
        // hand-written rows carrying each panel's label, tooltip, greyed-out
        // condition and slot-number formatting — a third copy of facts the
        // palette and the settings round-trip also held. They live in
        // PanelCatalog.h now; this menu says only which groups it shows and
        // in what order.
        ImGui::SeparatorText("Storage");
        panelMenuGroup(pom2::PanelGroup::DevStorage);
        ImGui::SeparatorText("Sound");
        panelMenuGroup(pom2::PanelGroup::DevSound);
        ImGui::SeparatorText("Ports & cards");
        panelMenuGroup(pom2::PanelGroup::DevPorts);
        ImGui::SeparatorText("Inspectors & tools");
        panelMenuGroup(pom2::PanelGroup::DevInspectors);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Display")) {
        // Presentation aspect (Phase 6). The Apple II pixel is not square;
        // these pick how the 280×192 active area fills the window.
        if (ImGui::BeginMenu("Aspect ratio")) {
            if (ImGui::MenuItem("Square pixels (1:1)", nullptr,
                                aspectMode == AspectMode::Square))
                aspectMode = AspectMode::Square;
            if (ImGui::MenuItem("4:3 (CRT shape)", nullptr,
                                aspectMode == AspectMode::Crt43))
                aspectMode = AspectMode::Crt43;
            if (ImGui::MenuItem("Integer scale (crisp)", nullptr,
                                aspectMode == AspectMode::Integer))
                aspectMode = AspectMode::Integer;
            ImGui::EndMenu();
        }

        // CRT glass sliders (scanlines / mask / barrel / persistence /
        // sharpness / BCS). The shared effect stack runs on every pipeline,
        // so this one panel governs the CRT look across all modes.
        panelMenuItem(pom2::PanelId::Crt);

        // 3D voxel view (MicroM8 "Voxel Cube"): rebuild the screen as an
        // upright 4:3 slab of equal-depth cubes; left-drag orbits, middle-drag
        // pans, wheel zooms. Works on any colour mode.
        panelMenuItem(pom2::PanelId::Voxel);
        panelMenuItem(pom2::PanelId::VoxelSettings);

        // ── Color pipeline ──────────────────────────────────────────────
        // How the Apple II bit stream becomes colour. One pick; the CRT
        // glass below is an independent, composable layer (Phase 3/4 — one
        // shared effect stack downstream of every pipeline).
        ImGui::Separator();
        ImGui::TextDisabled("Color pipeline");
        const Apple2Display::HiResMode cur = display->getHiResMode();
        auto pipeItem = [&](const char* label, const char* tip,
                            Apple2Display::HiResMode m) {
            if (ImGui::MenuItem(label, nullptr, cur == m))
                display->setHiResMode(m);
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        pipeItem("NTSC (MAME)", "7-bit artifact LUT — the canonical composite look.",
                 Apple2Display::HiResMode::ColorNTSC);
        pipeItem("NTSC (MAME) — medium",
                 "composite_color_mode 1: biases 4-dot colour runs (uglier 40-col text).",
                 Apple2Display::HiResMode::ColorCompMedium);
        pipeItem("NTSC (MAME) — 4-bit square",
                 "composite_color_mode 2: each 4-dot nibble → palette index, sharp edges.",
                 Apple2Display::HiResMode::ColorComp4Bit);
        pipeItem("Composite (OpenEmulator, GPU)",
                 "True subcarrier demodulation in a GLSL shader (presets under Effect layers).",
                 Apple2Display::HiResMode::ColorCompositeOE);
        pipeItem("Composite (OpenEmulator, CPU)",
                 "Same OpenEmulator demodulation computed on the CPU into the\n"
                 "framebuffer — no GLSL shader. Works without a GL shader path\n"
                 "and lets you A/B the two. CRT effect layers still apply.",
                 Apple2Display::HiResMode::ColorCompositeOECpu);

        // AppleWin NTSC — only the TV (50% line-blur) sub-mode is exposed
        // (the Monitor / Idealized variants were dropped). Flat entry that
        // forces the Tv sub-mode and selects the pipeline.
        if (ImGui::MenuItem("AppleWin NTSC (TV 50% line blur)", nullptr,
                            cur == Apple2Display::HiResMode::ColorAppleWin)) {
            display->setAppleWinSubMode(Apple2Display::AppleWinSubMode::Tv);
            display->setHiResMode(Apple2Display::HiResMode::ColorAppleWin);
        }

        // RGB card — clean Péritel decode, two distinct grays. Greyed out
        // when no Le Chat Mauve card is plugged in slot 7.
        ImGui::BeginDisabled(
            !devicePanelCoordinator_->captureInventory().chatMauvePlugged());
        pipeItem("RGB card — Le Chat Mauve", nullptr,
                 Apple2Display::HiResMode::ChatMauveRGB);
        ImGui::EndDisabled();

        pipeItem("Monochrome — White",      nullptr, Apple2Display::HiResMode::MonoWhite);
        pipeItem("Monochrome — Green (P31)", nullptr, Apple2Display::HiResMode::MonoGreen);
        pipeItem("Monochrome — Amber",      nullptr, Apple2Display::HiResMode::MonoAmber);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        // ── Full screen = kiosk ─────────────────────────────────────────
        // There is no separate "full screen": going full screen IS kiosk
        // mode (exclusive full-screen, chrome-free, settings read-only).
        // The machine keeps running across the switch — no state is lost.
        if (ImGui::MenuItem(ICON_FA_EXPAND " Full screen (kiosk)",
                            "Ctrl+Alt+F", kiosk_)) {
            toggleKioskMode();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Exclusive full screen with no UI chrome — the kiosk view.\n"
                "Ctrl+Alt+F (or F10) toggles back; the emulated machine\n"
                "keeps running across the switch, so nothing is lost.\n"
                "Settings are not written while in kiosk.");

        // ── Mouse capture ───────────────────────────────────────────────
        // Greyed with no Mouse Card on the bus: capturing the pointer with
        // nothing to feed it is the one state this feature must not reach.
        {
            const bool haveCard = mouseCoordinator_->capture().plugged();
            if (ImGui::MenuItem(ICON_FA_ARROW_POINTER " Capture mouse",
                                "Ctrl+Alt+G", mouseGrabbed_, haveCard)) {
                toggleMouseGrab();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    haveCard
                    ? "Give the host pointer to the Apple II Mouse Card.\n"
                      "Captured, motion is unbounded (the guest cursor can\n"
                      "always reach its own clamp edges) and the OS cursor\n"
                      "is hidden. Ctrl+Alt+G or a middle click releases it."
                    : "No Mouse Card plugged — add 'mouse' (MAME, needs both\n"
                      "ROMs) or 'mouseaw' (AppleWin HLE) in Slot Configuration.");
        }
        ImGui::Separator();

        // ── Docking layout ──────────────────────────────────────────────
        // Task-oriented presets. No checkmarks on purpose: the entries are
        // actions, and the moment the user drags a tab the "active" preset
        // stops describing what's on screen.
        if (ImGui::BeginMenu(ICON_FA_TABLE_COLUMNS " Layout")) {
            auto layoutItem = [&](const char* label, DockLayout p,
                                  const char* tip) {
                if (ImGui::MenuItem(label)) {
                    pendingDockLayout_   = p;
                    dockLayoutRequested_ = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            };
            layoutItem("Reset to default", DockLayout::Reset,
                       "Screen centre; Disk Library, Slot Configuration and\n"
                       "ImageWriter II tabbed right; inspectors as a tab\n"
                       "group bottom-right.");
            ImGui::Separator();
            layoutItem("Emulation", DockLayout::Emulation,
                       "Widest screen. Disk Library / Cassette / Floppy Emu\n"
                       "and Slot Config in one right column. No debug tools.");
            layoutItem("Debug", DockLayout::Debug,
                       "Memory viewer + maps right, horizontal map along the\n"
                       "bottom, inspectors bottom-right.");
            layoutItem("Audio", DockLayout::Audio,
                       "Mockingboard / Phasor / Echo+ right, mixer and tape\n"
                       "bottom-right.");
            ImGui::Separator();
            ImGui::TextDisabled("Drag any tab to re-dock; layout is saved.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Docks persist in ~/.config/POM2/imgui.ini.\n"
                    "Slot-numbered panels (Disk II, 3.5\", HDV, SmartPort,\n"
                    "Printer) build their title at runtime, so presets can't\n"
                    "place them — dock them once and they stay put.");
            ImGui::EndMenu();
        }
        ImGui::Separator();

        // ── Interface appearance ────────────────────────────────────────
        // Accent + zoom. Both re-theme immediately (applyUiTheme rebuilds
        // the style from scratch, so repeated calls don't compound the
        // scale) and both persist to state.cfg on exit.
        if (ImGui::BeginMenu(ICON_FA_PALETTE " Interface")) {
            ImGui::SeparatorText("Accent");
            std::size_t nAccents = 0;
            const pom2::UiAccent* accents = pom2::allAccents(nAccents);
            for (std::size_t i = 0; i < nAccents; ++i) {
                const pom2::UiAccent a = accents[i];
                if (ImGui::MenuItem(pom2::accentLabel(a), nullptr,
                                    uiAccent_ == a)) {
                    uiAccent_ = a;
                    applyUiTheme();
                }
            }

            ImGui::SeparatorText("Zoom");
            // Percent rather than a raw multiplier — "125 %" is the unit
            // every other desktop app uses for this control.
            int pct = static_cast<int>(uiScale_ * 100.0f + 0.5f);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderInt("##uiscale", &pct,
                                 static_cast<int>(pom2::kUiScaleMin * 100.0f),
                                 static_cast<int>(pom2::kUiScaleMax * 100.0f),
                                 "%d %%")) {
                uiScale_ = std::clamp(static_cast<float>(pct) / 100.0f,
                                      pom2::kUiScaleMin, pom2::kUiScaleMax);
                applyUiTheme();
            }
            auto zoomStep = [&](const char* label, float delta) {
                if (ImGui::MenuItem(label)) {
                    uiScale_ = std::clamp(uiScale_ + delta,
                                          pom2::kUiScaleMin, pom2::kUiScaleMax);
                    applyUiTheme();
                }
            };
            zoomStep("Zoom in",  +pom2::kUiScaleStep * 2.0f);
            zoomStep("Zoom out", -pom2::kUiScaleStep * 2.0f);
            if (ImGui::MenuItem("Reset to 100 %")) {
                uiScale_ = 1.0f;
                applyUiTheme();
            }
            if (dpiScale_ != 1.0f) {
                ImGui::Separator();
                ImGui::TextDisabled("Display scale: %.0f %% (from the OS)",
                                    dpiScale_ * 100.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Monitor content scale reported by the windowing\n"
                        "system. The zoom above multiplies on top of it —\n"
                        "effective UI scale is %.0f %%.",
                        uiScale_ * dpiScale_ * 100.0f);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();

        panelMenuItem(pom2::PanelId::MemViewer);
        panelMenuItem(pom2::PanelId::Debugger);
        ImGui::Separator();
        panelMenuItem(pom2::PanelId::MemBar);
        panelMenuItem(pom2::PanelId::MemBarH);
        panelMenuItem(pom2::PanelId::MemGrid);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Command palette...", "Ctrl+Shift+P"))
            openCommandPalette();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fuzzy-search every menu item, panel and machine\n"
                              "action. Type \"mock\", \"amber\", \"eject\"...");
        ImGui::Separator();
        // The AI Control row hides itself under WASM — AiControlServer cannot
        // open a listening socket in the browser sandbox. That used to be an
        // #ifndef here; it is now one line in registerPanels(), where the rest
        // of the panel's identity already lives.
        panelMenuGroup(pom2::PanelGroup::Tools);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        // Three rows, and one of them used to be wrong: ROM Status's tooltip
        // was attached to the Abstraction Levels item (two IsItemHovered
        // blocks after the same MenuItem), so one row showed the other's tip
        // and ROM Status showed none. Both now come from the catalog.
        panelMenuGroup(pom2::PanelGroup::Help);
        ImGui::Separator();
        if (ImGui::MenuItem("About POM2")) showAbout = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

// Bottom-of-viewport status bar. Carries the machine/mode/graphics summary,
// plus the three things that used to require opening a panel to answer:
// is a drive spinning and on what image, is the machine actually keeping up
// with the requested clock, and is host caps-lock on.
//
// Everything past the machine/mode/graphics group is optional and dropped
// when the window is too narrow (widths are measured in em so the pruning
// behaves the same at any UI scale).
void MainWindow::renderStatusBar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight();
    // NoDocking: the status bar is chrome. Without it a dragged panel can be
    // dropped into the one-line strip at the bottom of the screen.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##StatusBar", vp, ImGuiDir_Down,
                                    height, flags)) {
        if (ImGui::BeginMenuBar()) {
            const pom2::Palette& pal = pom2::palette();
            const auto  u32   = ImGui::ColorConvertU32ToFloat4;
            // Width unit: everything below budgets in ems so the drop-when-
            // narrow logic survives the UI zoom.
            const float em    = ImGui::GetFontSize();
            auto roomFor = [&](float ems) {
                return ImGui::GetContentRegionAvail().x > ems * em;
            };

            const auto mode = controller->getMode();
            const char* modeStr = "?";
            ImU32       modeCol = pal.textDim;
            switch (mode) {
                case EmulationController::Mode::Running:
                    modeStr = "RUN";  modeCol = pal.ok;   break;
                case EmulationController::Mode::Stopped:
                    modeStr = "STOP"; modeCol = pal.warn; break;
                case EmulationController::Mode::Step:
                    modeStr = "STEP"; modeCol = pal.info; break;
            }
            // Under the lock: this is live soft-switch state the worker
            // rewrites as the guest flips $C050-$C057, and it is copied out
            // as a struct, so an unlocked read can straddle a change.
            Memory::DisplayState state;
            { auto st = controller->lockState(); state = st.memory().getDisplayState(); }
            const char* gfx = state.textMode ? "TEXT"
                            : state.hiRes    ? (state.mixedMode ? "HGR+TXT" : "HGR")
                                             : (state.mixedMode ? "LGR+TXT" : "LGR");
            const auto& cfg = pom2::profileConfig(activeProfile);

            // ── Machine · mode · graphics (always shown) ─────────────────
            ImGui::TextColored(u32(pal.textDim), "%.*s",
                               static_cast<int>(cfg.displayName.size()),
                               cfg.displayName.data());
            pom2::verticalRule();
            ImGui::TextColored(u32(modeCol), "%s", modeStr);
            pom2::verticalRule();
            ImGui::TextColored(u32(pal.textDim), "%s", gfx);

            // ── Mounted media, every bay, each with its own access LED ───
            // Walked off the SlotBus rather than the named card aliases so a
            // second Disk II, a second CFFA or a SmartPort's two units all
            // appear — the aliases only ever remember one card per kind.
            //
            // The bar is one line and the machine can carry a lot of media,
            // so each entry is added only while `roomFor` still says yes and
            // the rest are dropped silently. Entries go in bus order (slot,
            // then drive/bay) so a given machine's layout stays put instead
            // of reshuffling as drives spin up.
            //
            // LED semantics differ per bay and that is deliberate: a Disk II
            // lights on real spindle motion, while a block device has no
            // mechanics and instead bleeds off an activity counter
            // (`Block512Backing::isBusy`). SmartPort units expose no activity
            // signal at all, so theirs stays dark — better an honest dark LED
            // than one that never means anything.
            //
            // Built as a VALUE snapshot under `stateMutex`, then rendered
            // with the lock released. Both halves matter: `getDiskPath()`
            // hands back a reference into live `DiskImage` state that the AI
            // server's HTTP thread rewrites on /disk and /eject, and holding
            // `stateMutex` across ImGui calls is what deadlocked the memory
            // viewer (a non-recursive mutex, and ImGui callbacks can re-enter
            // host code). Snapshot, unlock, draw.
            struct MediaRow {
                bool        active = false;
                const char* icon   = nullptr;
                std::string label;
                std::string tip;
                // Identity, so a click on the chip can act on the exact bay
                // it names. `index` is the Disk II drive or the media bay.
                int         slot   = 0;
                int         index  = 0;
                bool        diskII = false;
                bool        dirty  = false;   // unsaved changes pending
            };
            std::vector<MediaRow> mediaRows;
            {
                auto baseName = [](const std::string& p) {
                    return std::filesystem::path(p).filename().string();
                };
                auto st = controller->lockState();
                for (int slot = 1; slot <= 7; ++slot) {
                    SlotPeripheral* per =
                        st.memory().slotBus().peripheral(slot);
                    if (!per) continue;

                    if (auto* d2 = dynamic_cast<DiskIICard*>(per)) {
                        for (int drv = 0; drv < 2; ++drv) {
                            if (!d2->isDiskLoaded(drv)) continue;
                            // Only the SELECTED drive's motor turns: a Disk II
                            // controller drives one spindle at a time.
                            const bool spinning =
                                d2->isMotorOn() && d2->getActiveDrive() == drv;
                            const std::string path = d2->getDiskPath(drv);
                            mediaRows.push_back(
                                { spinning, ICON_FA_FLOPPY_DISK,
                                  baseName(path),
                                  "Slot " + std::to_string(d2->getSlot()) +
                                      ", drive " + std::to_string(drv + 1) +
                                      " — track " +
                                      std::to_string(d2->getCurrentTrack(drv)) +
                                      "\n" + path,
                                  d2->getSlot(), drv, /*diskII=*/true,
                                  d2->hasUnsavedChanges(drv) });
                        }
                        continue;
                    }

                    // Everything else that can hold an image advertises bays,
                    // and each bay reports its own activity — a SmartPort's
                    // two units light independently, which a card-wide flag
                    // could not express.
                    auto* media = dynamic_cast<pom2::MountableMediaCard*>(per);
                    if (!media) continue;

                    for (int bay = 0; bay < media->bayCount(); ++bay) {
                        const pom2::MediaBayInfo info = media->bayInfo(bay);
                        if (!info.loaded || info.path.empty()) continue;
                        std::string tip = "Slot " + std::to_string(slot);
                        if (media->bayCount() > 1)
                            tip += ", bay " + std::to_string(bay + 1);
                        if (!info.kindLabel.empty())
                            tip += " — " + info.kindLabel;
                        tip += "\n" + info.path;
                        mediaRows.push_back({ info.busy, ICON_FA_HARD_DRIVE,
                                              baseName(info.path),
                                              std::move(tip),
                                              slot, bay, /*diskII=*/false,
                                              info.hasUnsavedChanges });
                    }
                }
            }
            {
                const float lineH = ImGui::GetFrameHeight();
                int rowIdx = 0;
                for (const MediaRow& row : mediaRows) {
                    // 6 ems of chrome (rule + dot + icon + padding) plus the
                    // label itself, measured rather than guessed so a long
                    // filename cannot push the row off the end of the bar.
                    const float need =
                        6.0f * em + ImGui::CalcTextSize(row.label.c_str()).x;
                    if (ImGui::GetContentRegionAvail().x <= need) break;
                    ImGui::PushID(rowIdx++);
                    pom2::verticalRule();
                    pom2::indicatorDot(row.active, pal.warn, 4.0f, lineH);
                    // Each chip is a control, not a label: clicking it opens
                    // an eject menu for THAT bay. Brightening on hover is what
                    // says so — a status bar is read as read-only furniture
                    // until something under the pointer reacts.
                    //
                    // Reserved as a REAL item (an InvisibleButton the exact
                    // size of the text) and painted through the draw list,
                    // rather than drawn as text with a hand-rolled
                    // IsMouseHoveringRect. That call is not z-order aware, so
                    // the chip lit up through anything drawn over it — its own
                    // eject popup included — while the click, which goes
                    // through IsItemClicked, correctly did not. One item now
                    // answers hover, tooltip and click alike.
                    const std::string chip =
                        std::string(row.icon) + " " + row.label;
                    const ImVec2 chipSz  = ImGui::CalcTextSize(chip.c_str());
                    const ImVec2 chipPos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton(
                        "##chip", ImVec2(ImMax(chipSz.x, 1.0f), lineH));
                    const bool hot = ImGui::IsItemHovered();
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(chipPos.x,
                               chipPos.y + (lineH - chipSz.y) * 0.5f),
                        hot ? pal.accent
                            : (row.active ? pal.text : pal.textDim),
                        chip.c_str());
                    if (hot)
                        ImGui::SetTooltip("%s\n\nClick to eject.",
                                          row.tip.c_str());
                    // A menu rather than eject-on-click: the bar is a dense
                    // strip of small targets right under the screen, and an
                    // accidental click would pull a disk out from under a
                    // running program. One extra click also buys room to name
                    // the bay and to warn about unsaved changes.
                    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##ejectmenu");
                    if (ImGui::BeginPopup("##ejectmenu")) {
                        ImGui::TextDisabled("%s", row.tip.c_str());
                        ImGui::Separator();
                        if (row.dirty)
                            ImGui::TextColored(
                                u32(pal.warn),
                                "Unsaved changes — ejecting writes them back\n"
                                "if write-back is on for this drive, and drops\n"
                                "them if it is not.");
                        if (ImGui::MenuItem(ICON_FA_EJECT " Eject"))
                            ejectMediaBay(row.slot, row.index, row.diskII);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }

            // ── Mouse capture ────────────────────────────────────────────
            // The ONLY capture indicator now that the on-screen caption is
            // gone — this is the badge a user looks for when the pointer
            // "disappeared". Only ever shown while captured.
            if (mouseGrabbed_ && roomFor(9.0f)) {
                pom2::verticalRule();
                ImGui::TextColored(u32(pal.accent),
                                   ICON_FA_ARROW_POINTER " GRAB");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "The host pointer is captured and feeding the Mouse\n"
                        "Card: motion no longer stops at the edge of the\n"
                        "screen widget, and the OS cursor is hidden.\n"
                        "Ctrl+Alt+G or a middle click gives it back.");
                // Spell the way out in full, in the bar, for a good while
                // after the capture — long enough to read without hunting
                // for a tooltip, and it costs nothing but bar width. It is
                // written out rather than left to the tooltip above because
                // a user who cannot find their pointer is not in a mood to
                // go hovering things to find out why.
                if (lastFrameTime < mouseGrabHintUntil_ && roomFor(30.0f)) {
                    ImGui::SameLine();
                    ImGui::TextColored(u32(pal.textDim),
                                       ICON_FA_ARROW_RIGHT
                                       " Ctrl+Alt+G or middle click to release");
                }
            } else if (!mouseGrabbed_ && screenHovered_ &&
                       mouseCoordinator_->capture().plugged() && roomFor(30.0f)) {
                // Not captured, but the pointer is over the emulated screen
                // with a Mouse Card on the bus — the exact moment the user is
                // about to wonder why the guest cursor won't follow theirs.
                // Say how to hand it over, here rather than on the screen:
                // the on-screen captions were removed for being noise over a
                // running game, and this is the same information in the one
                // place that is already a status surface.
                //
                // `screenHovered_` is ImGui's z-order-aware verdict from
                // renderScreenWindow(), which runs earlier this frame — so a
                // menu or a docked panel drawn over the screen correctly
                // suppresses the hint. Gated on a card being plugged because
                // `shouldToggleGrab` would refuse to capture without one, and
                // advertising a shortcut that then does nothing is worse than
                // silence.
                pom2::verticalRule();
                ImGui::TextColored(u32(pal.textDim),
                                   ICON_FA_ARROW_POINTER
                                   " Ctrl+Alt+G or middle click to capture");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "The Mouse Card is a relative device: uncaptured, "
                        "your pointer\nstops at the edge of the screen "
                        "widget while the guest cursor\nstill has clamp "
                        "window left, and the two drift apart.\n"
                        "Capturing hides the OS cursor and feeds every delta "
                        "to the guest.");
            }

            // ── Host caps-lock ───────────────────────────────────────────
            // Only ever shown when ON: a permanent "CAPS off" badge would be
            // noise. Explains the classic "the game ignores my keys" report.
            if (hostCapsLock_ && roomFor(8.0f)) {
                pom2::verticalRule();
                ImGui::TextColored(u32(pal.warn), ICON_FA_KEYBOARD " CAPS");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Host caps-lock is on — every letter reaches the\n"
                        "Apple II as uppercase. Harmless on II/II+ (which\n"
                        "are uppercase-only) but breaks lowercase input on\n"
                        "//e and //c software.");
            }

            // A print in progress has to be visible without the paper tray
            // being open: the printer runs at 250 cps, so a page takes
            // minutes of host time, and with the real handshake enabled
            // the guest is deliberately frozen for that whole stretch.
            // Unexplained, that reads as a hung emulator.
            if (imageWriter && imageWriter->busy()) {
                const bool waiting =
                    printerBackPressure &&
                    printerCoordinator_->captureHost(*controller).grapplerBusy;
                pom2::verticalRule();
                ImGui::TextColored(u32(pal.warn),
                                   ICON_FA_PRINT " printing %zu B%s",
                                   imageWriter->pendingBytes(),
                                   waiting ? " (Apple II waiting)" : "");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "The ImageWriter is still laying this job down at "
                        "its real speed.\nDevices → ImageWriter II to watch "
                        "the sheet, or \"Print now\" to skip the wait.");
            }

            // Transient disk load / boot (and other) status messages, shown
            // right-aligned and auto-expiring (tapeStatusUntil). This is the
            // text that used to float in a separate overlay near the bottom.
            if (!tapeStatusMessage.empty() && lastFrameTime < tapeStatusUntil) {
                const float msgW = ImGui::CalcTextSize(
                    tapeStatusMessage.c_str()).x;
                const float avail = ImGui::GetContentRegionAvail().x;
                if (avail > msgW) {
                    ImGui::SameLine(0.0f, avail - msgW);
                } else {
                    ImGui::SameLine();
                }
                ImGui::TextColored(
                    ImGui::ColorConvertU32ToFloat4(pom2::palette().accent),
                    "%s", tapeStatusMessage.c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}
