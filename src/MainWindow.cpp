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

#include "MainWindow.h"

// Named because this file uses them DIRECTLY. All of them also arrive
// transitively through MainWindow.h today, which is precisely why they are
// listed: libstdc++ is stricter about transitive includes than libc++, so a
// header cleanup elsewhere would break the GCC build and nothing else.
#include "Apple2Display.h"
#include "CharRomCatalog.h"
#include "FujiNetCard.h"
#include "ImageWriter.h"
#include "LeChatMauve_ImGui.h"
#include "NtscPostProcessor.h"
#include "SystemProfile.h"
#include "imgui.h"


// Heavy headers — pulled here so MainWindow.h can stay forward-declared.
// Touch any of these only recompiles the MainWindow_*.cpp TUs, not every
// file that includes MainWindow.h.
#include "AiControlServer.h"
#include "CassetteDeck_ImGui.h"
#include "Rewind_ImGui.h"
#include "FujiNetCardFactory.h"
#include "ImageWriter_ImGui.h"
#include "RomStatus_ImGui.h"
#include "AbstractionLevels_ImGui.h"
#include "Keyboard_ImGui.h"
#include "Disk35Controller_ImGui.h"
#include "DiskController_ImGui.h"
#include "DiskLibrary_ImGui.h"
#include "EmulationController.h"
#include "HdvController_ImGui.h"
#include "JoystickInput.h"
#include "JoystickPanel_ImGui.h"
#include "Logger.h"
#include "Debugger_ImGui.h"
#include "HgrPaintEditor.h"     // portable hgrpaint/ editor (shared with POM1)
#include "HgrSpriteEditor.h"    // portable hgrsprite/ editor (same host seam)
#include "Pom2HgrPaintHost.h"
#include "MouseCoordinator.h"
#include "NetworkCoordinator.h"
#include "AudioCoordinator.h"
#include "DevicePanelCoordinator.h"
#include "SlotCardFactory.h"
#include "DebugCoordinator.h"
#include "SlotConfigurationCoordinator.h"
#include "SlotProvisioningCoordinator.h"
#include "SlotRebuildCoordinator.h"
#include "StorageCoordinator.h"
#include "PrinterCoordinator.h"
#include "CrtEffectStack.h"
#include "Voxel3DRenderer.h"
#include "CffaCard.h"
#include "ProDOSHardDiskCard.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SettingsList.h"
#include "IconsFontAwesome6.h"
#include "FloppyEmuDevice.h"
#include "FloppyEmu_ImGui.h"
#include "PrinterHistory.h"
#include "PrinterSoundDevice.h"
#include "SuperSerialCard.h"
#include "Toolbar_ImGui.h"
#include "CommandPalette_ImGui.h"

#include "imgui_internal.h"   // BeginViewportSideBar (status bar)
#include <GLFW/glfw3.h>

#include <algorithm>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>


namespace {

// The list-packing helpers moved to SettingsList.h when the persist half of
// this file became MainWindow_Session.cpp: both sides must agree on the
// separator, and agreement is not something two copies can promise.

} // anon namespace

MainWindow::MainWindow(bool forceIIPlus)
    // Member init order matches declaration order in MainWindow.h: the
    // controller is constructed first so memViewer can safely call
    // controller->memory() in its initialiser. Settings + AiControlServer
    // are heap-allocated for the same reason as the rest — keep their
    // headers out of MainWindow.h.
    : controller     (std::make_unique<EmulationController>()),
      display        (std::make_unique<Apple2Display>()),
      debugCoordinator_(std::make_unique<pom2::DebugCoordinator>(*controller)),
      debuggerPanel  (std::make_unique<pom2::Debugger_ImGui>()),
      settings       (std::make_unique<pom2::Settings>()),
      cassetteDeck   (std::make_unique<pom2::CassetteDeck_ImGui>()),
      rewindPanel_   (std::make_unique<pom2::Rewind_ImGui>()),
      // Declared above the coordinators in MainWindow.h, so initialised above
      // them here. Members are constructed in DECLARATION order whatever this
      // list says, and a list that disagrees is how an initialiser comes to
      // read a member that has not been built yet.
      disk35Panel    (std::make_unique<pom2::Disk35Controller_ImGui>()),
      diskLibrary    (std::make_unique<pom2::DiskLibrary_ImGui>()),
      cmdPalette     (std::make_unique<pom2::CommandPalette_ImGui>()),
      hdvPanel       (std::make_unique<pom2::HdvController_ImGui>()),
      smartPortPanel (std::make_unique<pom2::SmartPort_ImGui>()),
      fujiNetPanel   (std::make_unique<pom2::FujiNet_ImGui>()),
      floppyEmu      (std::make_unique<pom2::FloppyEmuDevice>()),
      floppyEmuPanel (std::make_unique<pom2::FloppyEmu_ImGui>()),
      joystickPanel  (std::make_unique<pom2::JoystickPanel_ImGui>()),
      printerSound   (std::make_unique<pom2::PrinterSoundDevice>()),
      printerHistory (std::make_unique<pom2::PrinterHistory>()),
      imageWriter    (std::make_unique<pom2::ImageWriter>()),
      imageWriterPanel(std::make_unique<pom2::ImageWriter_ImGui>()),
      chatMauvePanel (std::make_unique<pom2::LeChatMauve_ImGui>()),
      toolbar        (std::make_unique<pom2::Toolbar_ImGui>()),
      hgrPaintHost   (std::make_unique<Pom2HgrPaintHost>(controller.get())),
      hgrPaintEditor (std::make_unique<hgrpaint::HgrPaintEditor>(hgrPaintHost.get())),
      hgrSpriteEditor(std::make_unique<hgrsprite::HgrSpriteEditor>(hgrPaintHost.get())),
      mouseCoordinator_(std::make_unique<pom2::MouseCoordinator>(*controller)),
      networkCoordinator_(std::make_unique<pom2::NetworkCoordinator>()),
      printerCoordinator_(std::make_unique<pom2::PrinterCoordinator>()),
      audioCoordinator_(std::make_unique<pom2::AudioCoordinator>(
          controller->audio(), *controller)),
      devicePanelCoordinator_(std::make_unique<pom2::DevicePanelCoordinator>(
          *controller, *settings)),
      storageCoordinator_(std::make_unique<pom2::StorageCoordinator>()),
      slotCardFactory_(std::make_unique<pom2::SlotCardFactory>()),
      // The rebuild transaction. Every hook is required — the coordinator
      // throws rather than let a half-wired teardown run — and the ORDER is
      // its contract, not this list's: gate AI requests, drop the non-owning
      // views (audio sources, panels, the printer feed identity), clear the
      // bus, then the host-side services that no longer have a card.
      slotRebuildCoordinator_(std::make_unique<pom2::SlotRebuildCoordinator>(
          pom2::SlotRebuildCoordinator::Hooks{
              // Only reached after a successful flush, which is the point:
              // history that indexes a topology must not outlive it, and
              // session-only provisioning must not be persisted from it.
              [this] {
                  controller->rewind().clear();
                  storageCoordinator_->clearAutoProvisioned();
              },
              [this] { aiServer->detach(); },
              // Drive teardown off the registration inventory, never the card
              // aliases: two coexisting Mockingboard variants left the first
              // card's AudioSource registered against freed memory.
              [this] { unregisterAllAudioSources(); },
              [this] {
                  diskPanels.clear();
                  diskPanel = nullptr;
                  // These are read by panels every frame. The FujiNet card
                  // also owns a listening socket and a worker thread that
                  // SlotBus::clear() joins, so the alias has to go before the
                  // clear, not after it.
              },
              [this] { printerCoordinator_->resetFeedCursor(); },
              // Nothing host-side outlives the cards today; the hook exists
              // so a helper process gets torn down here rather than somewhere
              // that runs before SlotBus::clear().
              [] {},
              [this] { display->setChatMauveCard(nullptr); },
              [this] {
                  aiServer->attach(controller.get(), display.get(),
                                   primaryDiskII(), primaryHdvCard());
              },
          })),
      slotConfigCoordinator_(
          std::make_unique<pom2::SlotConfigurationCoordinator>()),
      slotProvisioningCoordinator_(
          std::make_unique<pom2::SlotProvisioningCoordinator>(
              *slotCardFactory_, *storageCoordinator_)),
      joystick       (std::make_unique<JoystickInput>()),
      sscPortInput   (SuperSerialCard::kDefaultPort),
      aiServer       (std::make_unique<pom2::AiControlServer>()),
      aiPortInput    (pom2::AiControlServer::kDefaultPort),
      charRomLocale  (pom2::CharRomLocale::ProfileDefault),
      activeProfile  (pom2::SystemProfile::AppleIIPlus)
{
    // Memory viewer writes go through Memory::memWrite under stateMutex,
    // so a byte poked from the UI passes through ROM-write protection and
    // any future I/O hooks just like a CPU store would.
    // Bind every panel in the catalog to its flag BEFORE anything reads or
    // writes panel state: the menus, the palette, the settings round-trip and
    // the browser build's chrome-light startup are all derived from those
    // bindings, and an unbound panel is a menu row that toggles nothing.
    registerPanels();

    // Load any persisted runtime config. Missing/malformed file → use
    // defaults; the fields below honour the saved values when present.
    settings->load();

    // Probe a few common locations so the binary works whether launched
    // from build/ or the repo root. Apple IIe (16 KB ROM at $C000-$FFFF
    // with internal I/O ROM in $C100-$CFFF) takes precedence: if
    // roms/apple2e.rom is present we run as a IIe (128 KB, 80-col, IIe
    // soft switches). Otherwise the legacy II+ path runs as before.
    namespace fs = std::filesystem;
    static const char* iieRomCandidates[]   = { "roms/apple2e.rom",
                                                "../roms/apple2e.rom",
                                                "../../roms/apple2e.rom" };
    static const char* romCandidates[]      = { "roms/apple2.rom",
                                                "../roms/apple2.rom",
                                                "../../roms/apple2.rom" };
    // Char ROM probing. Prefer the 4 KB IIe Enhanced variant (mousetext
    // + lowercase) when running in IIe mode; fall back to the 2 KB II/II+
    // ROM otherwise. Both formats are normalised to AppleWin-style
    // csbits in `Memory::loadCharRom`, so the renderer is uniform.
    static const char* charRomIIeCandidates[] = { "roms/apple2e_char.rom",
                                                  "../roms/apple2e_char.rom",
                                                  "../../roms/apple2e_char.rom" };
    static const char* charRomCandidates[]   = { "roms/apple2_char.rom",
                                                "../roms/apple2_char.rom",
                                                "../../roms/apple2_char.rom" };

    // findResource resolves each candidate against the CWD, the build/-
    // relative roots (dev), and the executable-relative / FHS roots
    // (portable bundle, AppImage, /usr/bin). See ResourcePaths.h.
    bool iiePresent = false;
    if (!forceIIPlus) {
        for (const char* p : iieRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { romPath = r; iiePresent = true; break; }
        }
    }
    if (!iiePresent) {
        for (const char* p : romCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { romPath = r; break; }
        }
    }
    charRomPath.clear();
    // Restore user-selected character ROM locale (toolbar dropdown).
    // ProfileDefault keeps the legacy auto-probe; anything else maps
    // to a specific file in roms/. The override is applied here BEFORE
    // the probe so the very first frame already shows the chosen font
    // — otherwise applyProfile() catches up a few hundred ms later
    // and the user briefly sees the wrong glyphs.
    charRomLocale = pom2::charRomLocaleFromKey(
        settings->getString("char_rom_locale", "default"));
    if (charRomLocale != pom2::CharRomLocale::ProfileDefault) {
        // resolveCharRomPath probes roms/X, ../roms/X, ../../roms/X —
        // same prefix sweep as the legacy IIe probe, so the override
        // works whether POM2 is launched from the repo root or from
        // build/.
        const std::string overridePath =
            pom2::resolveCharRomPath(charRomLocale);
        if (!overridePath.empty()) {
            charRomPath = overridePath;
        } else {
            // File missing — fall through to the legacy probe so we
            // don't end up with a blank screen, and reset the saved
            // locale so the dropdown reflects what actually loaded.
            charRomLocale = pom2::CharRomLocale::ProfileDefault;
        }
    }
    if (charRomPath.empty() && iiePresent) {
        for (const char* p : charRomIIeCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { charRomPath = r; break; }
        }
    }
    if (charRomPath.empty()) {
        for (const char* p : charRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { charRomPath = r; break; }
        }
    }

    // Constructor: the CPU worker is not running yet (controller->start()
    // comes later), so the raw accessor is correct here — there is no
    // second thread to be raced by. Everything past start() uses
    // lockState().
    if (iiePresent) {
        controller->memory().setIIEMode(true);
        const int banks = settings->getInt("ramworks_banks", 1);
        controller->memory().setRamWorksBanks(
            static_cast<uint32_t>(banks > 0 ? banks : 1));
        display->setAuxMemory(controller->memory().auxData());
    }

    if (controller->memory().loadAppleIIRom(romPath.c_str())) {
        romStatus = std::string(iiePresent ? "IIe (128K): " : "loaded: ") + romPath;
        romLoaded_ = true;
    } else {
        romStatus = std::string("NO ROM (") + romPath +
                    ") — only $D000-$FFFF stub is active";
        romLoaded_ = false;
        // First-launch newcomer with no firmware: greet them with the
        // Welcome panel (folders + expected filenames + Reload button)
        // instead of leaving them staring at a bare "NO ROM" screen.
        show(pom2::PanelId::Welcome) = true;
    }
    controller->memory().loadCharRom(charRomPath.c_str(),
                                     pom2::charRomBank(charRomLocale));

    // Load the MAME floppy sound samples (head step, motor spin, insert
    // click) for both 5.25" and 3.5" form factors. Each FloppySoundDevice
    // instance stores a single sample bank; we have two — one per form
    // factor — wired to DiskIICard / Sony35Drive / SmartPortCard. Probe
    // paths mirror the ROM probe order; the first directory containing
    // either set wins, the other set degrades to silent if absent.
    static const char* floppySampleDirs[] = {
        "roms/floppy_samples",
        "../roms/floppy_samples",
        "../../roms/floppy_samples",
    };
    for (const char* d : floppySampleDirs) {
        const std::string dir = pom2::findResource(d);
        if (dir.empty() || !fs::is_directory(dir)) continue;
        const bool ok525 = controller->floppySound525().loadSamples(
            dir, FloppySoundDevice::FormFactor::FF525);
        const bool ok35  = controller->floppySound35().loadSamples(
            dir, FloppySoundDevice::FormFactor::FF35);
        if (ok525 || ok35) break;
    }
    {
        // Restore persisted volume/mute per channel. The 3.5" channel
        // inherits the 5.25" defaults on first run so users who had
        // already tuned floppy_sound_volume don't get a louder/quieter
        // 3.5" surprise.
        const float vol525 = settings->getFloat("floppy_sound_volume", 0.6f);
        const bool  mute525 = settings->getBool ("floppy_sound_muted",  false);
        const float vol35   = settings->getFloat("floppy_sound_volume_35", vol525);
        // WASM default boots from an HDV ("hard disk"), and the disk-drive
        // mechanical sounds carry loud over the browser's Web Audio path.
        // Quarter the disk channels in the browser build. (HDV access itself
        // is silent — the 5.25"/3.5" floppy channels are the only disk sounds,
        // so they are what the "HD noise" actually is.) The factor scales the
        // restored value, so a user who lowers the mixer slider further still
        // sticks (the mixer reads the live device volume, not settings).
#ifdef __EMSCRIPTEN__
        constexpr float kWasmDiskGain = 0.25f;
#else
        constexpr float kWasmDiskGain = 1.0f;
#endif
        controller->floppySound525().setVolume(vol525 * kWasmDiskGain);
        controller->floppySound525().setMuted (mute525);
        controller->floppySound35().setVolume(vol35 * kWasmDiskGain);
        controller->floppySound35().setMuted(
            settings->getBool ("floppy_sound_muted_35",  mute525));
        // Audio master (mixer panel). Default 1.0 / unmuted to preserve
        // pre-mixer behaviour.
        controller->audio().setMasterVolume(
            settings->getFloat("master_volume", 1.0f));
        controller->audio().setMasterMuted(
            settings->getBool ("master_muted",  false));
        // Stereo bus (2026-08-01). Off = true stereo, which is what the
        // hardware does; the switch exists for mono playback gear and
        // for anyone who would rather not have a single-AY tune arrive
        // from the left speaker only.
        controller->audio().setMonoDownmix(
            settings->getBool ("audio_mono_downmix", false));
        // Stereo placement of the mono sources. Centre by default — the
        // Apple's own speaker has no stereo position to be faithful to,
        // so this is a taste knob, not an emulation one.
        controller->speaker().pan.store(
            settings->getFloat("speaker_pan",  0.0f));
        controller->cassette().pan.store(
            settings->getFloat("cassette_pan", 0.0f));
        controller->floppySound525().pan.store(
            settings->getFloat("floppy_sound_pan",    0.0f));
        controller->floppySound35().pan.store(
            settings->getFloat("floppy_sound_pan_35", 0.0f));
    }

    // Plug all expansion cards in their user-configured slots. The
    // mapping is read from `slot_1_card`..`slot_7_card` settings; absent
    // keys fall back to the legacy defaults (DiskII=6, HDV=5, SSC=2,
    // Clock=4, ChatMauve=7) so first-run users see no regression.
    // The lock is free here (the worker starts later in this ctor), but
    // plugSlotsFromSettings takes the handle rather than the mutex on
    // purpose — its other two callers are already holding it.
    {
        auto st = controller->lockState();
        plugSlotsFromSettings(st);
    }
    // Any FujiNet card it plugged has its transport still closed — opening
    // one is a blocking syscall and must not happen under the lock above.
    (void)startDeferredFujiNetLinks();

    // ── Restore display + UI prefs from previous session ─────────────
    {
        // Default when the key is absent (fresh install) is the
        // OpenEmulator GPU composite: it is POM2's best-looking colour
        // pipeline and the one the CRT Settings panel is built around. It
        // degrades safely — with no GL shader available `NtscPostProcessor`
        // falls back to the NTSC LUT, so this can't leave a user with a
        // black screen.
        const std::string mode =
            settings->getString("hi_res_mode", "ColorCompositeOE");
        if      (mode == "ColorNTSC")       display->setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        else if (mode == "ColorCompMedium") display->setHiResMode(Apple2Display::HiResMode::ColorCompMedium);
        else if (mode == "ColorComp4Bit")   display->setHiResMode(Apple2Display::HiResMode::ColorComp4Bit);
        else if (mode == "ChatMauveRGB")    display->setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        else if (mode == "ColorCompositeOE") display->setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
        else if (mode == "ColorCompositeOECpu") display->setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
        else if (mode == "MonoWhite")       display->setHiResMode(Apple2Display::HiResMode::MonoWhite);
        else if (mode == "MonoGreen")       display->setHiResMode(Apple2Display::HiResMode::MonoGreen);
        else if (mode == "MonoAmber")       display->setHiResMode(Apple2Display::HiResMode::MonoAmber);
        else if (mode == "ColorAppleWin")   display->setHiResMode(Apple2Display::HiResMode::ColorAppleWin);
        // AppleWin NTSC: only the TV (50% line-blur) sub-mode is exposed now
        // (Monitor / Idealized were dropped) — force it regardless of any
        // legacy applewin_submode value left in the settings file.
        display->setAppleWinSubMode(Apple2Display::AppleWinSubMode::Tv);

        // Seed the toolbar's colour-side memory from what was just restored,
        // so the first mono → colour round-trip returns to the mode the user
        // is actually looking at rather than to this member's ColorNTSC
        // initialiser. Mattered the moment the default stopped being
        // ColorNTSC.
        {
            const auto m = display->getHiResMode();
            if (m == Apple2Display::HiResMode::MonoWhite ||
                m == Apple2Display::HiResMode::MonoGreen ||
                m == Apple2Display::HiResMode::MonoAmber)
                lastMonoHiResMode_ = m;
            else
                lastColorHiResMode_ = m;
        }

        // Panel visibility: one loop over the registry (MainWindow_Panels.cpp)
        // instead of the 32 hand-written lines that used to live here — and
        // the 32 that mirrored them in the save path, which is where they
        // drifted: seven panels the palette could open had no key at all, so
        // the user opened them and they were gone next launch.
        loadPanelVisibility();
        // Floppy Emu: restore the emulation mode + SD-card root (its NVRAM).
        {
            pom2::FloppyEmuMode fm;
            if (pom2::FloppyEmuDevice::modeFromKey(
                    settings->getString("floppyemu_mode", "smartporthd"), fm))
                floppyEmu->setMode(fm);
            std::string sd = settings->getString("floppyemu_sd_root", "");
            if (sd.empty()) {
                // The Floppy Emu owns a DEDICATED 'floppyemu/' folder — its
                // virtual SD card, kept separate from the Disk Library's
                // disks_5.4/ ・ disks_3.5/ ・ hdv/. Probe the usual cwd anchors and,
                // if none exists, create it so there's always a clear place
                // to drop images.
                namespace fs = std::filesystem;
                std::error_code ec;
                for (const char* c : { "floppyemu", "../floppyemu",
                                       "../../floppyemu" }) {
                    if (fs::is_directory(c, ec)) { sd = c; break; }
                }
                if (sd.empty()) {
                    fs::create_directories("floppyemu", ec);
                    sd = "floppyemu";
                }
            }
            floppyEmu->setSdRoot(sd);
        }
        {
            hgrpaint::HgrPaintEditor::Session hs;
            hs.mode       = settings->getInt   ("hgr_paint_mode",  0);
            hs.page2      = settings->getBool  ("hgr_paint_page2", false);
            hs.zoomIdx    = settings->getInt   ("hgr_paint_zoom",  2);
            hs.ntscColor  = settings->getBool  ("hgr_paint_ntsc",  true);
            hs.aspect43   = settings->getBool  ("hgr_paint_43",    false);
            hs.canvasPipeline = settings->getInt("hgr_paint_pipe", 0);
            hs.browserDir = settings->getString("hgr_paint_dir",   "");
            hgrPaintEditor->restoreSession(hs);
        }
        controller->rewind().setEnabled(settings->getBool("rewind_enabled", false));
        {
            // The whole Joystick panel binding, not just the square gate:
            // the pad the user chose, the deadzone they dialled and the two
            // invert flags all used to reset to defaults on every launch.
            auto& jb = joystick->binding();
            jb.squareGate = settings->getBool("joystick_square_gate", jb.squareGate);
            jb.deadzone   = std::clamp(
                settings->getFloat("joystick_deadzone", jb.deadzone), 0.0f, 0.9f);
            jb.invert[0]  = settings->getBool("joystick_invert_x", jb.invert[0]);
            jb.invert[1]  = settings->getBool("joystick_invert_y", jb.invert[1]);
            // -2 is "key absent": a fresh install must still auto-bind, and
            // a persisted -1 means the user chose "(none)" and the
            // auto-binder has to keep its hands off (markBindingExplicit).
            const int host = settings->getInt("joystick_host", -2);
            if (host != -2) {
                jb.hostIdx = (host >= 0 && host < JoystickInput::kHostCount)
                                 ? host : -1;
                joystick->markBindingExplicit();
            }
        }
        // Paper + raster density survive a restart; the printed sheets
        // themselves deliberately do not (they are output, like the spool).
        {
            const int paper = settings->getInt("imagewriter_paper", 0);
            if (paper > 0 && paper < static_cast<int>(
                                pom2::ImageWriter::PaperSize::Count))
                imageWriter->setPaperSize(
                    static_cast<pom2::ImageWriter::PaperSize>(paper));
            // Mechanical sound. Levels are restored here; the REGISTRATION
            // lives at the end of plugSlotsFromSettings() instead, because
            // both slot-rebuild paths (profile switch, Slot Config "Apply")
            // call unregisterAllAudioSources() and then only re-register
            // card-owned sources. Registering here meant the printer went
            // permanently silent after the first profile switch — including
            // the one the constructor itself performs when the saved profile
            // differs from the ROM auto-probe.
            printerSound->setVolume(
                settings->getFloat("printer_sound_volume", 0.35f));
            printerSound->setMuted(
                settings->getBool("printer_sound_muted", false));
            imageWriter->setSoundSink(printerSound.get());

            // Durable printouts, alongside the spool and trace files POM2
            // already writes there.
            {
                std::string herr;
                const auto historyDir = pom2::userDataDir() / "printouts" / "history";
                if (!printerHistory->open(historyDir.string(), herr))
                    pom2::log().warn("PrinterHistory", herr);
            }

            imageWriter->setModel(static_cast<pom2::IwModel>(
                std::clamp(settings->getInt("imagewriter_model", 0), 0,
                           static_cast<int>(pom2::IwModel::Count) - 1)));
            imageWriter->setDpi(settings->getInt("imagewriter_dpi",
                                                 imageWriter->dpi()));
            // Line-feed-after-CR switch. Old configs stored a bool; the
            // mode (Auto/On/Off) supersedes it and Auto is the default —
            // it settles the question from the stream itself.
            imageWriter->setAutoFeedMode(
                static_cast<pom2::ImageWriter::AutoFeed>(
                    std::clamp(settings->getInt("imagewriter_autolf_mode",
                        static_cast<int>(pom2::ImageWriter::AutoFeed::Auto)),
                        0, static_cast<int>(
                               pom2::ImageWriter::AutoFeed::Count) - 1)));
            // POM2_TRACE_PRINTER=1 (or =<path>) opens the printer trace
            // before anything can print, so a printout that goes wrong
            // during boot is captured too.
            if (const char* t = std::getenv("POM2_TRACE_PRINTER")) {
                if (*t && std::strcmp(t, "0") != 0) {
                    const std::string path =
                        (std::strcmp(t, "1") == 0)
                            ? (pom2::userDataDir() / "printouts" /
                               "imagewriter_trace.log").string()
                            : std::string(t);
                    std::string err;
                    if (imageWriter->startTrace(path, err))
                        pom2::log().info("ImageWriter", "Tracing to " + path);
                    else
                        pom2::log().warn("ImageWriter", err);
                }
            }
            printerBackPressure =
                settings->getBool("imagewriter_backpressure", false);
            imageWriter->setRibbon(
                static_cast<pom2::ImageWriter::Ribbon>(
                    std::clamp(settings->getInt("imagewriter_ribbon", 0), 0,
                               static_cast<int>(
                                   pom2::ImageWriter::Ribbon::Count) - 1)));
            const int spd = settings->getInt(
                "imagewriter_speed",
                static_cast<int>(pom2::ImageWriter::Speed::Draft));
            if (spd >= 0 && spd < static_cast<int>(
                                pom2::ImageWriter::Speed::Count))
                imageWriter->setSpeed(
                    static_cast<pom2::ImageWriter::Speed>(spd));
        }
        sscPortInput       = settings->getInt  ("ssc_port",        sscPortInput);
        diskTurboWhileMotor = settings->getBool("disk_turbo",      diskTurboWhileMotor);
        // Dallas DS1216E "No-Slot Clock" — sits under the Monitor ROM
        // and ProDOS 2.0.3+ / GS-OS auto-detect it via the magic-key
        // scan. Default ON (battery-backed RTC for all profiles incl.
        // //c, which never had a slot to host a ThunderClock card).
        controller->noSlotClock().setEnabled(
            settings->getBool("nsclock_enable", true));
        // Composite-NTSC shader params (saved under ntsc_*). We can't
        // call ntscFx->setParams() yet because the postprocessor is
        // lazy-constructed in drawScreenImage; stash them into a
        // pending-params instance that will be picked up on the first
        // construction.
        {
            pom2::NtscParams p;
            p.brightness  = settings->getFloat("ntsc_brightness",  p.brightness);
            p.contrast    = settings->getFloat("ntsc_contrast",    p.contrast);
            p.saturation  = settings->getFloat("ntsc_saturation",  p.saturation);
            p.hue         = settings->getFloat("ntsc_hue",         p.hue);
            p.sharpness   = settings->getFloat("ntsc_sharpness",   p.sharpness);
            p.persistence = settings->getFloat("ntsc_persistence", p.persistence);
            p.scanlines   = settings->getFloat("ntsc_scanlines",   p.scanlines);
            p.barrel      = settings->getFloat("ntsc_barrel",      p.barrel);
            p.shadowMaskStrength = settings->getFloat(
                "ntsc_shadow_strength", p.shadowMaskStrength);
            p.luminanceGain = settings->getFloat(
                "ntsc_luminance_gain", p.luminanceGain);
            p.centerLighting = settings->getFloat(
                "ntsc_center_lighting", p.centerLighting);
            p.phosphorGamma = settings->getFloat(
                "ntsc_phosphor_gamma", p.phosphorGamma);
            p.rgbBandwidthMHz = settings->getFloat(
                "ntsc_rgb_bandwidth_mhz", p.rgbBandwidthMHz);
            const int sm = settings->getInt("ntsc_shadow_mask",
                                            static_cast<int>(p.shadowMask));
            p.shadowMask = static_cast<pom2::NtscParams::ShadowMask>(
                std::clamp(sm, 0, 3));
            p.palMode    = settings->getBool("ntsc_pal",        p.palMode);
            p.textSharp  = settings->getBool("ntsc_text_sharp", p.textSharp);
            // Clamp every float to its slider range: only values created
            // in-app are slider-bounded — a hand-edited/corrupted state.cfg
            // with e.g. ntsc_center_lighting=0 hits 1.0/uCenterLighting in
            // the glass shader → exp(-inf) → fully black screen everywhere.
            p.brightness         = std::clamp(p.brightness,        -0.5f, 0.5f);
            p.contrast           = std::clamp(p.contrast,           0.5f, 1.5f);
            p.saturation         = std::clamp(p.saturation,         0.0f, 2.0f);
            p.hue                = std::clamp(p.hue,               -0.5f, 0.5f);
            p.sharpness          = std::clamp(p.sharpness,          0.0f, 1.0f);
            p.persistence        = std::clamp(p.persistence,        0.0f, 0.95f);
            p.scanlines          = std::clamp(p.scanlines,          0.0f, 1.0f);
            p.barrel             = std::clamp(p.barrel,             0.0f, 0.30f);
            p.shadowMaskStrength = std::clamp(p.shadowMaskStrength, 0.0f, 1.0f);
            p.luminanceGain      = std::clamp(p.luminanceGain,      1.0f, 2.0f);
            p.centerLighting     = std::clamp(p.centerLighting,     0.5f, 1.0f);
            p.phosphorGamma      = std::clamp(p.phosphorGamma,      0.6f, 2.6f);
            p.rgbBandwidthMHz    = std::clamp(p.rgbBandwidthMHz,     0.0f, 8.0f);
#ifdef __EMSCRIPTEN__
            p.barrel = std::min(p.barrel, 0.03f);
#endif
            ntscFx = std::make_unique<pom2::NtscPostProcessor>();
            ntscFx->setParams(p);
        }
        crtEffectsEnabled = settings->getBool("crt_effects_enabled",
                                              crtEffectsEnabled);
        // Own the renderer up-front (ctor is GL-free; initialize() stays lazy)
        // so the settings panel and persistence can bind to its tunables even
        // before the 3D view is first toggled on.
        if (!voxel3d_) voxel3d_ = std::make_unique<pom2::Voxel3DRenderer>();
        // Same slider-range clamps as the NTSC params above: negative depth
        // extrudes the slab away, fill 0 makes every cube invisible, and
        // ambient >1 flips the diffuse term — all persisted, so a stray
        // Ctrl+click-typed value would survive restarts.
        voxel3d_->voxelDepth  = std::clamp(settings->getFloat("voxel_depth",      voxel3d_->voxelDepth),  0.0f, 12.0f);
        voxel3d_->colorShift  = std::clamp(settings->getFloat("voxel_colorshift", voxel3d_->colorShift),  0.0f, 24.0f);
        voxel3d_->cubeFill    = std::clamp(settings->getFloat("voxel_fill",       voxel3d_->cubeFill),    0.2f, 1.0f);
        voxel3d_->ambient     = std::clamp(settings->getFloat("voxel_ambient",    voxel3d_->ambient),     0.0f, 1.0f);
        voxel3d_->superSample = std::clamp(settings->getInt  ("voxel_supersample", voxel3d_->superSample), 1, 4);
        voxel3d_->mono          = settings->getBool("voxel_mono",          voxel3d_->mono);
        voxel3d_->perColorDepth = settings->getBool("voxel_percolor_depth", voxel3d_->perColorDepth);
        const std::string asp = settings->getString("aspect_mode", "");
        if      (asp == "crt43")   aspectMode = AspectMode::Crt43;
        else if (asp == "integer") aspectMode = AspectMode::Integer;
        else if (asp == "square")  aspectMode = AspectMode::Square;

        // Interface appearance. Only stored here — the theme is applied by
        // `setDpiScale()`, which main() calls right after construction with
        // the monitor's content scale (unknown at this point). Clamped so a
        // hand-edited state.cfg can't leave the UI unusably small or huge.
        uiAccent_ = pom2::accentFromKey(
            settings->getString("ui_accent",
                                pom2::accentKey(uiAccent_)).c_str());
        uiScale_  = std::clamp(settings->getFloat("ui_scale", uiScale_),
                               pom2::kUiScaleMin, pom2::kUiScaleMax);
        // Docking: has a layout already been seeded into imgui.ini? Without
        // this the default layout would be rebuilt on every launch, throwing
        // away whatever the user had docked.
        dockSeeded_ = settings->getBool("ui_dock_seeded", false);
        libraryFavourites_ = pom2::splitSettingList(settings->getString("library_favourites", ""));
        libraryRecents_    = pom2::splitSettingList(settings->getString("library_recents", ""));
        libraryHideSizeDate_ = settings->getBool("library_hide_sizedate", false);
        if (libraryRecents_.size() > kMaxLibraryRecents)
            libraryRecents_.resize(kMaxLibraryRecents);
#ifdef __EMSCRIPTEN__
        // Browser startup is intentionally chrome-light: keep only the menu,
        // toolbar, Apple II Screen window, and bottom status bar. Users can
        // still open panels from the menus after boot.
        //
        // FIRST VISIT ONLY, since 2026-09-01. This block used to run on every
        // browser launch, which was harmless while the browser build had no
        // persistence at all and actively wrong once it did: the settings
        // store had just restored the visitor's panels three lines above
        // (loadPanelVisibility), and this closed every one of them again. A
        // returning visitor gets what they left; a new one gets the curated
        // opening. `empty()` is the honest test — no state.cfg in IDBFS, or
        // an IDBFS that could not be read.
        if (settings->empty()) {
            display->setHiResMode(Apple2Display::HiResMode::ColorCompMedium);
            lastColorHiResMode_ = Apple2Display::HiResMode::ColorCompMedium;
            // Was 28 assignments naming 28 panels, which meant every panel
            // added after it was written stayed open on the browser build —
            // the list could only rot in one direction. The registry knows
            // all of them.
            hideAllPanels();
            // …except the greeting a browser user with no ROM still needs:
            // the constructor opened it above, and chrome-light is about
            // chrome.
            if (!romLoaded_) show(pom2::PanelId::Welcome) = true;
        }
#endif
    }

    // Disk II / HDV / CFFA / SmartPort media are restored by
    // StorageCoordinator::restoreMediaFromSettings(), at the end of
    // plugSlotsFromSettings() above — one pass against the finished topology
    // rather than a second one here.
    //
    // This block used to do it, and it ran AFTER the plug pass, so every image
    // was opened twice at startup. It also called insertDisk() with the
    // default argument, so drive 2 was never restored at all: its path was
    // persisted on exit and silently ignored on the next launch.

    // ── Restore previously-mounted 3.5" disks ─────────────────────────
    // Same pattern as the 5.25" / HDV restore above. Only honour the
    // paths when the file still exists; silently skip otherwise so a
    // moved / deleted image doesn't block startup.
    {
        std::error_code ec;
        // Write-back BEFORE the mount: `mount35` reads the flag off the image
        // to decide what the staged copy inherits, and the drive presents a
        // medium with write-back off as write-protected. Persisting only the
        // paths meant a user who had opted in got the disk back read-only on
        // every launch — and lost the session's writes at the next eject.
        controller->disk35Internal().setWriteBackEnabled(
            settings->getBool("disk35_writeback_1", false));
        controller->disk35External().setWriteBackEnabled(
            settings->getBool("disk35_writeback_2", false));
        const std::string p1 = settings->getString("disk35_path_1", "");
        if (!p1.empty() && fs::is_regular_file(p1, ec) &&
            controller->mount35(0, p1)) {
            pom2::log().info("Sony35", "Internal re-mounted from settings: " + p1);
        }
        const std::string p2 = settings->getString("disk35_path_2", "");
        if (!p2.empty() && fs::is_regular_file(p2, ec) &&
            controller->mount35(1, p2)) {
            pom2::log().info("Sony35", "External re-mounted from settings: " + p2);
        }
    }

    // ── Restore audio levels ─────────────────────────────────────────
    {
        const float spkVol = settings->getFloat("speaker_volume", 1.0f);
        controller->speaker().setVolume(spkVol);
        controller->speaker().setMuted(settings->getBool("speaker_muted", false));
        controller->setCassetteVolume(settings->getFloat("cassette_volume", 0.6f));
        controller->cassette().setAutoRewind(
            settings->getBool("cassette_auto_rewind", false));
    }

    // Always wake up at the Applesoft prompt. A default HDV / disk may be
    // mounted (above), but we never auto-boot — the user picks via the
    // Disk II / HDV panel libraries. Use coldBoot (not just a CPU reset)
    // so the Apple II Monitor runs its full cold-start sequence: HOME
    // clears the freshly-zeroed text page so the user briefly sees the
    // "Apple //e" banner instead of the `@`-tile garbage that the text
    // page renders when full of $00, then it tries slot 6, fails (no
    // disk in drive at first launch), and falls through to AppleSoft.
    controller->coldBoot();
    controller->setMode(EmulationController::Mode::Running);
    controller->start();

    // ── AI control server (loopback HTTP) ────────────────────────────────
    // Wire the bridge once the emulator core is alive so the server's
    // first request hits a fully-formed emulator. Auto-start only if
    // the last session left it on — fresh users opt in via the panel.
    aiPortInput   = settings->getInt   ("ai_control_port",   aiPortInput);
    aiTokenInput  = settings->getString("ai_control_token",  "");
    aiServer->attach(controller.get(), display.get(), primaryDiskII(), primaryHdvCard());
    aiServer->setAuthToken(aiTokenInput);
    aiServer->setProfileLabel(std::string(pom2::profileConfig(activeProfile).displayName));
    if (settings->getBool("ai_control_enable", false)) {
        aiServer->start(static_cast<uint16_t>(aiPortInput));
    }

    // Determine the active profile from what the legacy boot path
    // resolved. If a `system_profile` setting was persisted from a
    // previous launch AND it disagrees with the auto-detected one, the
    // user explicitly picked that profile last time — honour it via a
    // full cold reset via applyProfile() (which the menu also calls).
    activeProfile = iiePresent ? pom2::SystemProfile::AppleIIe
                               : pom2::SystemProfile::AppleIIPlus;
    // `--ii-plus` (forceIIPlus) must win over any persisted profile: it was
    // requested precisely to avoid the IIe path. Without this guard the
    // saved-profile catch-up below would re-apply a saved iie/iic/iic+ and
    // silently defeat the flag. (forceIIPlus already suppressed the IIe ROM
    // probe above, so activeProfile is AppleIIPlus here.)
    //
    // A fresh install (no `system_profile` key) defaults to **//e Enhanced
    // PAL**: 50 Hz European timing is what the French Touch / DIX demo corpus
    // POM2 benchmarks against is written for, and it is a superset machine —
    // 128 K, 65C02, 80 columns, all seven slots free. It is expressed as a
    // default for `getString` rather than as a new `activeProfile` initialiser
    // so the catch-up below runs `applyProfile` for it, which is what actually
    // installs the PAL video standard, the 20313-cycle frame budget and the
    // per-card clock updates. Falls back to the auto-probed profile when no
    // //e ROM was found (a PAL //e with no //e ROM would just fail to boot).
    const std::string defaultProfile =
        iiePresent ? std::string("iie-pal") : std::string();
    const std::string savedProfile =
        forceIIPlus ? std::string()
                    : settings->getString("system_profile", defaultProfile);
    if (!savedProfile.empty()) {
        const pom2::SystemProfile saved = pom2::profileFromKey(savedProfile);
        if (saved != activeProfile) {
            // Saved choice differs from auto-probe — re-run the full
            // profile machinery (slots will replug, ROMs reload, etc.).
            applyProfile(saved);
        } else {
            // Same profile but the user might have selected a non-default
            // CPU mode override. Apply it.
            const auto& cfg = pom2::profileConfig(activeProfile);
            const M6502::CpuMode resolved = resolveCpuMode(cfg.defaultCpu);
            auto st = controller->lockState();
            if (resolved != st.cpu().getCpuMode())
                st.cpu().setCpuMode(resolved);
        }
    }
    // Profile-specific floppy motor pitch — applies only to the 5.25"
    // bank. The 3.5" instance keeps motorPitch=1.0 because the 35_*.wav
    // samples are already recorded at the Sony 800K cadence, so a pitch
    // bump would over-shift them. applyProfile() already calls
    // setMotorPitch internally; do it here for the paths that don't go
    // through applyProfile (auto-probe matching the saved profile, or
    // no saved profile at all).
    controller->floppySound525().setMotorPitch(floppyMotorPitchForProfile(activeProfile));

    // The machine's snapshot identity, unconditionally — `applyProfile` step
    // 10 sets it, and the two branches above that SKIP applyProfile (the
    // saved profile already matches the auto-probe, or `--ii-plus`) left it at
    // 0. A zero id makes the guard dead both ways: every snapshot taken this
    // session is stamped "unknown", and a snapshot from a real profile loads
    // onto this machine unchallenged. Cheap and idempotent when applyProfile
    // did run.
    controller->setMachineId(pom2::snapshotMachineId(activeProfile));

    // activeProfile is now fully resolved (auto-probe + saved-profile
    // catch-up). Refresh the AI server's cached label: the wiring above set it
    // from the still-default activeProfile (AppleIIPlus) BEFORE resolution, and
    // when the saved profile matches the auto-probe the applyProfile() path
    // (which also refreshes the label) is skipped — so /status would otherwise
    // report the wrong machine (e.g. "Apple ][+" while running a //e).
    aiServer->setProfileLabel(std::string(pom2::profileConfig(activeProfile).displayName));
}

// Out-of-line accessor bodies — these need EmulationController and
// Apple2Display to be complete types, which is true here but not in
// MainWindow.h (where both are forward-declared so consumers don't drag
// in the whole subsystem cone). Public API behaviour unchanged.
EmulationController& MainWindow::emul()       { return *controller; }
Apple2Display&       MainWindow::displayRef() { return *display; }

bool MainWindow::startAiControlFromCli(unsigned short port, std::string& errOut)
{
    aiServer->attach(controller.get(), display.get(), primaryDiskII(), primaryHdvCard());
    aiServer->setAuthToken("");
    if (!aiServer->start(port)) {
        errOut = "cannot listen on 127.0.0.1:" + std::to_string(port);
        return false;
    }
    pom2::log().info("CLI", "AI control listening on 127.0.0.1:" +
                              std::to_string(port));
    return true;
}

MainWindow::~MainWindow()
{
    // Stop the AI control server BEFORE the CPU worker — pending requests
    // hold `controller->stateMutex()` and call into `controller->memory()` /
    // `controller->cpu()`; we want them quiesced before we tear anything
    // else down. The server's destructor would do the same on member
    // destruction order, but doing it here keeps the dependency obvious.
    aiServer->stop();
    controller->stop();

    // Flush every mounted medium while all cards are still alive.  The old
    // teardown only covered Disk II and 3.5-inch drives, so dirty HDV/CFFA
    // blocks vanished on quit without even attempting a host write.
    std::string shutdownFlushError;
    if (!flushSlotMedia(shutdownFlushError)) {
        pom2::log().error("Disk", "save-on-shutdown failed: " + shutdownFlushError);
    }

    // Detach every audio source BEFORE any member is destroyed.
    //
    // AudioDevice keeps raw pointers and dereferences them from the miniaudio
    // callback thread every ~5 ms, and `controller->stop()` only parks the CPU
    // worker — it never touches audio. Member destruction then runs in reverse
    // declaration order, and `controller` (which owns the AudioDevice that
    // finally drains the callback) is the FIRST member, hence the last to go:
    // everything else, `printerSound` included, dies while the callback is
    // still live. Card-owned sources were safe by accident, because
    // ~EmulationController tears down Memory (and the SlotBus) after
    // audioDev.reset(); the first MainWindow-owned source inverted that.
    unregisterAllAudioSources();

    // Free the paint/sprite editors' GPU textures while the GL context is
    // still current (same window teardown order as the About-photo texture).
    // "Still current" is a guarantee main() makes, not a property of being a
    // destructor: MainWindow is owned by a unique_ptr there and reset()
    // BEFORE ImGui_ImplOpenGL3_Shutdown / glfwDestroyWindow / glfwTerminate.
    // It used to be a plain local, so every glDelete* below (and
    // ~Voxel3DRenderer's) ran after glfwTerminate() had already torn the
    // context down — a driver-dependent crash on the way out.
    if (hgrPaintEditor)  hgrPaintEditor->releaseGL();
    if (hgrSpriteEditor) hgrSpriteEditor->releaseGL();
    if (imageWriterPanel) imageWriterPanel->shutdown();

    // Deferred 3.5" write-backs (a firmware eject hands its payload to the
    // controller's queue thread) must be on disk BEFORE the settings that
    // describe the media are written, and while the log is still being
    // written where the user can read it. Without this the only drain was
    // ~EmulationController's, long after both. Takes no lock, and must not:
    // the queue takes `stateMutex` itself to report each commit.
    controller->drainDeferredWriteBacks();

    // Everything the next launch needs to look like this one. Extracted to
    // MainWindow_Session.cpp so the browser build — whose MainWindow is never
    // destroyed, see that file — can call it on a heartbeat. `false`: the
    // flush above already ran, under the lock.
    persistSession(false);

    if (aboutImageTex_) {
        GLuint t = aboutImageTex_;
        glDeleteTextures(1, &t);
        aboutImageTex_ = 0;
    }
    if (kbImageTex_) {
        GLuint t = kbImageTex_;
        glDeleteTextures(1, &t);
        kbImageTex_ = 0;
    }
}

// ─── Slot configuration ─────────────────────────────────────────────────
//
// `plugSlotsFromSettings()` is the single source of truth for which card
// is in which slot. It reads `slot_1_card`..`slot_7_card` from the runtime
// settings store, falling back to the historical defaults below when a
// slot key is absent (so first-run users see no regression). Each card is
// constructed with its slot number passed to the constructor — the slot
// is baked into card slot ROMs (PR#n entry points, ProDOS unit numbers,
// etc.) so we can't just plug a "slot-2-style" SSC into slot 5 and expect
// PR#5 to find it.
//
// Validation: each card-type identifier appears in at most one slot. A
// duplicate request logs a warning and skips the second instance. Empty
// slots are simply not plugged.
//
// Identifiers (canonical strings stored in settings):
//   ""           empty slot
//   "diskii"     DiskIICard
//   "hdv"        ProDOSHardDiskCard
//   "ssc"        SuperSerialCard
//   "clock"      ClockCard
//   "chatmauve"  LeChatMauveCard
//   "mouse"      MouseCard (Phase 4 — falls through with a warning until then)
//   "mockingboard"  MockingboardCard (Sweet Microsystems A/C — 6522×2 + AY×2)

// ─── Audio-source inventory ──────────────────────────────────────────────
//
// See MainWindow.h: AudioDevice holds raw pointers that the miniaudio
// callback thread dereferences, so every source registered here must be
// unregistered before the SlotBus destroys the card that owns it. Going
// through this pair (rather than the named `*Card` aliases) is what makes
// that hold for multi-instance configurations — two Mockingboard variants,
// a future second Phasor — without each new card type having to remember
// to add a line to two teardown blocks.

void MainWindow::registerAudioSource(AudioSource* src)
{
    // Idempotent, and it stays idempotent: the printer sound is re-registered
    // from every plugSlotsFromSettings() pass, and a double entry would mix
    // the source twice and then dangle after a single removeSource().
    audioCoordinator_->registerSource(src);
}

void MainWindow::unregisterAllAudioSources()
{
    audioCoordinator_->unregisterAll();
}

// ─── Screenshot ───────────────────────────────────────────────────────────

// ─── Keyboard ─────────────────────────────────────────────────────────────

// ─── Paste ───────────────────────────────────────────────────────────────

// ─── Texture upload ──────────────────────────────────────────────────────

// ─── Disk Library favourites / recents ───────────────────────────────────

// ─── Command palette ─────────────────────────────────────────────────────
//
// One list, one dispatch switch. Adding a command means one line in
// buildCommands() plus one `if` in runCommand() — deliberately not a
// registration mechanism with callbacks, because the whole value of the
// palette is that every command is visible in one place when you read it.

// ─── Docking ─────────────────────────────────────────────────────────────

void MainWindow::renderDockSpace()
{
    // The dockspace covers the viewport WORK area, which the main menu bar,
    // the toolbar and the status bar have each already reserved a slice of
    // (all three are `BeginViewportSideBar` windows). So the chrome is never
    // overlapped and never needs hardcoded offsets.
    //
    // PassthruCentralNode: when nothing is docked in the middle, the central
    // node draws no background. Without it an empty centre is a grey slab
    // covering the whole work area.
    dockspaceId_ = ImGui::DockSpaceOverViewport(
        ImGui::GetID("POM2_DockSpace"), ImGui::GetMainViewport(),
        ImGuiDockNodeFlags_PassthruCentralNode);

    // Seed the default layout the first time POM2 runs with docking (or when
    // the user picks a preset). Gated on a persisted flag rather than on "is
    // the node empty": DockSpaceOverViewport has already created the node by
    // this point, so emptiness can't distinguish "fresh install" from "user
    // undocked everything on purpose".
    if (!dockSeeded_) {
        dockSeeded_          = true;
        dockLayoutRequested_ = true;
        pendingDockLayout_   = DockLayout::Reset;
    }
    if (dockLayoutRequested_) {
        dockLayoutRequested_ = false;
        applyDockLayout(pendingDockLayout_);
    }
}

void MainWindow::applyDockLayout(DockLayout preset)
{
    if (dockspaceId_ == 0) return;

    // Rebuild from scratch. RemoveNode undocks everything first, so windows
    // the preset doesn't mention end up floating rather than stuck in a
    // stale node.
    ImGui::DockBuilderRemoveNode(dockspaceId_);
    ImGui::DockBuilderAddNode(dockspaceId_, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId_,
                                  ImGui::GetMainViewport()->WorkSize);

    // `centre` is rebound by each split to the *remaining* opposite side, so
    // successive splits carve off the outside and leave the screen in the
    // middle. SetNodeSize above matters: split ratios are computed against
    // the node's size, and without it the first split's sizes are unreliable.
    ImGuiID centre = dockspaceId_;
    ImGuiID right = 0, rightLower = 0, bottom = 0;

    switch (preset) {
        case DockLayout::Reset:
            right      = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
                                                     0.34f, nullptr, &centre);
            rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
                                                     0.45f, nullptr, &right);
            break;
        case DockLayout::Emulation:
            // No inspectors: one right column, all storage.
            right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
                                                0.32f, nullptr, &centre);
            rightLower = right;
            break;
        case DockLayout::Debug:
            right      = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
                                                     0.38f, nullptr, &centre);
            bottom     = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down,
                                                     0.30f, nullptr, &centre);
            rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
                                                     0.50f, nullptr, &right);
            break;
        case DockLayout::Audio:
            right      = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
                                                     0.40f, nullptr, &centre);
            rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
                                                     0.35f, nullptr, &right);
            break;
    }
    if (rightLower == 0) rightLower = right;
    if (bottom     == 0) bottom     = rightLower;

    // The screen is always the centre. Its window carries NoMove + a manual
    // title-bar drag; `renderScreenWindow` disables that drag while docked so
    // the two don't fight (a docked window is moved by its tab, not its body).
    ImGui::DockBuilderDockWindow("Apple II Screen", centre);

    // Everything below docks by literal title. Panels that are hidden right
    // now still get assigned — the assignment is what makes them open as a
    // tab in the right group later instead of floating over the screen, which
    // is most of the value of doing this at all.
    auto dock = [](const char* title, ImGuiID node) {
        ImGui::DockBuilderDockWindow(title, node);
    };

    switch (preset) {
        case DockLayout::Reset:
            // The default startup trio, tabbed to the right of the screen:
            // what you mount (Disk Library), what the machine is made of
            // (Slot Configuration), what it prints (ImageWriter II). All
            // three default to visible, so a fresh install opens on exactly
            // this arrangement. Disk Library is docked first, so it is the
            // selected tab.
            dock("Disk Library", right);
            dock("Slot Configuration", right);
            dock(ICON_FA_PRINT " ImageWriter II###imageWriterPanel", right);
            // Inspector tab group, bottom right.
            dock("Cassette Deck", rightLower);
            dock("Floppy Emu (BMOW)", rightLower);
            dock("Memory viewer", rightLower);
            dock("Mockingboard (VIA + AY state)", rightLower);
            dock("Mouse Inspector", rightLower);
            dock("CRT Settings (Composite NTSC)", rightLower);
            dock("Audio Mixer", rightLower);
            dock("Joystick", rightLower);
            dock("Rewind", rightLower);
            break;

        case DockLayout::Emulation:
            dock("Disk Library", right);
            dock("Cassette Deck", right);
            dock("Floppy Emu (BMOW)", right);
            dock("Internal Disks & Media", right);
            dock("Slot Configuration", right);
            dock("Rewind", right);
            break;

        case DockLayout::Debug:
            dock("Memory viewer", right);
            dock("Memory Map Grid", right);
            dock("Memory Map Bar", right);
            dock("Mouse Inspector", rightLower);
            dock("No-Slot Clock (Dallas DS1216E)###nsclockPanel", rightLower);
            dock("AI Control (HTTP)", rightLower);
            dock("Memory Map Bar (Horizontal)", bottom);
            break;

        case DockLayout::Audio:
            dock("Mockingboard (VIA + AY state)", right);
            dock("Phasor (mode + 2×VIA + 4×AY)", right);
            dock("Echo+ (SSI263 speech)", right);
            dock("Audio Mixer", rightLower);
            dock("Cassette Deck", rightLower);
            break;
    }

    ImGui::DockBuilderFinish(dockspaceId_);
}

// ─── Interface appearance ────────────────────────────────────────────────

void MainWindow::applyUiTheme()
{
    pom2::applyTheme(uiAccent_, uiScale_, dpiScale_);
}

void MainWindow::setDpiScale(float s)
{
    // Guard against a windowing system reporting 0 (or something absurd) —
    // a zero scale would collapse every padding to 0 and hide the font.
    dpiScale_ = (s > 0.1f && s < 8.0f) ? s : 1.0f;
    applyUiTheme();
}

// ─── Render passes ───────────────────────────────────────────────────────

// The Apple II screen carries NO capture caption. It used to carry two — a
// "Click to capture the mouse" hint and a how-to-get-out reminder — and both
// existed to paper over the click-to-grab contract that is now gone: a click
// that silently changed what the mouse did had to announce itself, and a user
// who got captured by accident had to be told the way out.
//
// Neither problem exists any more. Capture is entered only by Ctrl+Alt+G or a
// middle click, and each of those is also the way out, so anyone captured got
// there deliberately and already knows the gesture. The standing reminder is
// the status-bar GRAB chip (with a long-lived hint beside it); painting over
// the emulated screen to say the same thing is exactly the clutter the chip
// exists to avoid.

// ─── Kiosk disk selector (gamepad-driven) ───────────────────────────────
//
// A keyboard-free way to flip disks in kiosk mode: Start opens a list of the
// 5.25" images sitting in the same folder as the booted disk (so a game's
// "Side B" is one press away), D-pad/stick move, A mounts the highlighted one
// into the boot Disk II drive (slot 6, drive 1) without rebooting — the
// flip-disk gesture Wings of Fury and friends expect — and B/Start dismiss.

// ─── Joystick / paddles ──────────────────────────────────────────────────

// ─── Abstraction Levels (LLE / HLE) ──────────────────────────────────────
//
// The window is `AbstractionLevels_ImGui` and the catalog of subsystems is
// static data beside it; what lives here is the part only MainWindow can
// answer — which cards are on the bus, and which of them are running their
// real ROM versus a fallback. That distinction is the panel's reason to
// exist: `docs/lle_vs_hle.md` § "Keeping a level once you have it" names
// silent degradation as a structural hole, because every ROM-driven low
// level in POM2 falls back to a working higher one when its dump is absent
// and nothing anywhere says so.

// ─── Disk Library (unified browser: 5.25 / 3.5 / HDV) ───────────────────

// ─── HDV (slot 5) ────────────────────────────────────────────────────────

// ─── //c+ SmartPort 3.5" ─────────────────────────────────────────────────

// ─── Apple //e keyboard (clickable photo) ────────────────────────────────

void MainWindow::render()
{
    // Track wallclock between frames so the deck counter / armed pulse /
    // status overlay can age correctly.
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    const double now = std::chrono::duration<double>(clock::now() - t0).count();
    const float deltaSeconds = static_cast<float>(std::max(0.0, now - lastFrameTime));
    lastFrameTime = now;

    // Screen-widget hover is re-established by renderScreenWindow() further
    // down, if it draws at all. Clearing it here is what makes "the screen
    // window is collapsed / hidden / not reached this frame" mean "the
    // pointer is not the guest's" — a latched `true` would keep feeding the
    // Mouse Card from a widget that is no longer on screen.
    screenHovered_ = false;

    pollJoystickAndPushToMemory();

    // A pointer capture only makes sense while a Mouse Card is on the bus.
    // Every path that can take one away — Slot Configuration, a profile
    // switch, a snapshot restore — unplugs both mouse cards, so
    // releasing here covers all of them at one point instead of chasing
    // each call site. Kiosk deliberately keeps the grab (it is the mode
    // most likely to want it), which is why this sits above the kiosk
    // early-out below.
    if (mouseGrabbed_ && !mouseCoordinator_->capture().plugged()) setMouseGrab(false);

    // Decide CPU turbo from disk activity every frame, independent of whether
    // any disk panel window is open (the disk panel defaults to hidden).
    updateAutoTurbo();

    // Kiosk: only the screen, no chrome. Joystick + auto-turbo above still
    // run so the machine behaves identically; everything else is skipped.
    // F6 hold-to-rewind still works (no toolbar button in kiosk).
    if (kiosk_) {
        // F6 is inert while the menu has the machine parked: releaseHold →
        // rewindEndAndResume would setMode(Running) behind the overlay.
        driveRewindHold(!kioskMenuOpen_ && ImGui::IsKeyDown(ImGuiKey_F6));
        updateKioskMenu();         // Start/Select drive the in-game menu
        renderKiosk();
        renderKioskMenu();         // overlay drawn on top of the screen
        // The printer still runs behind the chrome-free screen — without
        // this a //c printing in kiosk mode parked every byte in the card
        // spool forever (unbounded growth, nothing on paper).
        pumpImageWriter();
        return;
    }

    renderMenuBar();
    // Toolbar must render after the menu bar so we know its height
    // (`ImGui::GetFrameHeight()` reflects the menu bar font size +
    // padding). It's positioned just below — pinned, can't be moved
    // or resized.
    {
        pom2::Toolbar_ImGui::Snapshot tb;
        const auto mode = controller->getMode();
        tb.isRunning          = (mode == EmulationController::Mode::Running);
        tb.isStopped          = (mode == EmulationController::Mode::Stopped);
        tb.cyclesPerFrame     = controller->getCyclesPerFrame();
        tb.videoStandard      = controller->getVideoStandard();
        tb.memoryGridVisible  = show(pom2::PanelId::MemGrid);
        tb.activeProfile      = activeProfile;
        tb.hasPrimaryDiskCard = (primaryDiskII() != nullptr);
        tb.charRomLocale      = charRomLocale;
        auto isMonoHiRes = [](Apple2Display::HiResMode m) {
            return m == Apple2Display::HiResMode::MonoWhite ||
                   m == Apple2Display::HiResMode::MonoGreen ||
                   m == Apple2Display::HiResMode::MonoAmber;
        };
        tb.displayIsMono      = isMonoHiRes(display->getHiResMode());
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            tb.rewindEnabled   = controller->rewind().enabled();
            tb.rewindHasFrames = !controller->rewind().empty();
        }

        const auto tr = toolbar->render(ImGui::GetFrameHeight(), tb);
#ifdef __EMSCRIPTEN__
        auto browserResetBoot = [&]() -> bool {
            if (browserResetBootImage_.empty()) return false;
            std::string err;
            if (insertAndBootImage(browserResetBootImage_, err)) {
                tapeStatusMessage = "Boot: " + browserResetBootImage_;
                tapeStatusUntil = lastFrameTime + 3.0;
            } else {
                tapeStatusMessage = "Boot failed: " + err;
                tapeStatusUntil = lastFrameTime + 5.0;
            }
            return true;
        };
#endif
        if (tr.requestColdBoot) {
#ifdef __EMSCRIPTEN__
            if (!browserResetBoot())
#endif
            controller->coldBoot();
        }
        if (tr.requestSoftReset) {
#ifdef __EMSCRIPTEN__
            if (!browserResetBoot())
#endif
            controller->softReset();
        }
        if (tr.requestHardReset)       controller->hardReset();
        if (tr.requestPauseToggle) {
            controller->setMode(tb.isRunning
                ? EmulationController::Mode::Stopped
                : EmulationController::Mode::Running);
        }
        if (tr.requestStep)            controller->requestStep();
        if (tr.requestScreenshot)      saveScreenshot();
        if (tr.setCyclesPerFrame > 0)
            controller->setCyclesPerFrame(tr.setCyclesPerFrame);
        if (tr.setProfileRequested)    applyProfile(tr.setProfile);
        if (tr.requestMemoryGridToggle) show(pom2::PanelId::MemGrid) = !show(pom2::PanelId::MemGrid);
        // Same entry point as F10 and the View menu item, so the three
        // routes into kiosk cannot drift apart.
        if (tr.requestKioskToggle)      toggleKioskMode();
        if (tr.requestAbout)            showAbout = true;
        if (tr.requestMonoColorToggle) {
            // Flip color ↔ monochrome, remembering each side's submode so a
            // round-trip restores the user's exact choice. Persisted via the
            // dtor's hi_res_mode write, like the View menu picks.
            const auto curHi = display->getHiResMode();
            if (isMonoHiRes(curHi)) {
                lastMonoHiResMode_ = curHi;
                display->setHiResMode(lastColorHiResMode_);
            } else {
                lastColorHiResMode_ = curHi;
                display->setHiResMode(lastMonoHiResMode_);
            }
        }
        if (tr.requestInsertDisk && diskPanel) {
            // Reuse the existing per-panel popup machinery: setting the
            // primary panel's `insertDialogOpen` flag is exactly what
            // its own "Insert .dsk…" button does. `renderDiskFileDialog`
            // picks it up next frame and routes to `primaryDiskII()`.
            diskPanel->insertDialogOpen = true;
            if (diskPanel->dialogPath.empty()) diskPanel->dialogPath = "disks_5.4/";
        }
        if (tr.setCharRomRequested) {
            // Hot swap: Memory::loadCharRom rewrites the csbits table
            // in place; Apple2Display re-reads `mem.charRom()` on every
            // frame so the new glyphs show up at the next render. No
            // cold reset needed.
            charRomLocale = tr.setCharRomLocale;
            std::string newPath;
            if (charRomLocale == pom2::CharRomLocale::ProfileDefault) {
                // Replay the active profile's probe order (which
                // already lists path candidates resolvable from both
                // repo root and build/, via the SystemProfile config).
                const auto& cfg = pom2::profileConfig(activeProfile);
                for (const auto& p : cfg.charRomProbeOrder) {
                    const std::string r = pom2::resolveCharRomPath(p);
                    if (!r.empty()) { newPath = r; break; }
                }
            } else {
                newPath = pom2::resolveCharRomPath(charRomLocale);
            }
            namespace fs = std::filesystem;
            if (!newPath.empty() && fs::exists(newPath)) {
                // The lock scope ends at `loadCharRom`. `settings->save()`
                // is a write + fsync + rename — hundreds of microseconds to
                // tens of milliseconds — and stateMutex is taken by the CPU
                // worker every 4096-cycle chunk AND by this very thread to
                // paint the next frame, so holding it across the commit
                // freezes machine and window together. Same shape as
                // StorageCoordinator::ejectDiskII: mutate under the lock,
                // persist outside it.
                bool loaded = false;
                {
                    auto st = controller->lockState();
                    loaded = st.memory().loadCharRom(
                        newPath.c_str(), pom2::charRomBank(charRomLocale));
                }
                if (loaded) {
                    charRomPath = newPath;
                    settings->setString("char_rom_locale",
                        pom2::charRomLocaleKey(charRomLocale));
                    settings->save();
                    pom2::log().info("CharRom",
                        std::string("Switched to ") + newPath);
                } else {
                    pom2::log().warn("CharRom",
                        std::string("loadCharRom failed for ") + newPath);
                }
            } else {
                pom2::log().warn("CharRom",
                    std::string("Selected ROM missing: ") +
                    (newPath.empty() ? "(no path)" : newPath));
            }
        }

        // Hold-to-rewind from either input source: F6 (works everywhere) or
        // the toolbar's rewind button (held this frame). One edge-tracker.
        driveRewindHold(ImGui::IsKeyDown(ImGuiKey_F6) || tr.requestRewindHeld);
    }
    // After the menu bar + toolbar (both reserve viewport work area), before
    // any dockable window: the DockSpace has to exist when the panels below
    // call Begin(), or their first frame renders undocked.
    renderDockSpace();
    renderScreenWindow();

    // Every panel, in catalog order, each drawn only while it is visible.
    // This was 43 hand-ordered calls — some gated here, most gating
    // themselves, in an order that was the order somebody happened to add
    // them in. What is left around it is the code that is NOT a panel: the
    // modal file dialogs, the printer pump (a side effect that must run
    // whether or not its window is open), the About box, the status bar and
    // the palette overlay, which must stay last so it draws above everything.
    renderPanels(deltaSeconds);

    // The printer keeps consuming its card's spool with every window shut —
    // without this a //c printing with the panel closed parked every byte in
    // the spool forever, nothing on paper.
    pumpImageWriter();

    renderTapeFileDialogs();
    renderPasteFileDialog();
    renderHdvFileDialog();
    renderDiskFileDialog();
    renderDisk35FileDialog();
    renderAboutDialog();
    renderStatusBar();
    // Last: the palette is an overlay and must draw above every panel.
    renderCommandPalette();

    // Hide the host OS cursor whenever the AppleWin HLE firmware is
    // driving a visible emulated cursor AND the host pointer is over the
    // Apple II Screen widget — the two cursors are otherwise stacked and
    // distracting. The ImGui-Glfw backend honours
    // ImGuiMouseCursor_None at EndFrame by calling
    // glfwSetInputMode(GLFW_CURSOR_HIDDEN); on the next frame, leaving
    // it Normal (default ImGuiMouseCursor_Arrow) brings the OS cursor
    // back. The screen rect is fresh because `renderScreenWindow()` ran
    // earlier this frame.
    const auto mouseInventory = mouseCoordinator_->capture();
    if (mouseInventory.appleWinPlugged) {
        const bool mouseOn = mouseInventory.appleWin.mouseOn();
        const float w = screenRectMax.x - screenRectMin.x;
        const float h = screenRectMax.y - screenRectMin.y;
        const bool insideWidget =
            w > 0.0f && h > 0.0f &&
            lastMouseHostX >= double(screenRectMin.x) &&
            lastMouseHostX <= double(screenRectMax.x) &&
            lastMouseHostY >= double(screenRectMin.y) &&
            lastMouseHostY <= double(screenRectMax.y);
        if (mouseOn && insideWidget) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        }
    }
}
