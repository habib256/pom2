// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "MainWindow.h"

// Heavy headers — pulled here so MainWindow.h can stay forward-declared.
// Touch any of these only recompiles the MainWindow_*.cpp TUs, not every
// file that includes MainWindow.h.
#include "AiControlServer.h"
#include "Apple2Display.h"
#include "Version.h"
#include "CassetteDeck_ImGui.h"
#include "Rewind_ImGui.h"
#include "CassetteDevice.h"
#include "CharRomCatalog.h"
#include "ClockCard.h"
#include "SoftCardZ80.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "GrapplerCard.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "PrintToEmail.h"
#include "Disk35Controller_ImGui.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "DiskLibrary_ImGui.h"
#include "EmulationController.h"
#include "HdvController_ImGui.h"
#include "JoystickInput.h"
#include "JoystickPanel_ImGui.h"
#include "LeChatMauveCard.h"
#include "LeChatMauve_ImGui.h"
#include "Logger.h"
#include "MemoryViewer_ImGui.h"
#include "HgrPaintEditor.h"     // portable hgrpaint/ editor (shared with POM1)
#include "HgrSpriteEditor.h"    // portable hgrsprite/ editor (same host seam)
#include "Pom2HgrPaintHost.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "NtscPostProcessor.h"
#include "CrtEffectStack.h"
#include "Voxel3DRenderer.h"
#include "CffaCard.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "IconsFontAwesome6.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "FloppyEmuDevice.h"
#include "FloppyEmu_ImGui.h"
#include "SmartPort_ImGui.h"
#include "SpeakerDevice.h"
#include "SuperSerialCard.h"
#include "SystemProfile.h"
#include "Toolbar_ImGui.h"

#include "imgui.h"
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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

// GL types/symbols are needed by uploadScreenTexture (further down) AND by
// the About-dialog photo loader called from this same translation unit.
// Pulling the platform-correct header once at the top keeps both sites
// working without duplicating the #if/#else block.
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// stb_image is bundled (single-header public-domain JPEG/PNG decoder)
// solely for the About-dialog Apple ][+ photo. The implementation macro
// is defined *here* so symbols land in MainWindow.cpp.o and nowhere else.
// STB_IMAGE_STATIC keeps the unused entry points internal; we suppress
// the resulting -Wunused-function noise locally rather than tagging the
// third-party header.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace {
// Sentinel prefix used in HdvController_ImGui::LibraryEntry::fullPath to
// flag the synthetic prodos_folder/ host-folder mount. The dispatcher in
// renderHdvPanelWindow detects this prefix and routes to the synthesiser
// instead of treating the path as a real .hdv file.
constexpr const char* kProDOSHostSentinel = "@PRODOS_HOST_FOLDER@:";
} // namespace

MainWindow::MainWindow(bool forceIIPlus)
    // Member init order matches declaration order in MainWindow.h: the
    // controller is constructed first so memViewer can safely call
    // controller->memory() in its initialiser. Settings + AiControlServer
    // are heap-allocated for the same reason as the rest — keep their
    // headers out of MainWindow.h.
    : controller     (std::make_unique<EmulationController>()),
      display        (std::make_unique<Apple2Display>()),
      memViewer      (std::make_unique<MemoryViewer_ImGui>(&controller->memory())),
      settings       (std::make_unique<pom2::Settings>()),
      cassetteDeck   (std::make_unique<pom2::CassetteDeck_ImGui>()),
      rewindPanel_   (std::make_unique<pom2::Rewind_ImGui>()),
      disk35Panel    (std::make_unique<pom2::Disk35Controller_ImGui>()),
      diskLibrary    (std::make_unique<pom2::DiskLibrary_ImGui>()),
      hdvPanel       (std::make_unique<pom2::HdvController_ImGui>()),
      smartPortPanel (std::make_unique<pom2::SmartPort_ImGui>()),
      floppyEmu      (std::make_unique<pom2::FloppyEmuDevice>()),
      floppyEmuPanel (std::make_unique<pom2::FloppyEmu_ImGui>()),
      joystickPanel  (std::make_unique<pom2::JoystickPanel_ImGui>()),
      chatMauvePanel (std::make_unique<pom2::LeChatMauve_ImGui>()),
      toolbar        (std::make_unique<pom2::Toolbar_ImGui>()),
      hgrPaintHost   (std::make_unique<Pom2HgrPaintHost>(controller.get())),
      hgrPaintEditor (std::make_unique<hgrpaint::HgrPaintEditor>(hgrPaintHost.get())),
      hgrSpriteEditor(std::make_unique<hgrsprite::HgrSpriteEditor>(hgrPaintHost.get())),
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
    memViewer->setWriteCallback([this](uint16_t a, uint8_t v) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        controller->memory().memWrite(a, v);
    });

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
        showWelcomePanel = true;
    }
    controller->memory().loadCharRom(charRomPath.c_str());

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
    }

    // Plug all expansion cards in their user-configured slots. The
    // mapping is read from `slot_1_card`..`slot_7_card` settings; absent
    // keys fall back to the legacy defaults (DiskII=6, HDV=5, SSC=2,
    // Clock=4, ChatMauve=7) so first-run users see no regression.
    plugSlotsFromSettings();

    // ── Restore display + UI prefs from previous session ─────────────
    {
        const std::string mode = settings->getString("hi_res_mode", "");
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

        showDiskPanel      = settings->getBool ("show_disk_panel", showDiskPanel);
        showDisk35Panel    = settings->getBool ("show_disk35_panel", showDisk35Panel);
        showDiskLibrary    = settings->getBool ("show_disk_library", showDiskLibrary);
        showHdvPanel       = settings->getBool ("show_hdv_panel",  showHdvPanel);
        showSmartPortPanel = settings->getBool ("show_smartport_panel", showSmartPortPanel);
        showSlotConfigPanel = settings->getBool ("show_slot_config", showSlotConfigPanel);
        showFloppyEmu      = settings->getBool ("show_floppy_emu", showFloppyEmu);
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
        showCassetteDeck   = settings->getBool ("show_cassette",   showCassetteDeck);
        showHgrPaintEditor = settings->getBool ("show_hgr_paint",  showHgrPaintEditor);
        showHgrSpriteEditor = settings->getBool("show_hgr_sprite", showHgrSpriteEditor);
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
        showRewindBar      = settings->getBool ("show_rewind",     showRewindBar);
        controller->rewind().setEnabled(settings->getBool("rewind_enabled", false));
        showJoystickPanel  = settings->getBool ("show_joystick",   showJoystickPanel);
        joystick->binding().squareGate =
            settings->getBool("joystick_square_gate",
                              joystick->binding().squareGate);
        showMouseInspector = settings->getBool ("show_mouse_inspector",
                                                 showMouseInspector);
        showChatMauvePanel = settings->getBool ("show_chatmauve",  showChatMauvePanel);
        showMockingboardPanel = settings->getBool ("show_mockingboard",
                                                  showMockingboardPanel);
        showPhasorPanel    = settings->getBool ("show_phasor",     showPhasorPanel);
        showEchoPlusPanel  = settings->getBool ("show_echoplus",   showEchoPlusPanel);
        showAudioMixer     = settings->getBool ("show_mixer",      showAudioMixer);
        showSscPanel       = settings->getBool ("show_ssc",        showSscPanel);
        showPrinterPanel   = settings->getBool ("show_printer",    showPrinterPanel);
        printerEmailTo     = settings->getString("printer_email_to", printerEmailTo);
        sscPortInput       = settings->getInt  ("ssc_port",        sscPortInput);
        diskTurboWhileMotor = settings->getBool("disk_turbo",      diskTurboWhileMotor);
        // Dallas DS1216E "No-Slot Clock" — sits under the Monitor ROM
        // and ProDOS 2.0.3+ / GS-OS auto-detect it via the magic-key
        // scan. Default ON (battery-backed RTC for all profiles incl.
        // //c, which never had a slot to host a ThunderClock card).
        controller->noSlotClock().setEnabled(
            settings->getBool("nsclock_enable", true));
        showNoSlotClockPanel = settings->getBool("show_nsclock",
                                                 showNoSlotClockPanel);
        showNtscSettings   = settings->getBool("show_ntsc",
                                               showNtscSettings);
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
#ifdef __EMSCRIPTEN__
            p.barrel = std::min(p.barrel, 0.03f);
#endif
            ntscFx = std::make_unique<pom2::NtscPostProcessor>();
            ntscFx->setParams(p);
        }
        crtEffectsEnabled = settings->getBool("crt_effects_enabled",
                                              crtEffectsEnabled);
        show3dVoxel_      = settings->getBool("show_3d_voxel", show3dVoxel_);
        showVoxelSettings_ = settings->getBool("show_voxel_settings",
                                               showVoxelSettings_);
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
#ifdef __EMSCRIPTEN__
        // Browser startup is intentionally chrome-light: keep only the menu,
        // toolbar, Apple II Screen window, and bottom status bar. Users can
        // still open panels from the menus after boot.
        display->setHiResMode(Apple2Display::HiResMode::ColorCompMedium);
        lastColorHiResMode_ = Apple2Display::HiResMode::ColorCompMedium;
        showDiskPanel = showDisk35Panel = showDiskLibrary = false;
        showHdvPanel = showSmartPortPanel = showSlotConfigPanel = false;
        showFloppyEmu = showCassetteDeck = showJoystickPanel = false;
        showMouseInspector = showChatMauvePanel = false;
        showMockingboardPanel = showPhasorPanel = showEchoPlusPanel = false;
        showAudioMixer = showSscPanel = showPrinterPanel = false;
        showNoSlotClockPanel = showNtscSettings = showAiControlPanel = false;
        showVoxelSettings_ = false;
        showMemViewer = showMemoryBar = showMemoryBarH = showMemoryGrid = false;
#endif
    }

    // ── Restore Disk II state per-slot ────────────────────────────────
    // Each plugged DiskII gets its own `disk_path_slotN` /
    // `disk_writeback_slotN` keys. The primary (lowest-slot) card also
    // honours the legacy unsuffixed `disk_path` / `disk_writeback` keys
    // so settings written before multi-instance support keep loading.
    for (auto* c : diskCards) {
        if (!c) continue;
        const std::string slotKey = "_slot" + std::to_string(c->getSlot());
        const bool isPrimary = (c == diskCard);
        const bool wb = settings->getBool(
            "disk_writeback" + slotKey,
            isPrimary ? settings->getBool("disk_writeback", false) : false);
        c->setWriteBackEnabled(wb);
        const std::string diskPath = settings->getString(
            "disk_path" + slotKey,
            isPrimary ? settings->getString("disk_path", "") : std::string());
        std::error_code ec;
        if (!diskPath.empty() && fs::is_regular_file(diskPath, ec)) {
            if (c->insertDisk(diskPath)) {
                pom2::log().info("Disk II",
                    "slot " + std::to_string(c->getSlot()) +
                    " re-inserted from settings: " + diskPath);
            }
        }
    }

    // ── Restore previously-mounted 3.5" disks ─────────────────────────
    // Same pattern as the 5.25" / HDV restore above. Only honour the
    // paths when the file still exists; silently skip otherwise so a
    // moved / deleted image doesn't block startup.
    {
        std::error_code ec;
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
    aiServer->attach(controller.get(), display.get(), diskCard, hdvCard);
    aiServer->setAuthToken(aiTokenInput);
    aiServer->setProfileLabel(std::string(pom2::profileConfig(activeProfile).displayName));
    showAiControlPanel = settings->getBool("show_ai_control", showAiControlPanel);
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
    const std::string savedProfile =
        forceIIPlus ? std::string() : settings->getString("system_profile", "");
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
            if (resolved != controller->cpu().getCpuMode()) {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                controller->cpu().setCpuMode(resolved);
            }
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

bool MainWindow::setChatMauveInvertBit7(bool v)
{
    if (!chatMauveCard) return false;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        chatMauveCard->setInvertBit7(v);
    }
    if (settings) settings->setBool("chatmauve_invert_bit7", v);
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

    // Free the paint/sprite editors' GPU textures while the GL context is
    // still current (same window teardown order as the About-photo texture).
    if (hgrPaintEditor)  hgrPaintEditor->releaseGL();
    if (hgrSpriteEditor) hgrSpriteEditor->releaseGL();

    // Persist the current state so the next launch restores the same
    // mounted disks, video mode, panels, and audio levels.
    // Skip persisting an HDV card that ensureHdvCardForBoot auto-plugged for
    // a one-shot `POM2 <image.hdv>` boot — it's session-local by contract.
    const bool hdvIsAutoProvisioned =
        hdvCard && hdvCard->getSlot() == autoProvisionedHdvSlot_;
    if (!hdvIsAutoProvisioned && hdvCard && hdvCard->isImageLoaded()) {
        // Don't persist the synthesised host-folder volume — the path is
        // a sentinel, not a real file. Re-synthesis happens on click.
        const std::string& p = hdvCard->getImagePath();
        if (p.rfind("[host folder] ", 0) == std::string::npos) {
            settings->setString("hdv_path", p);
        } else {
            settings->setString("hdv_path", "");
        }
    } else {
        settings->setString("hdv_path", "");
    }

    // Persist per-slot DiskII state. The primary (lowest-slot) card ALSO
    // writes to the legacy unsuffixed `disk_path` / `disk_writeback` so
    // an older POM2 build reading this settings.ini still sees the disk.
    for (auto* c : diskCards) {
        if (!c) continue;
        const std::string slotKey = "_slot" + std::to_string(c->getSlot());
        const std::string p = c->isDiskLoaded() ? c->getDiskPath() : std::string();
        settings->setString("disk_path"      + slotKey, p);
        settings->setBool  ("disk_writeback" + slotKey, c->isWriteBackEnabled());
        if (c == diskCard) {
            settings->setString("disk_path",      p);
            settings->setBool  ("disk_writeback", c->isWriteBackEnabled());
        }
    }

    // Persist mounted 3.5" disks across restarts AND flush any firmware-
    // driven write-backs (format / save / etc.) that arrived after the
    // user opted in to write-back. Mirrors the Disk II save-on-shutdown
    // hook so changes survive a hard quit.
    for (pom2::Disk35Image* img :
            { &controller->disk35Internal(), &controller->disk35External() }) {
        if (img->isLoaded() && img->hasUnsavedChanges() &&
            !img->isWriteProtected()) {
            img->saveDirty();
        }
    }
    settings->setString("disk35_path_1",
        controller->disk35Internal().isLoaded()
            ? controller->disk35Internal().path() : std::string());
    settings->setString("disk35_path_2",
        controller->disk35External().isLoaded()
            ? controller->disk35External().path() : std::string());

    if (hdvCard) {
        settings->setBool("hdv_writeback", hdvCard->isWriteBackEnabled());
    }

    // CFFA per-slot image + write-back for EVERY plugged CFFA card. `cffa`
    // is multi-instance, so persist each (not just the primary `cffaCard`),
    // mirroring the DiskII loop above. (blockCards() also returns synthetic
    // HDV cards — those persist via hdv_path; skip them here.)
    for (auto* blk : blockCards()) {
        auto* cffa = dynamic_cast<pom2::CffaCard*>(blk);
        if (!cffa) continue;
        const std::string key = "cffa_slot" + std::to_string(cffa->getSlot());
        settings->setString(key + "_path",
                            cffa->isImageLoaded() ? cffa->getImagePath()
                                                  : std::string());
        settings->setBool(key + "_writeback", cffa->isWriteBackEnabled());
    }

    // Per-slot persistence so the //c's two SSC ports (printer sl1 +
    // modem sl2) each keep their own port / listener / raw-mode state.
    // Legacy global keys (`ssc_listening`, `ssc_port`, `ssc_raw_mode`)
    // are mirrored to the primary SSC for backwards-compat with older
    // settings files and the AI control path.
    for (auto* ssc : sscCards) {
        if (!ssc) continue;
        const std::string sk = "_slot" + std::to_string(ssc->getSlot());
        settings->setBool("ssc_listening" + sk, ssc->isListening());
        settings->setInt ("ssc_port"      + sk, ssc->getPort());
        settings->setBool("ssc_raw_mode"  + sk, ssc->rawMode());
    }
    if (sscCard) {
        settings->setBool("ssc_listening", sscCard->isListening());
        settings->setInt ("ssc_port",      sscCard->getPort());
        settings->setBool("ssc_raw_mode",  sscCard->rawMode());
    }

    // AI control listener — persist enable, port, token, and the panel
    // visibility flag. Re-armed on next launch by the constructor.
    settings->setBool  ("ai_control_enable", aiServer->isRunning());
    settings->setInt   ("ai_control_port",   aiServer->getPort());
    settings->setString("ai_control_token",  aiTokenInput);
    settings->setBool  ("show_ai_control",   showAiControlPanel);

    // Persist the per-slot card mapping so changes via the Slot
    // Configuration panel survive a restart. Slots the ACTIVE profile forces
    // (//c/+ on-board SSC/Mouse/SmartPort/Disk II, and the empty virtual slots
    // on a no-physical-slots model) are NOT persisted — `slotCards` holds the
    // forced built-in there, and writing it would clobber the user's real
    // choice (e.g. quitting on //c would overwrite slot_4_card=mockingboard
    // with the //c's on-board "mouseaw", losing it when they go back to //e).
    // The Le Chat Mauve rear-connector adapter IS user-controllable on //c, so
    // it persists. (Mirrors the "saved key left untouched" contract in
    // plugSlotsFromSettings.)
    {
        const auto& cfg = pom2::profileConfig(activeProfile);
        for (int s = 1; s <= 7; ++s) {
            if (s == autoProvisionedHdvSlot_) continue;   // session-local auto-plug
            if (s == autoProvisionedSmartPortSlot_) continue;   // idem (Floppy Emu)
            // Profile-forced slots (built-ins / noPhysicalSlots) hold the
            // profile's value, not the user's — shared guard with the Slot
            // Config Apply button (pom2::slotKeyIsUserChoice).
            const std::string key = "slot_" + std::to_string(s) + "_card";
            if (!pom2::slotKeyIsUserChoice(cfg, s, slotCards[s],
                                           settings->getString(key, "")))
                continue;
            settings->setString(key, slotCards[s]);
        }
    }

    auto modeName = [](Apple2Display::HiResMode m) -> const char* {
        switch (m) {
            case Apple2Display::HiResMode::ColorNTSC:        return "ColorNTSC";
            case Apple2Display::HiResMode::ColorCompMedium:  return "ColorCompMedium";
            case Apple2Display::HiResMode::ColorComp4Bit:    return "ColorComp4Bit";
            case Apple2Display::HiResMode::ChatMauveRGB:     return "ChatMauveRGB";
            case Apple2Display::HiResMode::ColorCompositeOE: return "ColorCompositeOE";
            case Apple2Display::HiResMode::ColorCompositeOECpu: return "ColorCompositeOECpu";
            case Apple2Display::HiResMode::MonoWhite:        return "MonoWhite";
            case Apple2Display::HiResMode::MonoGreen:        return "MonoGreen";
            case Apple2Display::HiResMode::MonoAmber:        return "MonoAmber";
            case Apple2Display::HiResMode::ColorAppleWin:    return "ColorAppleWin";
        }
        return "ColorNTSC";
    };
    settings->setString("hi_res_mode", modeName(display->getHiResMode()));
    {
        const char* sub = "monitor";
        switch (display->getAppleWinSubMode()) {
            case Apple2Display::AppleWinSubMode::Monitor:   sub = "monitor";   break;
            case Apple2Display::AppleWinSubMode::Tv:        sub = "tv";        break;
            case Apple2Display::AppleWinSubMode::Idealized: sub = "idealized"; break;
        }
        settings->setString("applewin_submode", sub);
    }
    settings->setBool  ("show_disk_panel", showDiskPanel);
    settings->setBool  ("show_disk35_panel", showDisk35Panel);
    settings->setBool  ("show_disk_library", showDiskLibrary);
    settings->setBool  ("show_hdv_panel",  showHdvPanel);
    settings->setBool  ("show_smartport_panel", showSmartPortPanel);
    settings->setBool  ("show_slot_config", showSlotConfigPanel);
    settings->setBool  ("show_floppy_emu", showFloppyEmu);
    settings->setString("floppyemu_mode",
                        pom2::FloppyEmuDevice::modeKey(floppyEmu->mode()));
    settings->setString("floppyemu_sd_root", floppyEmu->sdRoot());
    settings->setBool  ("show_cassette",   showCassetteDeck);
    settings->setBool  ("show_hgr_paint",  showHgrPaintEditor);
    settings->setBool  ("show_hgr_sprite", showHgrSpriteEditor);
    {
        const auto hs = hgrPaintEditor->session();
        settings->setInt   ("hgr_paint_mode",  hs.mode);
        settings->setBool  ("hgr_paint_page2", hs.page2);
        settings->setInt   ("hgr_paint_zoom",  hs.zoomIdx);
        settings->setBool  ("hgr_paint_ntsc",  hs.ntscColor);
        settings->setBool  ("hgr_paint_43",    hs.aspect43);
        settings->setInt   ("hgr_paint_pipe",  hs.canvasPipeline);
        settings->setString("hgr_paint_dir",   hs.browserDir);
    }
    settings->setBool  ("show_rewind",     showRewindBar);
    settings->setBool  ("rewind_enabled",  controller->rewind().enabled());
    settings->setBool  ("show_joystick",   showJoystickPanel);
    settings->setBool  ("show_mouse_inspector", showMouseInspector);
    settings->setBool  ("show_chatmauve",  showChatMauvePanel);
    settings->setBool  ("show_mockingboard", showMockingboardPanel);
    settings->setBool  ("show_phasor",       showPhasorPanel);
    settings->setBool  ("show_echoplus",     showEchoPlusPanel);
    settings->setBool  ("show_mixer",      showAudioMixer);
    settings->setBool  ("show_ssc",        showSscPanel);
    settings->setBool  ("show_printer",    showPrinterPanel);
    settings->setString("printer_email_to", printerEmailTo);
    settings->setBool  ("show_nsclock",    showNoSlotClockPanel);
    settings->setBool  ("nsclock_enable",  controller->noSlotClock().isEnabled());
    settings->setBool  ("show_ntsc",       showNtscSettings);
    if (ntscFx) {
        const auto& p = ntscFx->getParams();
        settings->setFloat("ntsc_brightness",  p.brightness);
        settings->setFloat("ntsc_contrast",    p.contrast);
        settings->setFloat("ntsc_saturation",  p.saturation);
        settings->setFloat("ntsc_hue",         p.hue);
        settings->setFloat("ntsc_sharpness",   p.sharpness);
        settings->setFloat("ntsc_persistence", p.persistence);
        settings->setFloat("ntsc_scanlines",   p.scanlines);
        settings->setFloat("ntsc_barrel",      p.barrel);
        settings->setFloat("ntsc_shadow_strength", p.shadowMaskStrength);
        settings->setFloat("ntsc_luminance_gain", p.luminanceGain);
        settings->setFloat("ntsc_center_lighting", p.centerLighting);
        settings->setFloat("ntsc_phosphor_gamma", p.phosphorGamma);
        settings->setInt  ("ntsc_shadow_mask", static_cast<int>(p.shadowMask));
        settings->setBool ("ntsc_pal",         p.palMode);
        settings->setBool ("ntsc_text_sharp",  p.textSharp);
    }
    settings->setBool  ("crt_effects_enabled", crtEffectsEnabled);
    settings->setBool  ("show_3d_voxel", show3dVoxel_);
    settings->setBool  ("show_voxel_settings", showVoxelSettings_);
    if (voxel3d_) {
        settings->setFloat("voxel_depth",       voxel3d_->voxelDepth);
        settings->setFloat("voxel_colorshift",  voxel3d_->colorShift);
        settings->setFloat("voxel_fill",        voxel3d_->cubeFill);
        settings->setFloat("voxel_ambient",     voxel3d_->ambient);
        settings->setInt  ("voxel_supersample", voxel3d_->superSample);
        settings->setBool ("voxel_mono",           voxel3d_->mono);
        settings->setBool ("voxel_percolor_depth", voxel3d_->perColorDepth);
    }
    settings->setString("aspect_mode",
        aspectMode == AspectMode::Crt43   ? "crt43" :
        aspectMode == AspectMode::Integer ? "integer" : "square");
    settings->setBool  ("disk_turbo",      diskTurboWhileMotor);
    settings->setFloat ("speaker_volume",  controller->speaker().getVolume());
    settings->setBool  ("speaker_muted",   controller->speaker().isMuted());
    settings->setFloat ("cassette_volume", controller->cassette().getVolume());
    settings->setBool  ("cassette_auto_rewind",
                        controller->cassette().isAutoRewindEnabled());
    settings->setFloat ("floppy_sound_volume",    controller->floppySound525().getVolume());
    settings->setBool  ("floppy_sound_muted",     controller->floppySound525().isMuted());
    settings->setFloat ("floppy_sound_volume_35", controller->floppySound35().getVolume());
    settings->setBool  ("floppy_sound_muted_35",  controller->floppySound35().isMuted());
    settings->setFloat ("master_volume",          controller->audio().getMasterVolume());
    settings->setBool  ("master_muted",           controller->audio().isMasterMuted());
    settings->setString("char_rom_locale",        pom2::charRomLocaleKey(charRomLocale));
    if (mockingboardCard) {
        settings->setFloat("mockingboard_volume", mockingboardCard->getVolume());
        settings->setBool ("mockingboard_muted",  mockingboardCard->isMuted());
    }

    // Kiosk is a read-only launcher: don't write state.cfg, so the disk it
    // booted (and any HDV card auto-plugged for it by ensureHdvCardForBoot)
    // never leak into the user's saved GUI config. The setString calls
    // above are in-memory only and discarded with `settings` here.
    if (!kiosk_) settings->save();

    if (aboutImageTex_) {
        GLuint t = aboutImageTex_;
        glDeleteTextures(1, &t);
        aboutImageTex_ = 0;
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
void MainWindow::plugSlotsFromSettings()
{
    namespace fs = std::filesystem;

    // Default mapping when no `slot_N_card` keys are present. Matches the
    // historical hard-wired layout from before the slot-config refactor.
    static const char* kDefaults[8] = {
        "",          // slot 0 (Language Card — owned by Memory, not us)
        "",          // slot 1
        "ssc",       // slot 2
        "",          // slot 3
        "clock",     // slot 4
        "hdv",       // slot 5
        "diskii",    // slot 6
        "chatmauve"  // slot 7
    };

    // Resolve each slot from settings, with the legacy `clock_card_enable`
    // flag as a one-shot fallback: if the user has clock_card_enable=false
    // AND no slot_4_card key, slot 4 stays empty. Once any slot_N_card key
    // is set in the file, that key is the source of truth.
    for (int s = 1; s <= 7; ++s) {
        const std::string key = "slot_" + std::to_string(s) + "_card";
        slotCards[s] = settings->getString(key, kDefaults[s]);
    }
    if (!settings->getBool("clock_card_enable", true)) {
        // Legacy opt-out: only respect when no explicit slot_N_card key
        // was set (settings->getString returned the default for slot 4).
        if (settings->getString("slot_4_card", "__missing__") == "__missing__"
            && slotCards[4] == "clock") {
            slotCards[4] = "";
        }
    }

    // Profile-defined built-in slots override anything the user persisted.
    // Real //c / //c+ have no physical expansion bus; their on-board cards
    // (serial, mouse, Disk II, SmartPort) live at fixed virtual slot IDs
    // and CANNOT be unplugged (D-6-1, E-6-1). Keep the settings file's
    // value out of harm's way so toggling profiles back and forth doesn't
    // produce a wedged machine.
    //
    // When the profile declares `noPhysicalSlots` (//c, //c+), the
    // remaining "free" slots (sl3 = AUX, sl7) are FORCED EMPTY too —
    // there's no physical connector on real //c hardware, so plugging
    // anything there is meaningless. The user's saved key is left
    // untouched in settings (so switching back to //e restores it) but
    // ignored for this run.
    {
        const auto& cfg = pom2::profileConfig(activeProfile);
        // Does the profile already carry a built-in Le Chat Mauve (//c PAL)?
        // If so, a user-configured chatmauve elsewhere must NOT plug a second
        // card — the rear connector is already taken by the on-board adapter.
        bool builtinRgb = false;
        for (int s = 1; s <= 7; ++s)
            if (cfg.builtInSlots[s].has_value() &&
                cfg.builtInSlots[s]->cardKey == "chatmauve")
                builtinRgb = true;
        for (int s = 1; s <= 7; ++s) {
            if (cfg.builtInSlots[s].has_value()) {
                const std::string& forced = cfg.builtInSlots[s]->cardKey;
                if (slotCards[s] != forced) {
                    pom2::log().info("Slots",
                        "Slot " + std::to_string(s) + " forced to '" +
                        forced + "' (built-in on " +
                        std::string(cfg.displayName) +
                        "); user setting '" + slotCards[s] + "' ignored");
                    slotCards[s] = forced;
                }
            } else if (cfg.noPhysicalSlots && !slotCards[s].empty()) {
                // Exception: the Le Chat Mauve RGB card is NOT a peripheral-
                // slot card — on a //c it ships as the "Adaptateur IIc" that
                // plugs into the rear DB-15 video-expansion connector, which
                // the //c/+ DOES have. So honour a user-configured `chatmauve`
                // even on a no-physical-slots model (the European //c is the
                // very machine that took this adapter — see § System profiles)
                // — unless the profile already wires one on-board (//c PAL).
                if (slotCards[s] == "chatmauve" && !builtinRgb) {
                    pom2::log().info("Slots",
                        "Slot " + std::to_string(s) + " = Le Chat Mauve RGB "
                        "(rear video-connector adapter) on " +
                        std::string(cfg.displayName));
                } else {
                    pom2::log().info("Slots",
                        "Slot " + std::to_string(s) + " left empty on " +
                        std::string(cfg.displayName) +
                        " (no physical slot connector on this model); "
                        "user setting '" + slotCards[s] + "' ignored");
                    slotCards[s] = "";
                }
            }
        }
    }

    // Validate uniqueness — each card type plugs into at most one slot.
    // Exceptions (multi-instance): "diskii" (option C 2026-05-15: //e configs
    // carried DiskII slot 6 + slot 4 for 4 drives), and — since the Slot
    // Manager can drive several of each — "cffa" and "smartport35", whose
    // persistence is already per-slot (cffa_slotN_*, smartport_slotN_*). The
    // rest stay single-instance because their driver / ROM signature / global
    // settings keys assume exclusivity ("hdv" uses the global hdv_path key).
    //
    // Built-in slots are ALWAYS exempt: when a profile forces the same card
    // type into multiple slots (e.g. //c has two SSC-compatible serial ports
    // at sl1+sl2), the uniqueness rule would otherwise eject the second one.
    // Profile authors are trusted to declare hardware that actually shipped.
    auto multiInstance = [](const std::string& k) -> bool {
        return k == "diskii" || k == "cffa" || k == "smartport35";
    };
    const auto& uniqCfg = pom2::profileConfig(activeProfile);
    auto firstOccurrence = [&](const std::string& type) -> int {
        for (int s = 1; s <= 7; ++s) if (slotCards[s] == type) return s;
        return -1;
    };
    for (int s = 1; s <= 7; ++s) {
        if (slotCards[s].empty())  continue;
        if (multiInstance(slotCards[s])) continue;    // multi-instance OK
        if (uniqCfg.builtInSlots[s].has_value())          // forced by profile
            continue;
        const int first = firstOccurrence(slotCards[s]);
        if (first != s) {
            pom2::log().warn("Slots",
                "Slot " + std::to_string(s) + " requested '" + slotCards[s] +
                "' but that card is already in slot " + std::to_string(first) +
                " — leaving slot " + std::to_string(s) + " empty");
            slotCards[s] = "";
        }
    }

    // ── Per-card construction helpers. Each one populates the matching
    //    raw `*Card` member pointer (non-owning) for the rest of MainWindow
    //    to find, and plugs the card into the SlotBus. ────────────────

    auto plugDiskII = [&](int s) {
        static const char* diskRomCandidates[] = {
            "roms/disk2.rom", "../roms/disk2.rom", "../../roms/disk2.rom"
        };
        for (const char* p : diskRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { diskRomPath = r; break; }
        }
        auto card = std::make_unique<DiskIICard>(s);
        // DiskIICard ctor pre-loads the embedded Apple 341-0027-A boot PROM,
        // so the card is bootable out of the box. A user-supplied dump at
        // `roms/disk2.rom` overrides only if it loads cleanly (256 bytes).
        if (fs::exists(diskRomPath) && card->loadBootRom(diskRomPath)) {
            diskRomStatus = std::string("loaded: ") + diskRomPath;
        } else {
            diskRomStatus = "embedded 341-0027-A PROM (no user disk2.rom)";
        }
        // Optional: load the P6 LSS sequencer PROM (Apple part 341-0028-A)
        // for the cycle-accurate bit-level controller path. Bundled at
        // `roms/diskii_p6.rom`; falls back to the embedded default.
        static const char* lssRomCandidates[] = {
            "roms/diskii_p6.rom", "../roms/diskii_p6.rom",
            "../../roms/diskii_p6.rom"
        };
        for (const char* p : lssRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { (void)card->loadLssRom(r); break; }
        }
        // Optional 13-sector PROMs (Apple 341-0009 boot + 341-0010 LSS) for
        // pre-DOS-3.3 disks. The card serves them only while a 13-sector
        // disk is mounted (see DiskIICard serving13_). Bundled at
        // roms/disk2_13.rom + roms/diskii_p6_13.rom.
        static const char* boot13Candidates[] = {
            "roms/disk2_13.rom", "../roms/disk2_13.rom", "../../roms/disk2_13.rom"
        };
        for (const char* p : boot13Candidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { (void)card->loadBootRom13(r); break; }
        }
        static const char* lss13Candidates[] = {
            "roms/diskii_p6_13.rom", "../roms/diskii_p6_13.rom",
            "../../roms/diskii_p6_13.rom"
        };
        for (const char* p : lss13Candidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { (void)card->loadLssRom13(r); break; }
        }
        // Wire the CPU pointer for sub-instruction cycle accuracy on
        // MMIO reads/writes (cycle-precise copy protections rely on the
        // LSS state at the exact sub-cycle of the data fetch, not at
        // instruction-start). See DiskIICard::setCpu doc for context.
        card->setCpu(&controller->cpu());
        card->setFloppySound(&controller->floppySound525());
        // //c+ on-board IWM — only the slot-6 card pushes its drive
        // pointer to the IWM, mirroring the //c+ wiring. Multi-instance
        // Disk II (option C) lets the user plug a second Disk II in
        // slot 4 etc.; that secondary card stays off the IWM path to
        // avoid clobbering the //c+ flux mirror.
        if (s == 6) card->setIWM(&controller->iwm());
        DiskIICard* raw = card.get();
        // First plugged DiskII is the "primary" — its panel & state
        // power the legacy menu paths (auto-turbo, AI control, top-
        // bar Eject). plugSlotsFromSettings iterates slots ascending,
        // so the lowest-numbered slot wins naturally.
        diskCards.push_back(raw);
        if (!diskCard) diskCard = raw;
        diskPanels.push_back(std::make_unique<pom2::DiskController_ImGui>());
        if (!diskPanel) diskPanel = diskPanels.front().get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugHdv = [&](int s) {
        // Restore ONLY the image explicitly mounted in the previous
        // session — mirrors the Disk II / 3.5" restore above (no folder
        // scan). This used to fall back to auto-mounting the first
        // .hdv/.2mg found under hdv/ when no path was saved, which
        // silently re-mounted (and auto-booted) a hard disk the user had
        // just ejected. An empty `hdv_path` now means "nothing mounted",
        // consistent with every other storage type.
        const std::string saved = settings->getString("hdv_path", "");
        std::error_code ec;
        if (!saved.empty() && fs::is_regular_file(saved, ec)) {
            hdvPath = saved;
        }

        auto card = std::make_unique<ProDOSHardDiskCard>(s);
        if (!hdvPath.empty() && card->loadImage(hdvPath)) {
            hdvStatus = std::string("loaded: ") + hdvPath;
        } else {
            hdvStatus = "no image mounted";
        }
        card->setWriteBackEnabled(settings->getBool("hdv_writeback", false));
        if (!hdvCard) hdvCard = card.get();   // primary = lowest slot
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugCffa = [&](int s) {
        // MAME-faithful CFFA 2.0: the real 4 KB firmware dump executed over an
        // emulated ATA chip (CffaCard → AtaBlockDevice → Block512Backing).
        // Pick the firmware variant matching the CPU (65C02 → eec02, else
        // ee02), falling back to whichever dump is present. No ROM → the slot
        // is left empty (the card type is also hidden in Slot Config then).
        // findResource adds the executable-relative / FHS roots on top of
        // the CWD + build/-relative ones, so the CFFA dumps resolve in a
        // portable bundle / AppImage / install too. See ResourcePaths.h.
        auto probe = [&](const char* a, const char* b) -> std::string {
            for (const char* nm : { a, b }) {
                std::string p = pom2::findResource(std::string("roms/") + nm);
                if (!p.empty()) return p;
            }
            return std::string();
        };
        const bool cmos =
            controller->cpu().getCpuMode() == M6502::CpuMode::CMOS;
        const std::string cffaRomPath = cmos
            ? probe("cffa20eec02.bin", "cffa20ee02.bin")
            : probe("cffa20ee02.bin", "cffa20eec02.bin");
        if (cffaRomPath.empty()) {
            pom2::log().warn("CFFA", "Slot " + std::to_string(s) +
                " requested CFFA but no firmware ROM (roms/cffa20ee02.bin) — "
                "leaving slot empty");
            slotCards[s] = "";
            return;
        }
        auto card = std::make_unique<pom2::CffaCard>(s);
        if (!card->loadRom(cffaRomPath)) { slotCards[s] = ""; return; }

        // Restore ONLY the explicitly-saved image (no folder scan — same
        // policy as plugHdv). Per-slot keys so a CFFA can live in any slot.
        const std::string key = "cffa_slot" + std::to_string(s);
        const std::string saved = settings->getString(key + "_path", "");
        std::error_code ec;
        if (!saved.empty() && fs::is_regular_file(saved, ec)) {
            if (!card->loadImage(saved))
                pom2::log().warn("CFFA", "Re-mount failed: " + card->getLastError());
        }
        card->setWriteBackEnabled(settings->getBool(key + "_writeback", false));
        if (!cffaCard) cffaCard = card.get();   // primary = lowest slot
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugChatMauve = [&](int s) {
        auto card = std::make_unique<LeChatMauveCard>(s);
        chatMauveCard = card.get();
        if (settings) {
            chatMauveCard->setInvertBit7(
                settings->getBool("chatmauve_invert_bit7", false));
            chatMauveCard->setColorTextEnabled(
                settings->getBool("chatmauve_color_text", true));
            chatMauveCard->setHgrDuochromeEnabled(
                settings->getBool("chatmauve_hgr_duochrome", false));
        }
        controller->memory().slotBus().plug(s, std::move(card));
        display->setChatMauveCard(chatMauveCard);
    };

    auto plugSsc = [&](int s) {
        auto card = std::make_unique<SuperSerialCard>(s);
        SuperSerialCard* raw = card.get();
        // Use pasteText (not queueKey) — pasteText respects the paste
        // queue, so a stream of bytes from telnet doesn't clobber earlier
        // characters that BASIC hasn't picked up yet.
        raw->setKeyboardSink(
            [&mem = controller->memory()](uint8_t b) {
                const char buf[1] = { static_cast<char>(b) };
                mem.pasteText(buf, 1);
            });
        // IRQ routing is auto-wired by SlotBus's installed router (see
        // Memory::setCpu) — no per-card setup needed.
        controller->memory().slotBus().plug(s, std::move(card));
        sscCards.push_back(raw);
        if (sscCard == nullptr) sscCard = raw;     // primary alias = lowest slot
        // Per-slot persistence; fall back to legacy global keys (the
        // primary SSC was the only one before //c dual-port support).
        const std::string sk = "_slot" + std::to_string(s);
        const bool legacyPrimary = (raw == sscCard);
        raw->setRawMode(settings->getBool(
            "ssc_raw_mode" + sk,
            legacyPrimary ? settings->getBool("ssc_raw_mode", false) : false));
        const bool listenDefault = legacyPrimary
            ? settings->getBool("ssc_listening", false) : false;
        if (settings->getBool("ssc_listening" + sk, listenDefault)) {
            const int portDefault = legacyPrimary
                ? settings->getInt("ssc_port", SuperSerialCard::kDefaultPort)
                : SuperSerialCard::kDefaultPort;
            const int p = settings->getInt("ssc_port" + sk, portDefault);
            raw->startListening(static_cast<uint16_t>(p));
        }
    };

    auto plugClock = [&](int s) {
        auto card = std::make_unique<ClockCard>(s);
        clockCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugSoftCard = [&](int s) {
        // Microsoft SoftCard (Z80 DMA bus master). No ROM to probe — the
        // hardware has none. The card needs the real bus (soft-switch
        // side effects, LC paging) and the 6502 so its $CnXX toggle can
        // halt the in-flight run() chunk at an instruction boundary.
        auto card = std::make_unique<SoftCardZ80>();
        card->setMemory(&controller->memory());
        card->setCpu(&controller->cpu());
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugPrinter = [&](int s) {
        auto card = std::make_unique<PrinterCard>(s);
        printerCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugPhasor = [&](int s) {
        // Applied Engineering Phasor. Same MMIO surface as a Mockingboard
        // plus a mode soft-switch at $C0(8+s)X that flips between MB-
        // compat (1 AY per VIA) and Phasor-native (2 AYs per VIA × 2 VIAs
        // = 4 chips, 12 voices). Audio synth is a v1 placeholder — the
        // card detects + responds to MMIO correctly but emits silence
        // until the 4-AY mix lands (TODO 🟡 [Phasor] audio synth).
        auto card = std::make_unique<PhasorCard>(s);
        card->setSampleRate(controller->audio().getActualSampleRate());
        card->setCpu(&controller->cpu());
        card->setVolume(settings->getFloat("phasor_volume", 0.5f));
        card->setMuted (settings->getBool ("phasor_muted",  false));
        if (controller->audio().isAvailable()) {
            controller->audio().addSource(card->audioSource());
        }
        phasorCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugEchoPlus = [&](int s) {
        // Street Electronics Echo+ — standalone SSI263 speech synth.
        // No PROM, no ROM dependency, audio is silent in v1 (chip
        // model complete but phoneme PCM blob deferred to a separate
        // commit pending license review of AppleWin's data).
        auto card = std::make_unique<EchoPlusCard>(s);
        card->setSampleRate(controller->audio().getActualSampleRate());
        card->setCpu(&controller->cpu());
        card->setVolume(settings->getFloat("echoplus_volume", 0.7f));
        card->setMuted (settings->getBool ("echoplus_muted",  false));
        if (controller->audio().isAvailable()) {
            controller->audio().addSource(card->audioSource());
        }
        echoPlusCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugEchoPlusTms = [&](int s) {
        // Street Electronics Echo+ AS ACTUALLY SHIPPED — 2×AY-3-8913 +
        // TMS5220. Scaffold only: register decode is present so software
        // detects the card, but the LPC core + AY synth are deferred.
        // Audio is silent. See EchoPlusTMS5220Card.h for the chipset
        // sourcing notes.
        auto card = std::make_unique<EchoPlusTMS5220Card>(s);
        echoPlusTmsCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugGrappler = [&](int s) {
        // Orange Micro Grappler+ — ROM-gated parallel printer. Loads
        // roms/grappler_plus.bin if present; falls back to a synthetic
        // stub ROM (PR#n trampoline only) so the card always plugs.
        auto card = std::make_unique<GrapplerCard>(s);
        static const char* kCandidates[] = {
            "roms/grappler_plus.bin",
            "roms/grappler+.bin",
            "roms/grappler.bin",
        };
        for (const char* c : kCandidates) {
            std::string r = pom2::findResource(c);
            if (!r.empty() && card->loadRom(r)) break;
        }
        if (!card->isRomLoaded()) {
            pom2::log().warn("Grappler",
                "Grappler+ plugged in slot " + std::to_string(s) +
                " without a 4 KB ROM dump (roms/grappler_plus.bin) — "
                "graphics dump commands unavailable, PR#n still works");
        }
        grapplerCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugMockingboard = [&](int s, MockingboardCard::Variant variant) {
        // Mockingboard A/C — 6522×2 + AY-3-8910×2. No ROM dependency, no
        // image to mount: software detects it by writing to the VIA at
        // $C(s)00 and observing the read-back. We always-plug when
        // requested. The inner AudioSource is registered with the audio
        // device so synthesised samples mix with the speaker output, and
        // the CPU IRQ line is wired so VIA T1 can drive the music
        // driver's tick.
        //
        // Variant::SoundII additionally adds an SSI263 speech synth at
        // $C(s)40-$C(s)44 with A/!R wired to VIA1.CA1 → IFR.CA1 →
        // (gated by IER.CA1) slot IRQ. Drivers configure PCR.0=0 for
        // negative-edge detection on the inverted A/!R wiring.
        auto card = std::make_unique<MockingboardCard>(s, variant);
        card->setSampleRate(controller->audio().getActualSampleRate());
        // CPU pointer feeds the lazy-sync timer back-channel
        // (getCycleCountNow); IRQ routing is auto-wired via SlotBus.
        card->setCpu(&controller->cpu());
        // Default volume is conservative — the card's three-channel mix
        // can dwarf the speaker at peak; the user can crank via the
        // Mockingboard panel (TODO).
        card->setVolume(settings->getFloat("mockingboard_volume", 0.5f));
        card->setMuted(settings->getBool ("mockingboard_muted",  false));
        if (controller->audio().isAvailable()) {
            controller->audio().addSource(card->audioSource());
        }
        mockingboardCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugSmartPort35 = [&](int s) {
        // Liron-class card. Each unit's type + image is restored from
        // settings (smartport_slotN_unitK_*) so per-card mixes (e.g.
        // unit 0 = 3.5", unit 1 = HDV) survive across launches. When
        // no setting exists, both units start empty — the user picks
        // a type via the SmartPort Configuration panel.
        auto card = std::make_unique<pom2::SmartPortCard>(s);
        // Mechanical sound: route to the dedicated 3.5" sound bank.
        // Block-level transfers only — the card synthesises step / motor
        // / click events from READBLOCK / WRITEBLOCK directly.
        card->setFloppySound(&controller->floppySound35());
        // Real Liron identity (roms/liron.rom, BMOW dump) on slot-having
        // machines only — NEVER on //c-class, whose on-board $C500 stub
        // must keep the synthetic $Cn07=$01 ProDOS-block identity (a
        // SmartPort-class byte re-triggers the boot-scan confusion, see
        // project_iic_smartport_boot).
        if (!pom2::profileConfig(activeProfile).noPhysicalSlots) {
            const std::string r = pom2::findResource("roms/liron.rom");
            if (!r.empty()) card->loadLironRom(r);
        }

        // Restore per-unit configuration from settings. Each unit row
        // remembers its kind ("35" / "hdv" / ""=empty), its mounted image
        // path, and the write-back toggle. Persistence keyspace:
        //   smartport_slotN_unitK_type     ("" / "35" / "hdv")
        //   smartport_slotN_unitK_path     (image path, optional)
        //   smartport_slotN_unitK_writeback (bool)
        const std::string slotKey = "smartport_slot" + std::to_string(s);
        for (size_t k = 0; k < pom2::SmartPortCard::kMaxUnits; ++k) {
            const std::string base = slotKey + "_unit" + std::to_string(k);
            const std::string kind = settings->getString(base + "_type", "");
            if (kind.empty()) continue;
            auto unit = pom2::makeSmartPortUnit(kind);
            if (!unit) {
                pom2::log().warn("SmartPort",
                    "Unknown unit kind '" + kind + "' for " + base +
                    " — leaving empty");
                continue;
            }
            unit->setWriteBackEnabled(
                settings->getBool(base + "_writeback", false));
            // Resolve the persisted path against multiple cwd anchors —
            // settings.ini may have been written when POM2 ran from
            // repo root (`hdv/X`) but a later launch from build/ would
            // fail `is_regular_file` on the bare relative path. Same
            // fallback the ROM probe uses.
            const std::string p = settings->getString(base + "_path", "");
            std::string resolved;
            if (!p.empty()) {
                std::error_code ec;
                for (const std::string& cand :
                     { p, std::string("../") + p, std::string("../../") + p }) {
                    if (std::filesystem::is_regular_file(cand, ec)) {
                        resolved = cand;
                        break;
                    }
                }
            }
            if (!resolved.empty()) {
                if (!unit->loadImage(resolved)) {
                    pom2::log().warn("SmartPort",
                        "Failed to remount " + resolved + " on " + base +
                        ": " + unit->lastError());
                }
            } else if (!p.empty()) {
                pom2::log().warn("SmartPort",
                    "Persisted path not found: " + p + " (on " + base + ")");
            }
            card->setUnit(k, std::move(unit));
        }
        if (!smartPortCard) smartPortCard = card.get();   // primary = lowest slot
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugMouse = [&](int s) {
        // Mouse Card (MAME-faithful 68705P3 + 6821 PIA + Apple ROMs).
        // Both Apple ROMs are required — without them the card has no
        // firmware and refuses to plug.
        std::string slotRomPath, mcuRomPath;
        static const char* slotRomCandidates[] = {
            "roms/mouse_341-0270-c.bin", "../roms/mouse_341-0270-c.bin",
            "../../roms/mouse_341-0270-c.bin"
        };
        static const char* mcuRomCandidates[] = {
            "roms/mouse_341-0269.bin", "../roms/mouse_341-0269.bin",
            "../../roms/mouse_341-0269.bin"
        };
        for (const char* p : slotRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { slotRomPath = r; break; }
        }
        for (const char* p : mcuRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { mcuRomPath = r; break; }
        }
        if (slotRomPath.empty() || mcuRomPath.empty()) {
            mouseRomStatus = "ROMs missing (need roms/mouse_341-0270-c.bin "
                             "and roms/mouse_341-0269.bin)";
            pom2::log().warn("Mouse",
                "Mouse Card requested in slot " + std::to_string(s) +
                " but " + mouseRomStatus + " — leaving slot empty");
            return;
        }
        auto card = std::make_unique<MouseCard>(s);
        if (!card->loadRoms(slotRomPath, mcuRomPath)) {
            mouseRomStatus = "ROM load failed (size mismatch?)";
            return;
        }
        // IRQ routing is auto-wired by SlotBus (see Memory::setCpu).
        // Mouse Card's MCU PB6 reaches the CPU via SlotPeripheral::
        // assertIrq, which fans out through M6502::setIrqLine(slot, …).
        mouseRomStatus = "loaded: " + slotRomPath + " + " + mcuRomPath;
        mouseCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    // Dispatch: walk slots 1..7 and plug whichever card the settings ask
    // for. Anything we don't recognise is logged and skipped.
    for (int s = 1; s <= 7; ++s) {
        const std::string& kind = slotCards[s];
        if      (kind.empty())          continue;
        else if (kind == "diskii")      plugDiskII(s);
        else if (kind == "hdv")         plugHdv(s);
        else if (kind == "cffa")        plugCffa(s);
        else if (kind == "ssc")         plugSsc(s);
        else if (kind == "printer")     plugPrinter(s);
        else if (kind == "clock")       plugClock(s);
        else if (kind == "softcard")    plugSoftCard(s);
        else if (kind == "chatmauve")   plugChatMauve(s);
        else if (kind == "mouse")       plugMouse(s);
        else if (kind == "mouseaw")     {
            // AppleWin HLE variant — only the slot EPROM is needed.
            std::string slotRomPath;
            static const char* slotRomCandidates[] = {
                "roms/mouse_341-0270-c.bin", "../roms/mouse_341-0270-c.bin",
                "../../roms/mouse_341-0270-c.bin"
            };
            for (const char* p : slotRomCandidates) {
                std::string r = pom2::findResource(p);
                if (!r.empty()) { slotRomPath = r; break; }
            }
            // Real //c always shipped with on-board mouse hardware, so on
            // //c-class profiles a missing AppleWin EPROM is a worse UX
            // than falling back to the MC68705 variant (which has its
            // own ROM probe). Hand off to plugMouse() in that case.
            if (slotRomPath.empty()) {
                pom2::log().warn("MouseAW",
                    "Mouse (AppleWin HLE) requested in slot " +
                    std::to_string(s) +
                    " but roms/mouse_341-0270-c.bin not found — "
                    "falling back to MC68705 \"mouse\" card");
                plugMouse(s);
                continue;
            }
            auto card = std::make_unique<MouseCardAppleWin>(s);
            if (!card->loadRom(slotRomPath)) {
                pom2::log().warn("MouseAW",
                    "ROM load failed for slot " + std::to_string(s) +
                    " — falling back to MC68705 \"mouse\" card");
                plugMouse(s);
                continue;
            }
            // MODE_INT_VBL pacing follows the machine's video standard
            // (17045 cycles ≈ 60 Hz NTSC, 20313 ≈ 50 Hz PAL) — NOT the
            // profile's defaultCyclesPerFrame, which on the //c+ carries
            // the 4× accelerator CPU budget while the video beam (and so
            // the VBL interrupt) still runs at 60 Hz.
            card->setVblCycles(pom2VideoTiming(
                pom2::profileConfig(activeProfile).videoStandard).cyclesPerFrame);
            mouseAwCard = card.get();
            controller->memory().slotBus().plug(s, std::move(card));
        }
        else if (kind == "mockingboard")   plugMockingboard(s, MockingboardCard::Variant::AC);
        else if (kind == "mockingboard_c") plugMockingboard(s, MockingboardCard::Variant::SoundII);
        else if (kind == "phasor")      plugPhasor(s);
        else if (kind == "echoplus")    plugEchoPlus(s);
        else if (kind == "echoplus_tms") plugEchoPlusTms(s);
        else if (kind == "grappler")    plugGrappler(s);
        else if (kind == "smartport35") plugSmartPort35(s);
        else {
            pom2::log().warn("Slots",
                "Slot " + std::to_string(s) + " has unknown card type '" +
                kind + "' — leaving empty");
            slotCards[s] = "";
        }
    }
}

// ─── Screenshot ───────────────────────────────────────────────────────────

void MainWindow::saveScreenshot()
{
    // Snapshot the framebuffer under stateMutex — the worker thread is
    // happily running but the renderer will never resize the buffer
    // mid-copy, so a brief lock is enough.
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        w = display->width();
        h = display->height();
        const uint32_t* src = display->pixels();
        pixels.assign(src, src + w * h);
    }

    // Pick the next unused screenshot_NNN.ppm in the current directory so
    // captures from successive F9 presses don't clobber each other.
    static int lastIdx = 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string path;
    while (true) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "screenshot_%03d.ppm", lastIdx);
        path = buf;
        if (!fs::exists(path, ec)) break;
        ++lastIdx;
        if (lastIdx > 999) { lastIdx = 0; break; }
    }

    // PPM "P6" — binary RGB, 1 row per scanline. Apple2Display's pixels
    // are 0xAABBGGRR (RGBA little-endian); strip alpha and swizzle to RGB.
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        tapeStatusMessage = "Screenshot: cannot write " + path;
        tapeStatusUntil   = lastFrameTime + 3.0;
        return;
    }
    f << "P6\n" << w << " " << h << "\n255\n";
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < pixels.size(); ++i) {
        const uint32_t p = pixels[i];
        rgb[i * 3 + 0] = static_cast<uint8_t>( p        & 0xFF);
        rgb[i * 3 + 1] = static_cast<uint8_t>((p >>  8) & 0xFF);
        rgb[i * 3 + 2] = static_cast<uint8_t>((p >> 16) & 0xFF);
    }
    f.write(reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));

    pom2::log().info("Screenshot", "wrote " + path +
                     " (" + std::to_string(w) + "x" + std::to_string(h) + ")");
    tapeStatusMessage = "Screenshot: " + path;
    tapeStatusUntil   = lastFrameTime + 3.0;
    ++lastIdx;
}

// ─── Keyboard ─────────────────────────────────────────────────────────────

void MainWindow::injectAscii(uint8_t apple2Code)
{
    controller->memory().queueKey(apple2Code);
}

void MainWindow::onChar(unsigned int codepoint)
{
    // While the kiosk menu is up, its keyboard fallbacks (K toggles the key
    // band, etc.) are read via ImGui::IsKeyPressed — the same keystroke must
    // not also land in the $C000 latch.
    if (kioskMenuOpen_) return;
    // In kiosk, K is reserved (Select fallback): the OPEN direction leaks
    // otherwise — this callback fires while the menu is still closed, then
    // updateKioskMenu opens the non-pausing Keys band the same frame, so the
    // running game would receive a live 'k' on every open.
    if (kiosk_ && (codepoint == 'k' || codepoint == 'K')) return;
    // Apple II accepts the full ASCII range (uppercase and lowercase). We
    // forward the codepoint as-is — Applesoft and the Monitor pick whichever
    // case the user typed.
    if (codepoint >= 0x20 && codepoint < 0x80) {
        injectAscii(static_cast<uint8_t>(codepoint));
    }
}

void MainWindow::onKey(int key, int /*scancode*/, int action, int mods)
{
    // Open-Apple / Solid-Apple are read by the IIe/IIc/IIc+ firmware via
    // $C061/$C062 bit 7 (MAME `apple2e.cpp:2157-2169`) — the firmware itself
    // decides cold-reboot vs self-test on Ctrl+Reset. We just source the
    // bits; observe both press and release so the firmware sees the key
    // released after Reset like on real hardware.
    if (key == GLFW_KEY_LEFT_ALT) {
        controller->memory().setOpenAppleKey(action != GLFW_RELEASE);
        return;
    }
    if (key == GLFW_KEY_RIGHT_ALT) {
        controller->memory().setSolidAppleKey(action != GLFW_RELEASE);
        return;
    }

    // Kiosk menu open: its arrows/Enter/Esc fallbacks are polled with
    // ImGui::IsKeyPressed and the menu window never captures the keyboard,
    // so everything below would double-deliver — Enter on the key band
    // would send the cell AND inject $0D, Esc would close the menu AND
    // type $1B into the game on resume.
    if (kioskMenuOpen_) return;
    // K reserved in kiosk (see onChar) — also blocks Ctrl-K's $0B, since
    // eSelect fires on the K key regardless of modifiers.
    if (kiosk_ && key == GLFW_KEY_K) return;

    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    // Ctrl-V intercepts the host shortcut: paste system clipboard into
    // the Apple II keyboard buffer rather than injecting raw $16. The
    // Apple II's own Ctrl-V (rarely used) can still be reached via the
    // Edit menu or via Ctrl-Shift-V if a future version chooses to map it.
    if (ctrl && key == GLFW_KEY_V) {
        pasteFromClipboard();
        return;
    }

    switch (key) {
        case GLFW_KEY_ENTER:        // fallthrough — main + numpad Enter both
        case GLFW_KEY_KP_ENTER:     injectAscii(0x0D); break;
        case GLFW_KEY_BACKSPACE:    injectAscii(0x08); break;
        case GLFW_KEY_LEFT:         injectAscii(0x08); break;
        case GLFW_KEY_RIGHT:        injectAscii(0x15); break;
        case GLFW_KEY_UP:           injectAscii(0x0B); break;
        case GLFW_KEY_DOWN:         injectAscii(0x0A); break;
        case GLFW_KEY_ESCAPE:       injectAscii(0x1B); break;
        case GLFW_KEY_TAB:          injectAscii(0x09); break;
        case GLFW_KEY_F9:           saveScreenshot(); break;
        case GLFW_KEY_F11:          controller->softReset(); break;
        case GLFW_KEY_F12:          controller->hardReset(); break;
        default:
            // Ctrl-A..Ctrl-Z generate $01..$1A — these matter for Applesoft
            // (Ctrl-C breaks out of a running program, Ctrl-G beeps, etc.)
            if (ctrl && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
                injectAscii(static_cast<uint8_t>(key - GLFW_KEY_A + 1));
            }
            break;
    }
}

// ─── Paste ───────────────────────────────────────────────────────────────

void MainWindow::pasteFromClipboard()
{
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) {
        tapeStatusMessage = "Paste: clipboard is empty";
        tapeStatusUntil   = lastFrameTime + 3.0;
        return;
    }
    std::string text = clip;
    if (pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller->memory().pasteText(text);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars queued from clipboard", queued);
    tapeStatusMessage = buf;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

void MainWindow::pasteFromFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) {
        tapeStatusMessage = "Paste: cannot open " + path;
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    if (pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller->memory().pasteText(text);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars from %s", queued, path.c_str());
    tapeStatusMessage = buf;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

// ─── Texture upload ──────────────────────────────────────────────────────

void MainWindow::uploadScreenTexture()
{
    if (screenTexture == 0) {
        glGenTextures(1, &screenTexture);
        glBindTexture(GL_TEXTURE_2D, screenTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Initial allocation matches the 280-wide buffer; the real
        // dimensions are set after the first display->render() below.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     Apple2Display::kWidth, Apple2Display::kHeight,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        screenTextureWidth  = Apple2Display::kWidth;
        screenTextureHeight = Apple2Display::kHeight;
    }

    // Mirror the live demod knobs into the display BEFORE render() so the
    // OE-CPU demod (and the OE-GPU mixed-frame CPU demod band) track the
    // CRT-Settings sliders exactly like the GPU shader — hue / sharpness /
    // PAL / textSharp used to be GPU-only and silently dead on the CPU path.
    {
        Apple2Display::OeDemodParams dp;
        if (ntscFx) {
            const pom2::NtscParams& np = ntscFx->getParams();
            dp.hue       = np.hue;
            dp.sharpness = np.sharpness;
            dp.palMode   = np.palMode;
            dp.textSharp = np.textSharp;
        }
        display->setOeDemodParams(dp);
    }

    {
        // Render under stateMutex so we get a consistent snapshot of RAM
        // (otherwise the CPU may be mid-frame with the text screen half
        // updated, producing tearing).
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        display->render(controller->memory());
    }
    // OE-CPU demod runs OUTSIDE the lock — it reads only display-owned
    // buffers and took ~1-2 ms of CPU-worker stall per UI frame inside it.
    display->finishPendingCpuDemod();

    const int w = display->width();
    const int h = display->height();
    glBindTexture(GL_TEXTURE_2D, screenTexture);
    if (w != screenTextureWidth || h != screenTextureHeight) {
        // 80-col toggled — reallocate. glTexImage2D releases the previous
        // storage, so we don't leak GL memory across mode switches.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, display->pixels());
        screenTextureWidth  = w;
        screenTextureHeight = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, display->pixels());
    }
}

// ─── Render passes ───────────────────────────────────────────────────────

void MainWindow::renderMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        ImGui::MenuItem("Disk Library (all formats)", nullptr, &showDiskLibrary);
        ImGui::Separator();
        // Disk II (slot 6) — frequent action, lifted out of the old
        // Hardware kitchen-sink. Panel still exposes its own insert/eject
        // buttons; this is the keyboard-friendly path.
        ImGui::BeginDisabled(diskCard == nullptr);
        if (ImGui::MenuItem("Insert disk image (.dsk / .do / .po / .nib / .woz)...")) {
            diskPanel->insertDialogOpen = true;
            if (diskPanel->dialogPath.empty()) diskPanel->dialogPath = "disks_5.4/";
        }
        if (ImGui::MenuItem("Eject disk", nullptr, false,
                            diskCard && diskCard->isDiskLoaded())) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            diskCard->ejectDisk();
            tapeStatusMessage = "Disk ejected";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(hdvCard == nullptr);
        if (ImGui::MenuItem("Mount HDV image (.hdv / .2mg)...")) {
            hdvPanel->mountDialogOpen = true;
            if (hdvPanel->dialogPath.empty()) hdvPanel->dialogPath = "hdv/";
        }
        if (ImGui::MenuItem("Eject HDV", nullptr, false,
                            hdvCard && hdvCard->isImageLoaded())) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            hdvCard->ejectImage();
            hdvStatus = "no image mounted";
            tapeStatusMessage = "HDV ejected";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!hdvCard || !hdvCard->isImageLoaded());
        // Label reflects where the user actually has the card plugged.
        const std::string bootHdvLabel = "Boot HDV (slot " +
            std::to_string(hdvCard ? hdvCard->getSlot() : 5) + ")";
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
            {
                // Must hold the emulation lock: loadAppleIIRom rewrites
                // $D000-$FFFF and can race with the CPU thread otherwise.
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = controller->memory().loadAppleIIRom(romPath.c_str());
                if (!ok) err = controller->memory().getLastError();
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
        const auto curCpu = controller->cpu().getCpuMode();
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
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                controller->cpu().setCpuMode(cfg.defaultCpu);
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
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                controller->cpu().setCpuMode(M6502::CpuMode::NMOS);
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("65C02 (CMOS)", nullptr,
                                curOverride == "65c02" ||
                                (curOverride == "auto" && curCpu == M6502::CpuMode::CMOS
                                 && curOverride != "nmos"))) {
                settings->setString("cpu_mode_override", "65c02");
                settings->save();
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                controller->cpu().setCpuMode(M6502::CpuMode::CMOS);
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
        ImGui::MenuItem("Slot Configuration...", nullptr, &showSlotConfigPanel);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Devices")) {
        // One flat 17-item list became hard to scan (audit 2026-05-31), so
        // it is grouped under SeparatorText headers and every row carries a
        // hover tooltip explaining what the panel does. `devItem` keeps the
        // boilerplate (optional grey-out + tooltip) in one place; disabled
        // rows still show their tip via AllowWhenDisabled so the user learns
        // what a card *would* do before plugging it in Slot Config.
        auto devItem = [](const char* label, bool* flag, const char* tip,
                          bool enabled = true) {
            if (!enabled) ImGui::BeginDisabled();
            ImGui::MenuItem(label, nullptr, flag);
            if (!enabled) ImGui::EndDisabled();
            if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tip);
        };

        ImGui::SeparatorText("Storage");
        devItem("Floppy Emu (BMOW)", &showFloppyEmu,
                "BMOW Floppy Emu: SD-card image browser + OLED, emulated.");
        devItem("Cassette deck", &showCassetteDeck,
                "Load/save tape images (.wav) on II/II+/IIe.");
        devItem("Disk II (slot 6)", &showDiskPanel,
                "5.25\" drive panel: insert / eject / write-protect, drive LEDs.");
        {
            // Mirror the panel's dynamic title — slot N on //e with a
            // Liron-class card, "//c+ on-board" on //c+.
            std::string lbl = smartPortCard
                ? "Disk 3.5\" (slot " + std::to_string(smartPortCard->getSlot()) + ")"
                : std::string("Disk 3.5\" (//c+ on-board)");
            devItem(lbl.c_str(), &showDisk35Panel,
                    "800K 3.5\" drive (SmartPort / //c+ on-board IWM).");
        }
        {
            const std::string lbl = "HDV (slot " +
                std::to_string(hdvCard ? hdvCard->getSlot() : 5) + ")";
            devItem(lbl.c_str(), &showHdvPanel,
                    "ProDOS hard-disk image (.hdv/.2mg): mount / eject / boot.");
        }
        {
            const std::string lbl = smartPortCard
                ? "SmartPort Configuration (slot " +
                      std::to_string(smartPortCard->getSlot()) + ")"
                : std::string("SmartPort Configuration (no card plugged)");
            devItem(lbl.c_str(), &showSmartPortPanel,
                    "SmartPort units behind a Liron-class card (3.5\" + HDV volumes).",
                    smartPortCard != nullptr);
        }

        ImGui::SeparatorText("Sound");
        devItem("Mockingboard (VIA + AY state)", &showMockingboardPanel,
                "Mockingboard A/C: live 6522 VIA + AY-3-8910 PSG register view.");
        {
            const std::string lbl = phasorCard
                ? "Phasor (slot " + std::to_string(phasorCard->getSlot()) + ")"
                : std::string("Phasor (no card plugged)");
            devItem(lbl.c_str(), &showPhasorPanel,
                    "Applied Engineering Phasor: 2× VIA, 4× AY, mode soft-switch.",
                    phasorCard != nullptr);
        }
        {
            const std::string lbl = echoPlusCard
                ? "Echo+ (slot " + std::to_string(echoPlusCard->getSlot()) + ")"
                : std::string("Echo+ (no card plugged)");
            devItem(lbl.c_str(), &showEchoPlusPanel,
                    "Echo/Cricket SSI263 speech chip state.",
                    echoPlusCard != nullptr);
        }
        devItem("Audio Mixer", &showAudioMixer,
                "Per-source volume: speaker, Mockingboard/Phasor, speech, floppy.");

        ImGui::SeparatorText("Ports & cards");
        // Super Serial — //c ships TWO (printer + modem), other profiles
        // have at most one. Label shows actual slot(s) so the user knows
        // which entry opens which port.
        {
            std::string lbl;
            if (sscCards.empty()) {
                lbl = "Super Serial (no card plugged)";
            } else if (sscCards.size() == 1) {
                lbl = "Super Serial (slot " +
                      std::to_string(sscCards[0]->getSlot()) + ")";
            } else {
                lbl = "Super Serial (slots";
                for (size_t i = 0; i < sscCards.size(); ++i) {
                    lbl += (i == 0) ? " " : ", ";
                    lbl += std::to_string(sscCards[i]->getSlot());
                }
                lbl += ")";
            }
            devItem(lbl.c_str(), &showSscPanel,
                    "6551 ACIA serial port + telnet bridge (modem / printer).",
                    !sscCards.empty());
        }
        {
            const std::string lbl = printerCard
                ? "Printer (slot " + std::to_string(printerCard->getSlot()) + ")"
                : std::string("Printer (no card plugged)");
            devItem(lbl.c_str(), &showPrinterPanel,
                    "Parallel printer card → text spool (.txt or e-mail).",
                    printerCard != nullptr);
        }
        devItem("Le Chat Mauve (slot 7)", &showChatMauvePanel,
                "Le Chat Mauve RGB / Eve video card controls.");
        devItem("Joystick", &showJoystickPanel,
                "Analog paddles / joystick mapping + push-buttons.");

        ImGui::SeparatorText("Inspectors & tools");
        ImGui::MenuItem("Rewind (time-travel)", "F6", &showRewindBar);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scrub back through machine state. Hold F6 to rewind live.");
        devItem("Mouse Inspector", &showMouseInspector,
                "Apple II Mouse Card state + host-cursor sync diagnostics.");
        devItem("No-Slot Clock (DS1216E)", &showNoSlotClockPanel,
                "Dallas DS1216E real-time clock hidden under the Monitor ROM.");
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
        ImGui::MenuItem("CRT Settings (sliders)...", nullptr, &showNtscSettings);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scanlines, shadow mask, barrel, phosphor curve,\n"
                              "persistence, brightness/contrast/saturation.");

        // 3D voxel view (MicroM8 "Voxel Cube"): rebuild the screen as an
        // upright 4:3 slab of equal-depth cubes; left-drag orbits, middle-drag
        // pans, wheel zooms. Works on any colour mode.
        ImGui::MenuItem("3D voxel view", nullptr, &show3dVoxel_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("MicroM8-style cube renderer.\n"
                              "Left-drag orbits, middle-drag pans, wheel zooms.");
        ImGui::MenuItem("3D voxel settings...", nullptr, &showVoxelSettings_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Depth, colour pop, fill, anti-alias, mono, per-colour depth.");

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
        ImGui::BeginDisabled(chatMauveCard == nullptr);
        pipeItem("RGB card — Le Chat Mauve", nullptr,
                 Apple2Display::HiResMode::ChatMauveRGB);
        ImGui::EndDisabled();

        pipeItem("Monochrome — White",      nullptr, Apple2Display::HiResMode::MonoWhite);
        pipeItem("Monochrome — Green (P31)", nullptr, Apple2Display::HiResMode::MonoGreen);
        pipeItem("Monochrome — Amber",      nullptr, Apple2Display::HiResMode::MonoAmber);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Memory viewer",               nullptr, &showMemViewer);
        ImGui::Separator();
        ImGui::MenuItem("Memory Map Bar",              nullptr, &showMemoryBar);
        ImGui::MenuItem("Memory Map Bar (Horizontal)", nullptr, &showMemoryBarH);
        ImGui::MenuItem("Memory Map Grid",             nullptr, &showMemoryGrid);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        ImGui::MenuItem("HGR Paint Editor", nullptr, &showHgrPaintEditor);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Paint directly into HGR/GR/DHGR video RAM through "
                              "the real NTSC pipeline (image import included).");
        ImGui::MenuItem("HGR Sprite Editor", nullptr, &showHgrSpriteEditor);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw HGR sprites on a scratch page, grab from / "
                              "stamp to the live screen, export ca65 .byte tables.");
#ifndef __EMSCRIPTEN__
        // The AI Control HTTP server is compiled out under WASM
        // (AiControlServer::start() returns false — no listening socket in
        // the browser sandbox), so its entry is hidden there.
        ImGui::MenuItem("AI Control (HTTP)...", nullptr, &showAiControlPanel);
#endif
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Welcome / Quick Start", nullptr, &showWelcomePanel);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Where to put ROMs/disks, keys, and signature features.");
        ImGui::Separator();
        if (ImGui::MenuItem("About POM2")) showAbout = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

// Bottom-of-viewport status bar. Carries the machine/mode/graphics/ROM
// summary that used to be crammed into the right edge of the menu bar.
void MainWindow::renderStatusBar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##StatusBar", vp, ImGuiDir_Down,
                                    height, flags)) {
        if (ImGui::BeginMenuBar()) {
            const char* modeStr = "?";
            switch (controller->getMode()) {
                case EmulationController::Mode::Running: modeStr = "RUN";  break;
                case EmulationController::Mode::Stopped: modeStr = "STOP"; break;
                case EmulationController::Mode::Step:    modeStr = "STEP"; break;
            }
            const auto state = controller->memory().getDisplayState();
            const char* gfx = state.textMode ? "TEXT"
                            : state.hiRes    ? (state.mixedMode ? "HGR+TXT" : "HGR")
                                             : (state.mixedMode ? "LGR+TXT" : "LGR");
            const auto& cfg = pom2::profileConfig(activeProfile);
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%.*s | %s | %s",
                          static_cast<int>(cfg.displayName.size()), cfg.displayName.data(),
                          modeStr, gfx);
            ImGui::TextDisabled("%s", buf);

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
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s",
                                   tapeStatusMessage.c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void MainWindow::renderScreenWindow()
{
    // Default startup layout. Native keeps room for the Disk Library column;
    // WASM starts with only menu + toolbar + Apple II Screen + status bar.
    // `FirstUseEver` only applies on a fresh install — once the user
    // moves / resizes the window their imgui.ini takes over.
#ifdef __EMSCRIPTEN__
    ImGui::SetNextWindowPos (ImVec2(5,    56),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1115, 760), ImGuiCond_FirstUseEver);
#else
    ImGui::SetNextWindowPos (ImVec2(5,    90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1115, 745), ImGuiCond_FirstUseEver);
#endif

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    // `NoMove` so ImGui's default "drag-from-anywhere" doesn't eat
    // click-and-drag gestures inside the screen (Mouse Card games like
    // A2Desktop need them to reach the guest). `NoCollapse` so a
    // double-click on the title bar doesn't accidentally collapse the
    // window — a single concern at a time. We restore the title-bar
    // drag manually below.
    const ImGuiWindowFlags screenFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Apple II Screen", nullptr, screenFlags)) {
        // ── Manual title-bar drag ─────────────────────────────────────
        // Compute the title bar rect from the public Begin geometry
        // (window pos + size + frame height). When the user clicks
        // inside that strip we latch `screenDraggingByTitleBar`; on
        // every subsequent frame we apply `io.MouseDelta` until the
        // button is released. `IsAnyItemActive()` guards against
        // claiming a click that another widget already consumed (e.g.
        // any future title-bar button).
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            const float  th = ImGui::GetFrameHeight();
            const ImVec2 m  = ImGui::GetIO().MousePos;
            const bool overTitleBar =
                (m.x >= wp.x && m.x <= wp.x + ws.x &&
                 m.y >= wp.y && m.y <= wp.y + th);
            // Do not start dragging when another foreground window covers
            // this title-bar rectangle. ImGui's normal move handling already
            // gives that front window priority; the Apple II Screen's manual
            // title drag must obey the same z-order rule.
            const bool screenWindowHovered = ImGui::IsWindowHovered();
            if (screenWindowHovered && overTitleBar &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::IsAnyItemActive()) {
                screenDraggingByTitleBar = true;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                screenDraggingByTitleBar = false;
            }
            if (screenDraggingByTitleBar) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                if (d.x != 0.0f || d.y != 0.0f) {
                    ImGui::SetWindowPos(ImVec2(wp.x + d.x, wp.y + d.y));
                }
            }
        }

        drawScreenImage();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void MainWindow::drawScreenImage()
{
    uploadScreenTexture();

    // OpenEmulator-style composite path: when the user selected
    // ColorCompositeOE AND Apple2Display produced a 14.318 MHz signal
    // this frame, lazily spin up the NTSC shader and run a single pass
    // that consumes the signal texture and writes an RGBA output we
    // hand to ImGui::Image. The first call compiles the shader; if
    // anything fails (driver lacking GL 3.x, shader compile error, …)
    // we fall back to the regular `screenTexture` for the rest of the
    // session — no crashes, no flicker, just the existing LUT view.
    unsigned int presentTex = screenTexture;
    // The 3D voxel view must sample the decoded COLOUR image, NEVER the CRT
    // glass — scanlines / shadow-mask / barrel warp would bake into the cube
    // grid. Track that tap point separately: it follows the colour pipeline
    // (NTSC demod, OE or otherwise) but stops *before* CrtEffectStack. For all
    // non-OE-GPU modes the colour image already lives in `screenTexture`; the
    // OE-GPU branch below redirects it to the demod output.
    unsigned int voxelSrcTex = screenTexture;
    // Sharp-text override: when the user wants legible text under the
    // composite mode, skip the shader for TEXT scanlines and let the
    // crisp RGB framebuffer go straight to ImGui. Full-screen text uses
    // textSharp; mixed HGR/lo-res/DHGR keeps the demod on the graphics
    // band only — the bottom 4 text rows are patched in frame80 as
    // white/black after demod (mixedCompositeUsesFramebuffer).
    // Use the soft-switch snapshot the render() above actually consumed —
    // re-polling Memory::getDisplayState() here raced the CPU worker (it may
    // have advanced past the rendered frame between the two), flashing one
    // LUT-fallback frame on a text↔graphics switch.
    const auto displayState = display->lastRenderState();
    const bool oeGpuMode = display->getHiResMode()
                         == Apple2Display::HiResMode::ColorCompositeOE;
    const bool oeCpuMode = display->getHiResMode()
                         == Apple2Display::HiResMode::ColorCompositeOECpu;
    const bool oeMode    = oeGpuMode;
    const bool oeFamily  = oeGpuMode || oeCpuMode;
    const bool wantSharpText = ntscFx && ntscFx->getParams().textSharp
                            && displayState.textMode;
    const bool mixedFbPresent = display->mixedCompositeUsesFramebuffer();

    // Compute the on-screen target size up-front so the CRT effect pass can
    // render at native output resolution. That is what lets the scanline /
    // shadow-mask patterns be sampled finely enough to analytically
    // anti-alias (no barrel-warp moiré) and lets ImGui blit the result 1:1
    // (no second resample beat). Same three aspect modes used for the final
    // blit below, which reuses `avail` / `size`.
    const float W = static_cast<float>(Apple2Display::kWidth);
    const float H = static_cast<float>(Apple2Display::kHeight);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 size;
    switch (aspectMode) {
        case AspectMode::Crt43: {
            float h = std::min(avail.y, avail.x * 3.0f / 4.0f);
            h = std::max(h, H);
            size = ImVec2(h * 4.0f / 3.0f, h);
            break;
        }
        case AspectMode::Integer: {
            float s = std::max(1.0f, std::floor(std::min(avail.x / W, avail.y / H)));
            size = ImVec2(W * s, H * s);
            break;
        }
        case AspectMode::Square:
        default: {
            float s = std::max(1.0f, std::min(avail.x / W, avail.y / H));
            size = ImVec2(W * s, H * s);
            break;
        }
    }
    const int dstW = std::max(1, static_cast<int>(size.x + 0.5f));
    const int dstH = std::max(1, static_cast<int>(size.y + 0.5f));

    if (oeMode && display->signalProduced() && !wantSharpText && !mixedFbPresent) {
        if (!ntscFx) ntscFx = std::make_unique<pom2::NtscPostProcessor>();
        if (!ntscFx->available() && !ntscFx->initialize()) {
            // initialize() already logged the failure. Stop trying so
            // we don't spam the log every frame.
        }
        if (ntscFx->available()) {
            // Phase 4 final: NtscPostProcessor is now demod-only (colour
            // recovery). The CRT glass (scanlines / mask / barrel /
            // persistence / BCS) lives in the shared CrtEffectStack, so OE
            // chains into it just like every other mode — one effects
            // implementation. Run the stack ALWAYS for OE (not gated by the
            // opt-in toggle) so the look the user configured is preserved.
            const unsigned int demod = ntscFx->process(
                display->signal(),
                display->signalWidth(),
                display->signalHeight(),
                display->signalPhaseOffset());
            if (demod != 0) {
                presentTex = demod;
                voxelSrcTex = demod;   // colour image, pre-CRT-glass (3D tap)
                // CRT glass only when the master toggle is on (top of the CRT
                // Settings window); otherwise present the raw demod output.
                if (crtEffectsEnabled) {
                    if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
                    if (!crtFx->available()) crtFx->initialize();
                    if (crtFx->available()) {
                        // The OE demod shader already applied hue + chroma-
                        // bandwidth sharpness; zero hue and neutralise sharpness
                        // here so the shared stack doesn't rotate the chroma or
                        // sharpen a second time (0.5 = the stack's neutral pass).
                        pom2::NtscParams crtP = ntscFx->getParams();
                        crtP.hue       = 0.0f;
                        crtP.sharpness = 0.5f;
                        crtFx->setParams(crtP);
                        const unsigned int out = crtFx->process(
                            demod, ntscFx->outputWidth(), ntscFx->outputHeight(),
                            dstW, dstH);
                        if (out != 0) presentTex = out;
                    }
                }
            }
        }
    }

    // OE CPU demod writes frame80 → screenTexture; the OE-GPU fallbacks
    // (mixed graphics+text frame, sharp text, shader unavailable, demod
    // failure) also still present screenTexture at this point. Give ALL of
    // them the same CRT glass — gating on oeCpuMode alone made scanlines /
    // mask / persistence vanish on exactly the mixed frames (score bands,
    // menus, BASIC) and pop back on full-screen graphics, with a stale-
    // persistence ghost on re-entry. `presentTex == screenTexture` is
    // precisely "no OE branch above produced a processed texture".
    if (oeFamily && crtEffectsEnabled && presentTex == screenTexture) {
        if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
        if (!crtFx->available()) crtFx->initialize();
        if (crtFx->available()) {
            // Neutralise the demod-stage knobs, same as the GPU branch above:
            // the OE-CPU demod (and the mixed-frame CPU demod band) now
            // applies hue + chroma-bandwidth sharpness itself via
            // setOeDemodParams, so the stack must not rotate the chroma or
            // sharpen a second time. (The only frames reaching here without
            // a demod are crisp B/W text — hue is a no-op on grays — and the
            // shader-unavailable LUT fallback, a documented degraded path.)
            pom2::NtscParams crtP = ntscFx ? ntscFx->getParams() : pom2::NtscParams{};
            crtP.hue       = 0.0f;
            crtP.sharpness = 0.5f;
            crtFx->setParams(crtP);
            const unsigned int out = crtFx->process(
                presentTex, display->width(), display->height(), dstW, dstH);
            if (out != 0) presentTex = out;
        }
    }

    // Universal CRT effect stack (Phase 3): for every NON-OE colour mode,
    // run the framebuffer through the shared scanline / mask / barrel /
    // persistence / BCS pass so those effects work on Color NTSC, Mono,
    // Chat Mauve and AppleWin too. OE GPU and OE CPU share one CRT branch
    // (demod hue/sharpness applied by their demod stage — neutralised there).
    if (!oeFamily && crtEffectsEnabled) {
        if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
        if (!crtFx->available()) crtFx->initialize();
        if (crtFx->available()) {
            // One CRT Settings panel drives both processors: mirror the
            // NtscParams the user edits there (the demod-only knobs are
            // ignored by the effect stack).
            if (ntscFx) crtFx->setParams(ntscFx->getParams());
            const unsigned int out = crtFx->process(
                presentTex, display->width(), display->height(), dstW, dstH);
            if (out != 0) presentTex = out;
        }
    }

    // 3D voxel view: rebuild the decoded COLOUR image (`voxelSrcTex`, the tap
    // taken before CrtEffectStack — so the cubes never inherit scanlines / mask
    // / barrel) as an upright 4:3 slab of equal-depth cubes (MicroM8 "Voxel
    // Cube"), viewed by the orbit camera. Renders at the on-screen size so the
    // aspect is exact; replaces the flat blit (CRT glass computed above is
    // discarded when the 3D view wins). Falls back to the flat texture if the
    // GL renderer can't initialise.
    if (show3dVoxel_) {
        if (!voxel3d_) voxel3d_ = std::make_unique<pom2::Voxel3DRenderer>();
        // One voxel per live Apple II pixel (280 or 560 × 192) so the cube grid
        // captures the full image — half-res sampling visibly lost detail.
        voxel3d_->gridW = std::max(1, display->width());
        voxel3d_->gridH = std::max(1, display->height());
#if defined(__EMSCRIPTEN__)
        // Perf guard: halve the 560-wide DHGR/80-col geometry on the browser so
        // the cube count stays near the comfortable HGR 280×192 (~54k); the FBO
        // supersample is likewise capped in Voxel3DRenderer::process.
        if (voxel3d_->gridW > 280) voxel3d_->gridW = 280;
#endif
        const int vw = std::max(16, static_cast<int>(size.x));
        const int vh = std::max(16, static_cast<int>(size.y));
        const float aspect = static_cast<float>(vw) / static_cast<float>(vh);
        const unsigned int out =
            voxel3d_->process(voxelSrcTex, vw, vh, voxelCam_.viewProj(aspect));
        if (out != 0) presentTex = out;
    }

    // Scale to the content region, then centre (letterbox on a wider/taller
    // region, e.g. a kiosk viewport). `avail` / `size` were computed up-front
    // (above) so the CRT effect pass could render at this exact resolution.
    // The 280×192 active area drove a 4:3 CRT, so its pixels are not square —
    // three presentation modes:
    //   Square  — 1:1 logical pixels (280:192 ≈ 1.46); crisp; never < 1×.
    //   Crt43   — stretch the active area to a true 4:3 frame (real-monitor
    //             shape); fills the region, letterboxed.
    //   Integer — Square snapped to an integer multiple (no fractional-scale
    //             shimmer); never < 1×.
    ImVec2 cur = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(
        cur.x + std::max(0.0f, (avail.x - size.x) * 0.5f),
        cur.y + std::max(0.0f, (avail.y - size.y) * 0.5f)));

    ImGui::Image(static_cast<ImTextureID>(presentTex), size);
    // Capture the screen widget's screen-space rect so the GLFW
    // cursor-pos callback (Phase 5) can route motion over the
    // screen to the Mouse Card.
    screenRectMin = ImGui::GetItemRectMin();
    screenRectMax = ImGui::GetItemRectMax();

    // 3D voxel view camera: left-drag orbits, middle-drag strafes (pan),
    // wheel zooms (MicroM8-style). All reference the Image item above
    // (IsItemHovered), so this must stay right after it. Mutates the
    // persistent `voxelCam_` the renderer reads.
    if (show3dVoxel_ && ImGui::IsItemHovered()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            voxelCam_.azimuth   += d.x * 0.008f;
            voxelCam_.elevation += d.y * 0.008f;
            const float lim = 1.5f;   // ~86°: stay off the lookAt up-vector poles
            voxelCam_.elevation = std::clamp(voxelCam_.elevation, -lim, lim);
        }
        // Middle-drag = pan/strafe. Scale to world-units-per-pixel at the
        // target plane so the grab tracks the cursor 1:1, and grab the scene
        // (drag right → scene follows right → camera slides left).
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            const float k = 2.0f * voxelCam_.distance *
                            std::tan(voxelCam_.fovY * 0.5f) /
                            std::max(1.0f, size.y);
            voxelCam_.pan(-d.x * k, d.y * k);
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            voxelCam_.distance =
                std::clamp(voxelCam_.distance * std::pow(0.9f, wheel), 0.6f, 20.0f);
    }
}

void MainWindow::renderKiosk()
{
    // Chrome-free full-viewport window: just the Apple II screen, centred
    // and letterboxed on a black background. No title bar, no resize, no
    // background decoration — the OS window is already full-screen.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("##kiosk", nullptr, flags)) {
        drawScreenImage();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ─── Kiosk disk selector (gamepad-driven) ───────────────────────────────
//
// A keyboard-free way to flip disks in kiosk mode: Start opens a list of the
// 5.25" images sitting in the same folder as the booted disk (so a game's
// "Side B" is one press away), D-pad/stick move, A mounts the highlighted one
// into the boot Disk II drive (slot 6, drive 1) without rebooting — the
// flip-disk gesture Wings of Fury and friends expect — and B/Start dismiss.

DiskIICard* MainWindow::kioskBootDiskCard()
{
    // Prefer the conventional boot slot 6; fall back to the primary card.
    for (auto* c : diskCards) if (c && c->getSlot() == 6) return c;
    return diskCard;
}

void MainWindow::openKioskStartMenu()
{
    kioskMenuOpen_ = true;
    kioskPage_     = KioskPage::List;
    kioskZone_     = KioskZone::Games;
    kioskActSel_   = 0;
    kioskRescanDisks();
}

// Rebuild the GAMES list from the booted disk's folder + the extra ROM
// folders. Split from openKioskStartMenu so the RomDirs page can refresh the
// list on its way back — otherwise a folder added/removed there is invisible
// until the menu is closed and reopened.
void MainWindow::kioskRescanDisks()
{
    kioskDiskPaths_.clear();
    kioskDiskLabels_.clear();
    kioskDiskSel_ = 0;
    kioskStatus_.clear();

    namespace fs = std::filesystem;
    std::error_code ec;

    DiskIICard* boot = kioskBootDiskCard();
    const std::string cur = boot ? boot->getDiskPath(0) : std::string{};

    // Scan the booted disk's own folder PLUS every configured extra ROM
    // folder. Unlike the old build we do NOT filter out unrelated titles:
    // we keep every mountable 5.25" image and SORT by name-proximity so the
    // current title's other sides float to the top while
    // the rest of the collection stays reachable below.
    std::vector<fs::path> dirs;
    auto addDir = [&](const fs::path& d) {
        if (d.empty() || !fs::is_directory(d, ec)) return;
        const fs::path norm = fs::weakly_canonical(d, ec);
        const fs::path key   = ec ? d : norm;
        for (const auto& e : dirs) if (e == key) return;   // dedup
        dirs.push_back(key);
    };
    if (!cur.empty()) addDir(fs::path(cur).parent_path());
    for (const auto& d : kioskRomDirs_) addDir(fs::path(d));

    if (dirs.empty()) {
        kioskStatus_ = boot ? "No disk folder to browse — add one via ROM folders"
                            : "No Disk II card in this config";
        return;
    }

    auto toLower = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return s;
    };
    auto commonPrefix = [](const std::string& a, const std::string& b) {
        const size_t n = std::min(a.size(), b.size());
        size_t i = 0;
        while (i < n && a[i] == b[i]) ++i;
        return i;
    };
    // A candidate is a "sibling" of the mounted disk when its stem shares a
    // long common prefix (≥6 chars, ≥half the shorter stem) — the same title's
    // other sides ("… (Side A)" ↔ "… (Side B)"), not every disk in the folder.
    const std::string curName = cur.empty() ? std::string{}
                                            : fs::path(cur).filename().string();
    const std::string curKey  = cur.empty() ? std::string{}
                                            : toLower(fs::path(cur).stem().string());
    auto isSibling = [&](const fs::path& p) {
        if (curKey.empty()) return false;
        const std::string k = toLower(p.stem().string());
        if (k == curKey) return true;
        const size_t pref   = commonPrefix(curKey, k);
        const size_t minLen = std::min(curKey.size(), k.size());
        return pref >= 6 && pref * 2 >= minLen;
    };

    // Accept every image the launcher can route — 5.25", 800K 3.5" and HDV —
    // not just floppies. A 5.25" disk is hot-swapped in place (flip-disk); a
    // 3.5"/HDV is mounted + booted through insertAndBootImage on activation.
    std::vector<fs::path> found;
    for (const auto& dir : dirs) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (classifyDiskForSlot(it->path().string()) == DiskSlotClass::Unknown)
                continue;
            found.push_back(it->path());
        }
    }
    // Sort: siblings of the mounted disk first, then alphabetical by filename.
    std::sort(found.begin(), found.end(), [&](const fs::path& a, const fs::path& b) {
        const bool sa = isSibling(a), sb = isSibling(b);
        if (sa != sb) return sa;
        return a.filename().string() < b.filename().string();
    });

    // Mark the disk currently in the boot drive so the list shows a ● next to
    // it and the cursor lands on it. Match canonically: the mounted path may
    // be relative (kiosk launched as `POM2 games/foo.dsk`) while scanned
    // entries come out of canonicalized dirs.
    std::error_code ecCur;
    const fs::path curCanon = cur.empty()
        ? fs::path{} : fs::weakly_canonical(fs::path(cur), ecCur);
    kioskMountedPath_ = cur;
    for (const auto& p : found) {
        const std::string name = p.filename().string();
        const bool isMounted = !cur.empty() &&
            (p.string() == cur ||
             (!ecCur && !curCanon.empty() && p == curCanon));
        if (isMounted) {
            kioskDiskSel_     = int(kioskDiskPaths_.size());
            // Adopt the scanned spelling so the render loop's exact string
            // compare against kioskDiskPaths_ draws the ● marker.
            kioskMountedPath_ = p.string();
        }
        kioskDiskPaths_.push_back(p.string());
        kioskDiskLabels_.push_back(name);
    }

    if (kioskDiskPaths_.empty())
        kioskStatus_ = "No disks found in the scanned folder(s)";

    pom2::log().info("Kiosk", "disk scan: " +
                     std::to_string(kioskDiskPaths_.size()) + " disk(s) across " +
                     std::to_string(dirs.size()) + " folder(s)");
}

void MainWindow::kioskMountSelected()
{
    if (kioskDiskSel_ < 0 || kioskDiskSel_ >= int(kioskDiskPaths_.size())) return;

    const std::string path  = kioskDiskPaths_[kioskDiskSel_];
    const std::string label = kioskDiskLabels_[kioskDiskSel_];

    // 3.5" and HDV volumes are boot media, not swap-in-place floppies: route
    // them into the right card and cold-boot straight away (like the CLI
    // launcher). 5.25" keeps the flip-disk gesture: hot-swap, no reboot.
    if (classifyDiskForSlot(path) != DiskSlotClass::Floppy525) {
        kioskSetPaused(false);          // let the worker run for the boot
        std::string err;
        if (insertAndBootImage(path, err)) {
            kioskMountedPath_ = path;
            kioskMenuOpen_ = false;     // booted → back to the game
        } else {
            kioskStatus_ = "Boot failed: " + err;
        }
        return;
    }

    DiskIICard* boot = kioskBootDiskCard();
    if (!boot) { kioskStatus_ = "No Disk II card in this config"; return; }

    bool ok = false;
    {
        // Same lock the CPU worker takes around softSwitchAccess: insertDisk
        // rebuilds the drive's track buffers, so it must not race the LSS.
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        ok = boot->insertDisk(0, path);   // in-place swap, no cold boot
    }
    if (ok) {
        // Keep the menu open so the user can chain a Restart (reboot on the
        // just-mounted disk) without reopening; B / Start dismisses it.
        kioskMountedPath_ = path;
        kioskStatus_ = "Mounted " + label + " — pick RESTART to reboot on it";
    } else {
        kioskStatus_ = "Mount failed: " + boot->getLastError(0);
    }
}

void MainWindow::kioskActivateFocused()
{
    // GAMES zone → mount the highlighted disk in place (no reboot).
    if (kioskZone_ == KioskZone::Games) {
        kioskMountSelected();
        return;
    }

    // ACTIONS zone → 0 Restart · 1 Keyboard · 2 ROM folders · 3 Quit.
    switch (kioskActSel_) {
        case 0: {   // Restart — reboot on whatever disk is now in the drive
            DiskIICard* boot = kioskBootDiskCard();
            kioskSetPaused(false);          // let the worker run for the boot
            if (boot) controller->bootFromSlot(boot->getSlot());
            else      controller->coldBoot();
            controller->setMode(EmulationController::Mode::Running);
            kioskMenuOpen_ = false;
            break;
        }
        case 1:     // Keyboard band — live keys to the running game
            kioskPage_   = KioskPage::Keys;
            kioskKeySel_ = 0;
            kioskSetPaused(false);          // game keeps running under the band
            break;
        case 2:     // ROM folders manager
            if (kioskPruneRomDirs()) kioskSaveRomDirs();
            kioskRomDirSel_ = 0;
            kioskPage_      = KioskPage::RomDirs;
            break;
        case 3:     // Quit — ask for confirmation first
            kioskPage_ = KioskPage::Quit;
            break;
    }
}

// Apple II key grid for the SELECT band. Each cell is an ASCII code the
// running program reads straight from the keyboard latch (queueKey), so no
// make/break bookkeeping is needed (unlike a scancode make/break scheme). Arrows use
// the II's control codes (←=$08 →=$15 ↑=$0B ↓=$0A).
namespace {
struct KioskKey { const char* label; uint8_t ascii; };
const KioskKey kKioskKeys[] = {
    {"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},{"5",'5'},
    {"6",'6'},{"7",'7'},{"8",'8'},{"9",'9'},{"0",'0'},          // row 0 (10)
    {"Q",'Q'},{"W",'W'},{"E",'E'},{"R",'R'},{"T",'T'},
    {"Y",'Y'},{"U",'U'},{"I",'I'},{"O",'O'},{"P",'P'},          // row 1 (10)
    {"A",'A'},{"S",'S'},{"D",'D'},{"F",'F'},{"G",'G'},
    {"H",'H'},{"J",'J'},{"K",'K'},{"L",'L'},                    // row 2 (9)
    {"Z",'Z'},{"X",'X'},{"C",'C'},{"V",'V'},{"B",'B'},
    {"N",'N'},{"M",'M'},                                        // row 3 (7)
    {"SPACE",' '},{"RET",0x0D},{"ESC",0x1B},
    {"\xe2\x86\x90",0x08},{"\xe2\x86\x91",0x0B},
    {"\xe2\x86\x93",0x0A},{"\xe2\x86\x92",0x15},                // row 4 (7)
};
constexpr int kKioskKeyCount = int(sizeof(kKioskKeys) / sizeof(kKioskKeys[0]));
// Row start/end (half-open) indices — mirrors the layout above.
const int kKioskKeyRows[][2] = { {0,10}, {10,20}, {20,29}, {29,36}, {36,43} };
constexpr int kKioskKeyRowN = int(sizeof(kKioskKeyRows) / sizeof(kKioskKeyRows[0]));
}  // namespace

void MainWindow::kioskInjectSelectedKey()
{
    if (kioskKeySel_ < 0 || kioskKeySel_ >= kKioskKeyCount) return;
    // The band leaves the machine running, so the latch is read live by the
    // program. queueKey masks to 7 bits and sets the strobe like the hardware.
    controller->memory().queueKey(kKioskKeys[kioskKeySel_].ascii);
    kioskStatus_ = std::string("Sent ") + kKioskKeys[kioskKeySel_].label;
}

void MainWindow::kioskSetPaused(bool want)
{
    if (want == kioskPausedByMenu_) return;
    if (!want) {
        // Resuming: while Stopped, the audio thread kept advancing the
        // speaker's reconstruction cursor over silence, so it now sits far
        // ahead of the (frozen) production. Flush it — otherwise the catch-up
        // logic would swallow the game's first sounds for ~the pause duration.
        controller->speaker().reset();
    }
    controller->setMode(want ? EmulationController::Mode::Stopped
                             : EmulationController::Mode::Running);
    kioskPausedByMenu_ = want;
}

void MainWindow::updateKioskMenu()
{
    // Load the persisted extra ROM folders once (feeds the disk scan below).
    if (!kioskRomDirsLoaded_) { kioskLoadRomDirs(); kioskRomDirsLoaded_ = true; }

    // The pad was already polled this frame (pollJoystickAndPushToMemory).
    const JoystickInput::UiNav nav = joystick->uiNav();

    // Keyboard fallbacks work even when the controller isn't a recognized
    // GLFW gamepad (so the gamepad-mapped buttons never fire). They mirror the
    // F10/Start open the Start menu, K/Select the keyboard band, arrows move,
    // Enter validates, Esc goes back.
    const bool eStart   = nav.menu    || ImGui::IsKeyPressed(ImGuiKey_F10,    false);
    const bool eSelect  = nav.select  || ImGui::IsKeyPressed(ImGuiKey_K,      false);
    const bool eConfirm = nav.confirm || ImGui::IsKeyPressed(ImGuiKey_Enter,  false);
    const bool eCancel  = nav.cancel  || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    // Left/right zone-swap is a one-shot edge (never auto-repeats).
    const bool eLeft    = nav.left    || ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false);
    const bool eRight   = nav.right   || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);

    // ── SELECT: open/close the keyboard band directly, even mid-game ─────
    // (Back/Select toggles the live keyboard without pausing).
    if (eSelect) {
        if (kioskMenuOpen_ && kioskPage_ == KioskPage::Keys) {
            kioskMenuOpen_ = false;
        } else {
            kioskMenuOpen_ = true;
            kioskPage_     = KioskPage::Keys;
            kioskKeySel_   = 0;
            kioskStatus_.clear();
        }
    }

    // ── START: open/close the Start menu ────────────────────────────────
    if (eStart) {
        if (kioskMenuOpen_ && kioskPage_ != KioskPage::Keys) kioskMenuOpen_ = false;
        else openKioskStartMenu();
    }

    // Pause the machine on every Start-menu page, but NOT the keyboard band
    // (its keys must reach a running game). Closed menu → running.
    const bool wantPause = kioskMenuOpen_ && kioskPage_ != KioskPage::Keys;
    kioskSetPaused(wantPause);
    // Re-park if something else resumed the worker behind the open menu
    // (e.g. an F6 hold released across the menu-open frame ends in
    // rewindEndAndResume → Mode::Running); kioskSetPaused alone early-outs
    // because kioskPausedByMenu_ still says "paused".
    if (wantPause && controller->getMode() != EmulationController::Mode::Stopped)
        controller->setMode(EmulationController::Mode::Stopped);

    if (!kioskMenuOpen_) return;

    // ── Temporal auto-repeat for held directions (400ms delay, 150ms rate) ──
    // The paused loop runs unthrottled, so a per-frame step would be
    // unaimable; gate held-direction steps on the wall clock instead. Edge
    // presses from the keyboard already single-step via IsKeyPressed below,
    // so `step` covers the *held* pad/keys case only.
    const bool upHeld   = nav.upHeld   || ImGui::IsKeyDown(ImGuiKey_UpArrow);
    const bool downHeld = nav.downHeld || ImGui::IsKeyDown(ImGuiKey_DownArrow);
    const bool leftHeld = nav.leftHeld || ImGui::IsKeyDown(ImGuiKey_LeftArrow);
    const bool rightHeld= nav.rightHeld|| ImGui::IsKeyDown(ImGuiKey_RightArrow);
    const bool pgUpHeld = nav.pageUpHeld  || ImGui::IsKeyDown(ImGuiKey_PageUp);
    const bool pgDnHeld = nav.pageDownHeld|| ImGui::IsKeyDown(ImGuiKey_PageDown);
    const bool navHeld  = upHeld || downHeld || leftHeld || rightHeld ||
                          pgUpHeld || pgDnHeld;
    const double tNow = ImGui::GetTime();
    bool step = false;
    if (navHeld) {
        if (!kioskNavHeld_) { step = true; kioskNavHeld_ = true; kioskNavNextT_ = tNow + 0.40; }
        else if (tNow >= kioskNavNextT_) { step = true; kioskNavNextT_ = tNow + 0.15; }
    } else {
        kioskNavHeld_ = false;
    }
    // Resolve directional intents: a fresh keyboard edge OR a repeat `step`.
    const bool up    = ImGui::IsKeyPressed(ImGuiKey_UpArrow,   false) || (step && upHeld);
    const bool down  = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || (step && downHeld);
    const bool left  = step && leftHeld;   // (edge handled separately below)
    const bool right = step && rightHeld;
    const bool pgUp  = ImGui::IsKeyPressed(ImGuiKey_PageUp,   false) || (step && pgUpHeld);
    const bool pgDn  = ImGui::IsKeyPressed(ImGuiKey_PageDown, false) || (step && pgDnHeld);

    switch (kioskPage_) {

    case KioskPage::Quit:
        if (eConfirm) { if (window) glfwSetWindowShouldClose(window, 1); kioskMenuOpen_ = false; }
        if (eCancel)  kioskPage_ = KioskPage::List;
        break;

    case KioskPage::Keys: {
        // 2D grid: up/down change row (clamp column), left/right within row.
        if (up || down) {
            int row = 0;
            for (int r = 0; r < kKioskKeyRowN; ++r)
                if (kioskKeySel_ >= kKioskKeyRows[r][0] && kioskKeySel_ < kKioskKeyRows[r][1]) row = r;
            int col = kioskKeySel_ - kKioskKeyRows[row][0];
            row = (row + (down ? 1 : -1) + kKioskKeyRowN) % kKioskKeyRowN;
            const int len = kKioskKeyRows[row][1] - kKioskKeyRows[row][0];
            if (col >= len) col = len - 1;
            kioskKeySel_ = kKioskKeyRows[row][0] + col;
        }
        if (left || right || eLeft || eRight) {
            int row = 0;
            for (int r = 0; r < kKioskKeyRowN; ++r)
                if (kioskKeySel_ >= kKioskKeyRows[r][0] && kioskKeySel_ < kKioskKeyRows[r][1]) row = r;
            const int len = kKioskKeyRows[row][1] - kKioskKeyRows[row][0];
            int col = kioskKeySel_ - kKioskKeyRows[row][0] + ((right || eRight) ? 1 : -1);
            col = std::max(0, std::min(len - 1, col));
            kioskKeySel_ = kKioskKeyRows[row][0] + col;
        }
        if (eConfirm) kioskInjectSelectedKey();   // send key, band stays open
        if (eCancel)  kioskMenuOpen_ = false;      // (B) close → resume game
        break;
    }

    case KioskPage::RomDirs: {
        const int total = 1 + int(kioskRomDirs_.size());   // [0]=ADD, [1..]=folders
        if (up || down) {
            kioskRomDirSel_ += down ? 1 : -1;
            kioskRomDirSel_ = (kioskRomDirSel_ % total + total) % total;
        }
        if (eConfirm) {
            if (kioskRomDirSel_ == 0) {                     // + ADD → browser
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path start = (!kioskRomDirs_.empty() &&
                                  fs::is_directory(kioskRomDirs_.back(), ec))
                                     ? fs::path(kioskRomDirs_.back())
                                     : fs::current_path(ec);
                fs::path abs = fs::absolute(start, ec);
                kioskBrowseDir_ = (ec ? start : abs).lexically_normal().string();
                kioskComputeShortcuts();
                kioskScanBrowse(kioskBrowseDir_);
                kioskPage_ = KioskPage::Browse;
            } else {                                        // remove this folder
                const int idx = kioskRomDirSel_ - 1;
                if (idx >= 0 && idx < int(kioskRomDirs_.size()))
                    kioskRomDirs_.erase(kioskRomDirs_.begin() + idx);
                kioskSaveRomDirs();
                if (kioskRomDirSel_ > int(kioskRomDirs_.size()))
                    kioskRomDirSel_ = int(kioskRomDirs_.size());
            }
        }
        if (eCancel) {
            kioskPage_ = KioskPage::List;
            kioskZone_ = KioskZone::Actions;
            kioskRescanDisks();   // pick up folders added/removed just now
        }
        break;
    }

    case KioskPage::Browse: {
        namespace fs = std::filesystem;
        const int nShort = int(kioskBrowseShortcutPaths_.size());
        const int total  = 2 + nShort + int(kioskBrowseSubdirs_.size());
        if (up || down) {
            kioskBrowseSel_ += down ? 1 : -1;
            kioskBrowseSel_ = (kioskBrowseSel_ % total + total) % total;
        }
        if (eConfirm) {
            if (kioskBrowseSel_ == 0) {                     // USE THIS FOLDER
                if (std::find(kioskRomDirs_.begin(), kioskRomDirs_.end(), kioskBrowseDir_)
                        == kioskRomDirs_.end())
                    kioskRomDirs_.push_back(kioskBrowseDir_);
                kioskSaveRomDirs();
                kioskRomDirSel_ = 0;
                kioskPage_ = KioskPage::RomDirs;
            } else if (kioskBrowseSel_ == 1) {              // .. parent
                const fs::path p(kioskBrowseDir_);
                if (p.has_parent_path() && p.parent_path() != p)
                    kioskBrowseDir_ = p.parent_path().string();
                kioskScanBrowse(kioskBrowseDir_);
                kioskBrowseSel_ = 0;
            } else if (kioskBrowseSel_ < 2 + nShort) {      // shortcut
                kioskBrowseDir_ = kioskBrowseShortcutPaths_[kioskBrowseSel_ - 2];
                kioskScanBrowse(kioskBrowseDir_);
                kioskBrowseSel_ = 0;
            } else {                                        // descend
                kioskBrowseDir_ = kioskBrowseSubdirs_[kioskBrowseSel_ - 2 - nShort];
                kioskScanBrowse(kioskBrowseDir_);
                kioskBrowseSel_ = 0;
            }
        }
        if (eCancel) kioskPage_ = KioskPage::RomDirs;
        break;
    }

    case KioskPage::List: default: {
        const int nd = int(kioskDiskPaths_.size());
        // LEFT/RIGHT (edge) swaps focus between the GAMES and ACTIONS zones.
        if (eLeft || eRight)
            kioskZone_ = (kioskZone_ == KioskZone::Games) ? KioskZone::Actions
                                                          : KioskZone::Games;
        if (kioskZone_ == KioskZone::Games) {
            if (nd > 0) {
                constexpr int kPage = 10;   // L1/R1 fast jump size
                int delta = 0;
                if      (down) delta =  1;
                else if (up)   delta = -1;
                else if (pgDn) delta =  kPage;
                else if (pgUp) delta = -kPage;
                if (delta != 0) {
                    kioskDiskSel_ += delta;
                    if (delta == 1 || delta == -1)          // step: wrap
                        kioskDiskSel_ = (kioskDiskSel_ % nd + nd) % nd;
                    else                                    // page jump: clamp
                        kioskDiskSel_ = std::max(0, std::min(nd - 1, kioskDiskSel_));
                }
            }
        } else if (up || down) {                            // ACTIONS column
            kioskActSel_ += down ? 1 : -1;
            kioskActSel_ = (kioskActSel_ % kKioskActionCount + kKioskActionCount)
                           % kKioskActionCount;
        }
        if (eConfirm) kioskActivateFocused();
        if (eCancel)  kioskMenuOpen_ = false;               // (B) resume game
        break;
    }
    }

    // A close on any page must let the machine run again.
    if (!kioskMenuOpen_) kioskSetPaused(false);
}

void MainWindow::renderKioskMenu()
{
    if (!kioskMenuOpen_) return;
    namespace fs = std::filesystem;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 disp = vp->Size;
    const bool keysPage = (kioskPage_ == KioskPage::Keys);

    const ImVec4 kYellow(1.0f, 0.85f, 0.30f, 1.0f);
    const ImVec4 kGreen (0.47f, 0.90f, 0.47f, 1.0f);
    const ImVec4 kDim   (0.50f, 0.50f, 0.50f, 1.0f);
    const ImVec4 kGrey  (0.60f, 0.60f, 0.60f, 1.0f);

    // Full-screen dim veil behind every page EXCEPT the keyboard band (there
    // the game must stay visible so you see keys land).
    if (!keysPage) {
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(disp);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##kioskVeil", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::End();
    }

    // Geometry: centered panel for full-screen pages, bottom band for keys.
    if (keysPage) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + disp.x * 0.5f, vp->Pos.y + disp.y * 0.98f),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(disp.x * 0.66f, disp.y * 0.34f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.72f);
    } else {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + disp.x * 0.5f, vp->Pos.y + disp.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(disp.x * 0.82f, disp.y * 0.80f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 12, 18, 245));
    }
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(220, 200, 80, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    // The kiosk screen is a full-viewport OPAQUE window — force the menu to
    // the front every frame or it renders hidden behind the black background.
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNavInputs;

    ImGui::Begin("##kioskMenu", nullptr, flags);

    auto rowText = [&](bool sel, const ImVec4& col, const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        char buf[512]; std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s %s", sel ? "\xe2\x96\xb6" : "  ", buf);
        if (sel) { ImGui::PopStyleColor(); ImGui::SetScrollHereY(0.5f); }
    };

    // ================= LIST — two zones (games / actions) ================
    if (kioskPage_ == KioskPage::List) {
        const int nd = int(kioskDiskLabels_.size());
        const bool zGames = (kioskZone_ == KioskZone::Games);

        ImGui::SetWindowFontScale(3.0f);
        ImGui::TextColored(kYellow, ICON_FA_GAMEPAD " MENU");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "\xe2\x97\x80\xe2\x96\xb6 switch zone   \xc2\xb7   "
                                  "up/down select   \xc2\xb7   L1/R1 fast   \xc2\xb7   "
                                  "A confirm   \xc2\xb7   B resume   \xc2\xb7   SELECT keyboard");
        ImGui::Separator();

        // Footer reserve = the 4 action rows @2.3 + a "disks found" line @1.3.
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float bf = ImGui::GetFontSize();
        const float footer = 4.0f * (bf * 2.3f + sp) + (bf * 1.3f + sp) + (sp + 6.0f);

        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextColored(zGames ? kYellow : kDim,
                           zGames ? "\xe2\x96\xb6 " ICON_FA_COMPACT_DISC " GAMES"
                                  : "  " ICON_FA_COMPACT_DISC " GAMES");
        ImGui::BeginChild("##kdList", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), true);
        ImGui::SetWindowFontScale(2.6f);
        if (nd == 0)
            ImGui::TextColored(kDim, "(no disks — add a ROM folder)");
        for (int i = 0; i < nd; ++i) {
            const bool sel = (i == kioskDiskSel_);
            const ImVec4 col = zGames ? kGreen : kDim;
            // ● marks the disk currently in the boot drive (flip-disk anchor).
            const bool mounted = !kioskMountedPath_.empty() &&
                                 kioskDiskPaths_[i] == kioskMountedPath_;
            rowText(sel, col, "%s%s", mounted ? ICON_FA_COMPACT_DISC " " : "",
                    kioskDiskLabels_[i].c_str());
        }
        ImGui::EndChild();

        // Actions column.
        ImGui::SetWindowFontScale(2.3f);
        const bool zAct = (kioskZone_ == KioskZone::Actions);
        auto actionRow = [&](int idx, const ImVec4& col, const char* label) {
            const bool sel = (kioskActSel_ == idx);
            ImGui::PushStyleColor(ImGuiCol_Text, (sel && zAct) ? kGreen : (zAct ? col : kDim));
            ImGui::Text("%s %s", (sel && zAct) ? "\xe2\x96\xb6" : "  ", label);
            ImGui::PopStyleColor();
        };
        actionRow(0, ImVec4(1.0f, 0.60f, 0.15f, 1.0f), ICON_FA_ROTATE " RESTART MACHINE");
        actionRow(1, ImVec4(0.55f, 0.80f, 1.0f, 1.0f), ICON_FA_KEYBOARD " KEYBOARD");
        actionRow(2, ImVec4(0.60f, 0.95f, 0.60f, 1.0f), ICON_FA_FOLDER_OPEN " ROM FOLDERS");
        actionRow(3, ImVec4(1.0f, 0.50f, 0.40f, 1.0f), ICON_FA_RIGHT_FROM_BRACKET " QUIT");

        ImGui::SetWindowFontScale(1.3f);
        ImGui::Separator();
        ImGui::TextColored(kYellow, ICON_FA_COMPACT_DISC " Disks found: %d", nd);
        if (!kioskStatus_.empty()) ImGui::TextColored(kGrey, "%s", kioskStatus_.c_str());
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= KEYBOARD band (game runs underneath) ==============
    else if (kioskPage_ == KioskPage::Keys) {
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(kYellow, ICON_FA_KEYBOARD " KEYBOARD");
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "  A send  \xc2\xb7  B close  \xc2\xb7  game keeps running");
        ImGui::Separator();
        for (int r = 0; r < kKioskKeyRowN; ++r) {
            ImGui::SetWindowFontScale(2.2f);
            for (int i = kKioskKeyRows[r][0]; i < kKioskKeyRows[r][1]; ++i) {
                const bool sel = (i == kioskKeySel_);
                if (i > kKioskKeyRows[r][0]) ImGui::SameLine();
                if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                char cell[24];
                std::snprintf(cell, sizeof cell, sel ? "[%s]" : " %s ", kKioskKeys[i].label);
                ImGui::TextUnformatted(cell);
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 4));
        }
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= QUIT confirmation =================================
    else if (kioskPage_ == KioskPage::Quit) {
        ImGui::SetWindowFontScale(3.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), ICON_FA_RIGHT_FROM_BRACKET " QUIT?");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 30));
        ImGui::SetWindowFontScale(2.2f);
        ImGui::TextColored(kGreen, ICON_FA_POWER_OFF " (A) Yes, quit");
        ImGui::SetWindowFontScale(1.8f);
        ImGui::TextColored(kGrey, "(B) No, back to menu");
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= ROM folders manager ==============================
    else if (kioskPage_ == KioskPage::RomDirs) {
        ImGui::SetWindowFontScale(2.6f);
        ImGui::TextColored(kYellow, ICON_FA_FOLDER_OPEN " ROM FOLDERS");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "up/down move   \xc2\xb7   A add / remove   \xc2\xb7   B back");
        ImGui::Separator();
        ImGui::BeginChild("##kRomDirs", ImVec2(0, 0), true);
        ImGui::SetWindowFontScale(2.2f);
        const int total = 1 + int(kioskRomDirs_.size());
        for (int i = 0; i < total; ++i) {
            const bool sel = (i == kioskRomDirSel_);
            if (i == 0) {
                rowText(sel, kGreen, ICON_FA_PLUS " [ ADD A FOLDER ]");
            } else {
                if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                ImGui::Text("%s " ICON_FA_XMARK " %s", sel ? "\xe2\x96\xb6" : "  ",
                            kioskRomDirs_[i - 1].c_str());
                if (sel) { ImGui::PopStyleColor(); ImGui::SetScrollHereY(0.5f); }
            }
        }
        if (kioskRomDirs_.empty()) {
            ImGui::SetWindowFontScale(1.3f);
            ImGui::TextColored(kDim, "   (only the booted disk's folder is scanned)");
        }
        ImGui::EndChild();
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= Directory browser ================================
    else if (kioskPage_ == KioskPage::Browse) {
        ImGui::SetWindowFontScale(2.4f);
        ImGui::TextColored(kYellow, ICON_FA_FOLDER_OPEN " SELECT ROM FOLDER");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "up/down move   \xc2\xb7   A enter / select   \xc2\xb7   B cancel");
        ImGui::Separator();
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", kioskBrowseDir_.c_str());
        ImGui::Separator();
        ImGui::BeginChild("##kBrowse", ImVec2(0, 0), true);
        ImGui::SetWindowFontScale(2.1f);
        const int nShort = int(kioskBrowseShortcutPaths_.size());
        const int total  = 2 + nShort + int(kioskBrowseSubdirs_.size());
        for (int i = 0; i < total; ++i) {
            const bool sel = (i == kioskBrowseSel_);
            if (i == 0)                 rowText(sel, kGreen, ICON_FA_STAR " [ USE THIS FOLDER ]");
            else if (i == 1)            rowText(sel, kGreen, ICON_FA_FOLDER_OPEN " ..");
            else if (i < 2 + nShort)    rowText(sel, kGreen, "%s", kioskBrowseShortcutLabels_[i - 2].c_str());
            else                        rowText(sel, kGreen, ICON_FA_FOLDER_OPEN " %s",
                                                fs::path(kioskBrowseSubdirs_[i - 2 - nShort]).filename().string().c_str());
        }
        ImGui::EndChild();
        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(keysPage ? 1 : 2);
}

// ─── Kiosk ROM-folders manager + directory browser ──────────────────────

void MainWindow::kioskScanBrowse(const std::string& dir)
{
    namespace fs = std::filesystem;
    kioskBrowseSubdirs_.clear();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) kioskBrowseSubdirs_.push_back(it->path().string());
    }
    std::sort(kioskBrowseSubdirs_.begin(), kioskBrowseSubdirs_.end(),
              [](const std::string& a, const std::string& b) {
                  return fs::path(a).filename().string() < fs::path(b).filename().string();
              });
    kioskBrowseSel_ = 0;
}

void MainWindow::kioskComputeShortcuts()
{
    namespace fs = std::filesystem;
    kioskBrowseShortcutPaths_.clear();
    kioskBrowseShortcutLabels_.clear();
    auto add = [&](const std::string& path, const std::string& label) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) return;
        if (std::find(kioskBrowseShortcutPaths_.begin(), kioskBrowseShortcutPaths_.end(),
                      path) != kioskBrowseShortcutPaths_.end()) return;   // dedup
        kioskBrowseShortcutPaths_.push_back(path);
        kioskBrowseShortcutLabels_.push_back(label);
    };
    add("/", std::string(ICON_FA_SERVER) + " / (filesystem root)");
    if (const char* home = std::getenv("HOME"))
        add(home, std::string(ICON_FA_FOLDER_OPEN) + " Home");
    // Removable-media mount points (guarded so each OS only shows what exists).
    const char* user = std::getenv("USER");
    std::vector<std::string> roots{ "/Volumes" };
    if (user) { roots.push_back(std::string("/run/media/") + user);
                roots.push_back(std::string("/media/") + user); }
    roots.push_back("/mnt");
    for (const auto& r : roots) {
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        for (fs::directory_iterator it(r, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2))
                add(it->path().string(),
                    std::string(ICON_FA_HARD_DRIVE) + " " + it->path().filename().string());
        }
    }
}

// Extra ROM folders persist OUTSIDE state.cfg (kiosk keeps its main config
// read-only), in a sibling "kiosk_romdirs.txt" — one absolute path per line.
static std::filesystem::path kioskRomDirsFile(const pom2::Settings& s)
{
    namespace fs = std::filesystem;
    fs::path store = s.getStorePath();
    fs::path dir = store.has_parent_path() ? store.parent_path() : fs::path(".");
    return dir / "kiosk_romdirs.txt";
}

void MainWindow::kioskLoadRomDirs()
{
    namespace fs = std::filesystem;
    kioskRomDirs_.clear();
    std::ifstream f(kioskRomDirsFile(*settings));
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::error_code ec;
        if (!line.empty() && fs::is_directory(line, ec)) kioskRomDirs_.push_back(line);
    }
}

void MainWindow::kioskSaveRomDirs()
{
    std::ofstream f(kioskRomDirsFile(*settings), std::ios::trunc);
    if (!f) return;
    for (const auto& d : kioskRomDirs_) f << d << '\n';
}

bool MainWindow::kioskPruneRomDirs()
{
    namespace fs = std::filesystem;
    bool changed = false;
    for (size_t i = 0; i < kioskRomDirs_.size(); ) {
        std::error_code ec;
        if (!fs::is_directory(kioskRomDirs_[i], ec)) {
            kioskRomDirs_.erase(kioskRomDirs_.begin() + long(i));
            changed = true;
        } else ++i;
    }
    return changed;
}

// ─── Mouse Card input routing ───────────────────────────────────────────

void MainWindow::onMouseMove(double x, double y)
{
    // First call after startup just seeds last-position; no delta yet.
    if (!mouseInited) {
        lastMouseHostX = x;
        lastMouseHostY = y;
        mouseInited = true;
        return;
    }
    const double rawDx = x - lastMouseHostX;
    const double rawDy = y - lastMouseHostY;
    lastMouseHostX = x;
    lastMouseHostY = y;

    // Either MAME-faithful MouseCard or AppleWin HLE MouseCardAppleWin
    // can be plugged (mutually exclusive). Both expose the same
    // `setHostMouse(rawX, rawY, button)` + `getSlot()` API; route through
    // tiny lambdas so the absolute / relative cursor logic below stays
    // variant-agnostic.
    if (!mouseCard && !mouseAwCard) return;
    auto pushMouse = [&](uint8_t rx, uint8_t ry, bool btn) {
        if (mouseCard)   mouseCard  ->setHostMouse(rx, ry, btn);
        if (mouseAwCard) mouseAwCard->setHostMouse(rx, ry, btn);
    };
    // Need a valid Apple II Screen widget rect to map host pixels into
    // Apple-cursor coordinates. Bail until renderScreen has populated it.
    const float widgetW = screenRectMax.x - screenRectMin.x;
    const float widgetH = screenRectMax.y - screenRectMin.y;
    if (widgetW <= 0.0f || widgetH <= 0.0f) return;

    // ── Absolute closed-loop cursor sync (AppleWin HLE only) ───────────
    // When the `mouseaw` card is plugged AND the AppleMouse firmware has
    // been turned on (MODE_MOUSE_ON, bit 0 of the latched MODE byte), the
    // card's HLE'd MCU keeps the cursor position in `iX/iY` clamped to
    // the firmware-installed window `[iMinX..iMaxX] × [iMinY..iMaxY]`.
    // We read that authoritative state via the debug snapshot, project
    // the host cursor's position onto the widget rect (saturating clamp
    // outside, so wandering out of the widget pins the Apple cursor at
    // the matching edge instead of letting it drift), and inject the
    // delta needed to drive `iX/iY` toward the projected target. The
    // earlier closed-loop attempt (reverted in commit ccd9a95) failed
    // because it assumed the clamp range equalled the display resolution
    // — that's true for the //e desktop but wrong for e.g. MousePaint's
    // 0..559 horizontal clamp. Using the card-reported clamp window
    // sidesteps that guess entirely.
    // Each push is bounded to ±127 (the MCU's 8-bit signed wrap range);
    // large gaps (first event after re-entry, big window resize) converge
    // over several events.
    bool absoluteHandled = false;
    if (mouseAwCard) {
        const auto s = mouseAwCard->debugSnapshot();
        const bool mouseOn = (s.byMode & 0x01) != 0;
        const int rangeX = s.iMaxX - s.iMinX;
        const int rangeY = s.iMaxY - s.iMinY;
        if (mouseOn && rangeX > 0 && rangeY > 0) {
            const double fracX = std::clamp(
                (x - double(screenRectMin.x)) / double(widgetW), 0.0, 1.0);
            const double fracY = std::clamp(
                (y - double(screenRectMin.y)) / double(widgetH), 0.0, 1.0);
            const int targetX = s.iMinX + int(fracX * rangeX + 0.5);
            const int targetY = s.iMinY + int(fracY * rangeY + 0.5);
            int dx = targetX - s.iX;
            int dy = targetY - s.iY;
            if (dx >  127) dx =  127;
            if (dx < -127) dx = -127;
            if (dy >  127) dy =  127;
            if (dy < -127) dy = -127;
            mouseAppleX = static_cast<uint8_t>(mouseAppleX + dx);
            mouseAppleY = static_cast<uint8_t>(mouseAppleY + dy);
            pushMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
            // Drop relative sub-pixel residue so a later fallback (mouse
            // turned off mid-session) doesn't replay stale fractional
            // motion accumulated before sync was active.
            mouseSubAppleX = 0.0;
            mouseSubAppleY = 0.0;
            absoluteHandled = true;
        }
    }
    if (absoluteHandled) return;

    // ── Relative drive (fallback) ───────────────────────────────────
    // Used by the MAME-faithful MouseCard (no iX/iY exposed — firmware
    // lives inside the 68705P3 MCU's internal RAM) and by the AppleWin
    // HLE card before the firmware enables MOUSE_ON. Gate on cursor
    // inside the widget so the host can still drive ImGui menus/panels
    // outside the screen.
    if (x < screenRectMin.x || x > screenRectMax.x ||
        y < screenRectMin.y || y > screenRectMax.y) {
        return;
    }

    // ── Speed mapping (relative drive — the only path) ──────────────
    // The closed-loop absolute sync was an experiment that didn't survive
    // contact with real apps: the cursor's real clamp range lives behind the
    // firmware (the MCU on the //e card, the internal ROM on the //c) and the
    // app's ClampMouse parameters don't reliably land in 6502-readable holes
    // for MGTK-based apps (A2Desktop/MousePaint), so any absolute target was
    // guesswork. The proven proportional drive below — what AppleWin/MAME do
    // — gives no centre-jump and lets the app's own firmware clamp at its
    // own edges naturally.
    // Used when the AppleMouse firmware is off or its clamp window is
    // non-standard (holes out of display range). Convert host-pixel
    // deltas to Apple-cursor units so 1 host pixel of motion = 1 host
    // pixel of cursor motion visually in the widget.
    //   apple_per_host_px = logical_screen_dim / widget_host_dim
    // The widget is ALWAYS drawn at kWidth(280)×kHeight(192) aspect
    // (drawScreenImage), so the X mapping must use kWidth, NOT
    // display->width() — the latter returns 560 in DHGR/80-col, which made
    // X track 2× faster than Y in 80-column mode (where A2Desktop runs).
    // Both axes now share the same logical→widget scale. Sub-pixel motion
    // accumulates across events.
    const double sxRatio = double(Apple2Display::kWidth)  / double(widgetW);
    const double syRatio = double(Apple2Display::kHeight) / double(widgetH);
    mouseSubAppleX += rawDx * sxRatio;
    mouseSubAppleY += rawDy * syRatio;
    int dxApple = static_cast<int>(mouseSubAppleX);
    int dyApple = static_cast<int>(mouseSubAppleY);
    // Clamp BEFORE consuming the sub-pixel accumulator so big jumps
    // (>127 ticks in one event, e.g. cursor teleported across widget)
    // carry the residual forward to the next event instead of being
    // silently dropped. ±127 = MCU's 8-bit signed wrap-correction range.
    if (dxApple >  127) dxApple =  127;
    if (dxApple < -127) dxApple = -127;
    if (dyApple >  127) dyApple =  127;
    if (dyApple < -127) dyApple = -127;
    mouseSubAppleX -= dxApple;
    mouseSubAppleY -= dyApple;

    mouseAppleX = static_cast<uint8_t>(mouseAppleX + dxApple);
    mouseAppleY = static_cast<uint8_t>(mouseAppleY + dyApple);
    pushMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
}

void MainWindow::onMouseButton(int button, int action)
{
    // Only the primary button is wired to the Apple Mouse Card (PB7 of
    // the MCU). GLFW button 0 = left.
    if (button != 0) return;
    // Gate on cursor-in-screen-rect: clicks outside the Apple II Screen
    // widget belong to ImGui (menus, panels, etc.). Inside the screen
    // widget, we route to the Mouse Card. PRESS uses the current cursor
    // position; RELEASE always passes through so a button pressed inside
    // the screen but released outside still gets cleared on the card.
    const bool press = (action != 0);
    if (press) {
        const double x = lastMouseHostX;
        const double y = lastMouseHostY;
        if (x < screenRectMin.x || x > screenRectMax.x ||
            y < screenRectMin.y || y > screenRectMax.y) {
            return;
        }
    }
    mouseButtonHeld = press;             // GLFW_RELEASE = 0, others = press/repeat
    if (mouseCard)
        mouseCard->setHostMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
    if (mouseAwCard)
        mouseAwCard->setHostMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
}

void MainWindow::bootHdvImage()
{
    pom2::ProDOSBlockCard* dev = hdvDevice();
    if (!dev || !dev->isImageLoaded()) {
        tapeStatusMessage = "HDV boot failed: no image loaded";
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    const std::string p = dev->getImagePath();
    // Boot from the slot the HDV/CFFA card is actually plugged in — the user
    // can move it to slot 2 / 7 / etc. via Slot Configuration and the
    // boot path follows. The card's slot ROM bakes its slot number into
    // the ProDOS dispatcher trampolines, so `bootFromSlot(N)` lands on
    // the right $C(N)00 entry point automatically.
    controller->bootFromSlot(dev->getSlot());
    tapeStatusMessage = "Booting HDV (slot " +
        std::to_string(dev->getSlot()) + "): " + p;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

void MainWindow::renderPasteFileDialog()
{
    if (showPasteFileDialog) {
        ImGui::OpenPopup("Paste from file");
        showPasteFileDialog = false;
    }
    if (ImGui::BeginPopupModal("Paste from file", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path to a text file (Applesoft listing, etc.)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", pasteDialogPath.c_str());
        if (ImGui::InputText("##PastePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            pasteDialogPath = buf;
        else
            pasteDialogPath = buf;
        ImGui::Checkbox("Auto-uppercase", &pasteAutoUppercase);
        ImGui::Separator();
        if (ImGui::Button("Paste", ImVec2(120, 0))) {
            if (!pasteDialogPath.empty()) pasteFromFile(pasteDialogPath);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ─── Joystick / paddles ──────────────────────────────────────────────────

void MainWindow::pollJoystickAndPushToMemory()
{
    joystick->poll();
    joystick->autoBindIfUnconfigured();

    // One-shot diagnostic when the bound pad (or its gamepad-mapping status)
    // changes: the kiosk Start-menu only works when gamepad-mapped=yes. If a
    // pad is present but reports "no", GLFW has no standard mapping for it,
    // so use the F10 keyboard fallback (or add an SDL mapping).
    {
        const int  hi = joystick->binding().hostIdx;
        const bool gp = joystick->activeIsGamepad();
        if (hi != loggedJoyHost_ || gp != loggedJoyGamepad_) {
            loggedJoyHost_    = hi;
            loggedJoyGamepad_ = gp;
            if (hi >= 0) {
                pom2::log().info(
                    "Joystick", "bound #" + std::to_string(hi + 1) + " '" +
                    joystick->activeName() + "' gamepad-mapped=" +
                    (gp ? "yes (Start opens kiosk disk menu)"
                        : "no (use F10 for the kiosk disk menu)"));
            } else {
                pom2::log().info("Joystick", "no pad bound");
            }
        }
    }

    // Apple II paddles (4) and push buttons (3). The Memory side already
    // handles the $C064-$C067 RC discharge model and $C061-$C063 push
    // buttons; we just hand it fresh values once per frame. Hold stateMutex
    // while writing: the CPU worker reads paddleValue/paddleButton inside
    // softSwitchAccess under the same lock (during processor.run()), so an
    // unlocked write here is a data race on those non-atomic arrays.
    // While the kiosk disk selector is open the pad drives the menu, not the
    // game: feed the Apple II centered paddles + released buttons so the A/B
    // navigation presses (which share physical buttons with PB0/PB1) don't
    // leak into the running title.
    const bool menuActive = kioskMenuOpen_;
    const JoystickInput::GamepadPlay play = joystick->play();

    // Menu → game isolation across the close. Circle/Cross double as the
    // menu's B/A and the Apple PB0/PB1, and this poll runs BEFORE
    // updateKioskMenu, so `menuActive` lags the close by a frame: the press
    // that dismissed the menu would land in the game as a fire-button hit.
    // Latch a swallow on the open→closed edge and hold it until every shared
    // button (faces + D-pad) is released. Analog paddles stay live — the
    // stick isn't a menu control and carries no edge.
    // Only gamepad-mapped pads need the latch: raw pads can't drive the menu
    // (nav requires a mapping), so a fire button held across a keyboard-
    // driven close is a legitimate game input, not a menu leftover.
    if (kioskMenuWasOpen_ && !menuActive && play.valid) kioskSwallowPad_ = true;
    kioskMenuWasOpen_ = menuActive;
    if (kioskSwallowPad_) {
        const bool anyHeld = play.valid
            ? (play.button0 || play.button1 || play.dpadUp || play.dpadDown ||
               play.dpadLeft || play.dpadRight)
            : (joystick->buttonDown(0) || joystick->buttonDown(1) ||
               joystick->buttonDown(2));
        if (!anyHeld) kioskSwallowPad_ = false;
    }
    const bool suppressGame = menuActive || kioskSwallowPad_;

    Memory& mem = controller->memory();
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        for (int i = 0; i < 4; ++i)
            mem.setPaddle(i, menuActive ? 128 : joystick->paddleValue(i));

        if (suppressGame) {
            for (int i = 0; i < 3; ++i) mem.setPaddleButton(i, false);
        } else if (play.valid) {
            // Gamepad-mapped: only Cross/Circle are Apple game-port buttons;
            // the other face buttons are keyboard keys (below), so PB2 is up.
            mem.setPaddleButton(0, play.button0);   // Circle → PB0
            mem.setPaddleButton(1, play.button1);   // Cross  → PB1
            mem.setPaddleButton(2, false);
        } else {
            // Raw pad (unknown layout): legacy buttons 0/1/2 → PB0/1/2.
            for (int i = 0; i < 3; ++i) mem.setPaddleButton(i, joystick->buttonDown(i));
        }
    }

    // Keyboard routing for the digital controls — outside stateMutex, since
    // queueKey has its own keyboard lock. Only in-game (menu closed, swallow
    // drained) and only for a gamepad-mapped pad whose layout we can trust.
    if (suppressGame || !play.valid) {
        // Drop the auto-repeat history so a direction still held from menu
        // navigation re-arms cleanly (press-then-delay) once released.
        for (bool& h : padArrowHeld_) h = false;
    } else {
        if (play.spaceEdge) mem.queueKey(0x20);   // Square   → SPACE
        if (play.enterEdge) mem.queueKey(0x0D);   // Triangle → RETURN
        // D-pad → Apple II arrow codes (←$08 →$15 ↑$0B ↓$0A) with auto-repeat
        // so a held direction keeps moving, like the //e keyboard.
        const bool    held[4] = { play.dpadUp, play.dpadDown, play.dpadLeft, play.dpadRight };
        const uint8_t code[4] = { 0x0B, 0x0A, 0x08, 0x15 };
        const double  t = ImGui::GetTime();
        for (int i = 0; i < 4; ++i) {
            if (!held[i]) { padArrowHeld_[i] = false; continue; }
            if (!padArrowHeld_[i]) {                       // press: fire once
                mem.queueKey(code[i]);
                padArrowHeld_[i]  = true;
                padArrowNextT_[i] = t + 0.35;              // delay before repeat
            } else if (t >= padArrowNextT_[i]) {           // held: repeat
                mem.queueKey(code[i]);
                padArrowNextT_[i] = t + 0.06;              // ~16/s
            }
        }
    }
}

void MainWindow::renderSscPanelWindow()
{
    if (!showSscPanel || sscCards.empty()) return;

    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Super Serial###sscPanel", &showSscPanel)) {
        ImGui::End();
        return;
    }

    // One panel hosts every plugged SSC under a tab bar. //c boots with
    // two (printer + modem); other profiles typically run zero or one.
    // Per-slot port-input state lives in a static map so each tab keeps
    // its own draft port number across frames.
    auto renderOne = [&](SuperSerialCard* ssc) {
        if (!ssc) return;
        const int slot = ssc->getSlot();
        static std::map<int, int> portDrafts;
        auto it = portDrafts.find(slot);
        if (it == portDrafts.end()) {
            portDrafts[slot] = ssc->getPort() ? ssc->getPort()
                : SuperSerialCard::kDefaultPort;
            it = portDrafts.find(slot);
        }
        int& portDraft = it->second;

        const bool listening = ssc->isListening();
        const bool connected = ssc->clientConnected();

        ImGui::Text("Status: %s%s",
            listening ? "listening" : "stopped",
            connected ? " — client connected" : "");
        ImGui::SameLine();
        ImGui::TextDisabled("(slot %d)", slot);

        ImGui::Separator();
        ImGui::PushID(slot);
#ifdef __EMSCRIPTEN__
        // The host telnet bridge is compiled out under WASM
        // (SuperSerialCard::startListening() is a no-op — no TCP sockets in
        // the browser sandbox, see SuperSerialCard.cpp). The ACIA itself is
        // still emulated, so PR#n output still flows into the TX counter /
        // recent traffic below. Grey out the listener controls rather than
        // hide them, so the panel reads honestly.
        (void)listening; (void)connected;
        ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("TCP port", &portDraft, 0, 0);
        ImGui::SameLine();
        ImGui::Button("Start listener");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Telnet bridge unavailable in the browser build.");
#else
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("TCP port", &portDraft, 0, 0);
        if (portDraft < 1)     portDraft = 1;
        if (portDraft > 65535) portDraft = 65535;

        ImGui::SameLine();
        if (!listening) {
            if (ImGui::Button("Start listener")) {
                if (!ssc->startListening(static_cast<uint16_t>(portDraft))) {
                    tapeStatusMessage = "SSC slot " + std::to_string(slot) +
                        ": bind failed (port busy?)";
                    tapeStatusUntil   = lastFrameTime + 4.0;
                }
            }
        } else {
            if (ImGui::Button("Stop listener")) ssc->stopListening();
        }

        if (listening) {
            ImGui::TextWrapped("Connect from a host terminal:");
            ImGui::TextWrapped("  telnet 127.0.0.1 %d", ssc->getPort());
            ImGui::TextWrapped("In the Apple II:  PR#%d  (or IN#%d for input)",
                slot, slot);
        } else {
            ImGui::TextDisabled("Click Start, then telnet to the port to "
                                "bridge I/O between your host shell and "
                                "the Apple II.");
        }
#endif

        ImGui::Separator();
        bool raw = ssc->rawMode();
        if (ImGui::Checkbox("Raw mode (8-bit binary)", &raw)) {
            ssc->setRawMode(raw);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Off: stock telnet — IAC ($FF) negotiation\n"
                              "swallowed + CR/LF normalised to CR.\n"
                              "On: every byte forwarded verbatim. Use for\n"
                              "XMODEM / Kermit / ADTPro / any binary protocol.");
        }

        ImGui::Separator();
        ImGui::Text("RX (telnet → A2): %llu B",
            static_cast<unsigned long long>(ssc->bytesRx()));
        ImGui::Text("TX (A2 → telnet): %llu B",
            static_cast<unsigned long long>(ssc->bytesTx()));

        if (ImGui::CollapsingHeader("Recent traffic")) {
            ImGui::TextDisabled("Last bytes the Apple II printed via PR#%d:",
                                slot);
            ImGui::TextWrapped("%s", ssc->recentTxText().c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Last bytes the host typed:");
            ImGui::TextWrapped("%s", ssc->recentRxText().c_str());
        }
        ImGui::PopID();
    };

    if (sscCards.size() == 1) {
        renderOne(sscCards[0]);
    } else if (ImGui::BeginTabBar("##sscTabs")) {
        // //c convention: sl1 = printer port, sl2 = modem port. Other
        // profiles just label by slot number.
        const bool isIIcLayout = (sscCards.size() == 2) &&
            (sscCards[0]->getSlot() == 1) && (sscCards[1]->getSlot() == 2);
        for (size_t i = 0; i < sscCards.size(); ++i) {
            const int slot = sscCards[i]->getSlot();
            std::string tab;
            if (isIIcLayout) tab = (i == 0) ? "Printer port (sl1)"
                                            : "Modem port (sl2)";
            else             tab = "Slot " + std::to_string(slot);
            if (ImGui::BeginTabItem(tab.c_str())) {
                renderOne(sscCards[i]);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void MainWindow::renderPrinterPanelWindow()
{
    if (!showPrinterPanel || !printerCard) return;

    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    const std::string title = "Printer (slot " +
        std::to_string(printerCard->getSlot()) + ")###printerPanel";
    if (!ImGui::Begin(title.c_str(), &showPrinterPanel)) {
        ImGui::End();
        return;
    }

    const size_t nBytes = printerCard->bytesWritten();
    ImGui::Text("Spool: %zu byte%s", nBytes, nBytes == 1 ? "" : "s");
    ImGui::SameLine();
    ImGui::TextDisabled("— PR#%d from BASIC sends output here",
                        printerCard->getSlot());

    ImGui::Separator();

    // Auto-suggest a timestamped path on first open so the user can hit
    // Save without typing anything. printerSavePath persists across saves
    // within a session — the user can edit it freely.
    if (printerSavePath.empty()) {
        const auto t   = std::time(nullptr);
        const auto tm  = *std::localtime(&t);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        printerSavePath = std::string("printouts/spool-") + stamp + ".txt";
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", printerSavePath.c_str());
#ifdef __EMSCRIPTEN__
    // No persistent host filesystem in the browser build: a "Save as .txt"
    // would land in MEMFS and vanish on reload, with no download path. Grey
    // the controls out — the live spool preview below still works (PR#n
    // output is captured), and "Clear spool" still resets the buffer.
    ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-110);
    ImGui::InputText("##printerPath", buf, sizeof(buf));
    ImGui::SameLine();
    ImGui::Button("Save as .txt", ImVec2(100, 0));
    ImGui::EndDisabled();
    ImGui::TextDisabled("Saving to a file is unavailable in the browser build.");
#else
    ImGui::SetNextItemWidth(-110);
    if (ImGui::InputText("##printerPath", buf, sizeof(buf))) {
        printerSavePath = buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as .txt", ImVec2(100, 0))) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path p = fs::path(printerSavePath);
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) {
            printerLastSaveStatus = "Save failed: cannot open " +
                                    p.string();
        } else {
            const std::string text = printerCard->spoolText();
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.close();
            printerLastSaveStatus = "Saved " + std::to_string(text.size()) +
                                    " bytes → " + p.string();
        }
    }
#endif

    // Print with e-mail — compose a mailto: URL carrying the spool text
    // and hand it to the host's default mail client (the browser's mail
    // handler on the WASM build, where it works even though file saves
    // don't). Composition + escaping live in PrintToEmail.cpp, pinned by
    // the printer_email_smoke test.
    {
        char ebuf[256];
        std::snprintf(ebuf, sizeof(ebuf), "%s", printerEmailTo.c_str());
        ImGui::SetNextItemWidth(-110);
        if (ImGui::InputTextWithHint("##printerEmailTo",
                                     "e-mail to (name@example.com)",
                                     ebuf, sizeof(ebuf))) {
            printerEmailTo = ebuf;
        }
        ImGui::SameLine();
        const bool canMail =
            (nBytes > 0) && pom2::printmail::looksLikeEmail(printerEmailTo);
        ImGui::BeginDisabled(!canMail);
        if (ImGui::Button("E-mail spool", ImVec2(100, 0))) {
            const std::string spoolBody = printerCard->spoolText();
            if (spoolBody.empty()) {
                // bytesWritten() can be non-zero on a NUL-only spool
                // (strobe noise) that spoolText() renders empty.
                printerLastSaveStatus =
                    "Spool contains no printable text — nothing to send";
            } else {
                const auto t  = std::time(nullptr);
                const auto tm = *std::localtime(&t);
                char stamp[32];
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm);
                const auto mail = pom2::printmail::buildMailtoUrl(
                    printerEmailTo,
                    std::string("POM2 printout ") + stamp,
                    spoolBody);
                std::string err;
                if (pom2::printmail::openMailClient(mail.url, &err)) {
#ifdef __EMSCRIPTEN__
                    // No file saves in the browser build — point at the
                    // preview instead of the greyed-out Save button.
                    printerLastSaveStatus = mail.truncated
                        ? "Opened mail client — spool truncated to fit the "
                          "message body; copy the full text from the preview"
                        : "Opened mail client with the spool in the message body";
#else
                    printerLastSaveStatus = mail.truncated
                        ? "Opened mail client — spool truncated to fit the "
                          "message body; use Save as .txt for the full printout"
                        : "Opened mail client with the spool in the message body";
#endif
                } else {
                    printerLastSaveStatus = "E-mail failed: " + err;
                }
            }
        }
        ImGui::EndDisabled();
        if (!canMail) {
            ImGui::TextDisabled(nBytes == 0
                ? "(print something first — the spool is empty)"
                : "(enter a valid e-mail address to enable sending)");
        }
    }

    if (!printerLastSaveStatus.empty()) {
        ImGui::TextDisabled("%s", printerLastSaveStatus.c_str());
    }

    if (ImGui::Button("Clear spool")) {
        printerCard->clearSpool();
        printerLastSaveStatus.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(host-side buffer only — does NOT touch the Apple II)");

    ImGui::Separator();
    ImGui::TextDisabled("Preview (high bit stripped, CR → LF):");

    // Snapshot once per frame; printerCard mutates this from the CPU
    // thread, but we hold the state lock everywhere it's touched so a
    // single read is consistent.
    const std::string preview = printerCard->spoolText();
    ImGui::BeginChild("##printerPreview", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (preview.empty()) {
        ImGui::TextDisabled("(empty — try `PR#%d : PRINT \"HELLO\"` from BASIC)",
                            printerCard->getSlot());
    } else {
        ImGui::TextUnformatted(preview.data(),
                               preview.data() + preview.size());
    }
    ImGui::EndChild();

    ImGui::End();
}

void MainWindow::renderAiControlPanelWindow()
{
    if (!showAiControlPanel) return;

    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Control (HTTP)", &showAiControlPanel)) {
        ImGui::End();
        return;
    }

    const bool running = aiServer->isRunning();
    ImGui::Text("Status: %s", running ? "listening" : "stopped");
    if (running) {
        ImGui::SameLine();
        ImGui::TextDisabled("on 127.0.0.1:%u", aiServer->getPort());
    }
    ImGui::Text("Requests served: %llu",
                static_cast<unsigned long long>(aiServer->requestsServed()));
    {
        const std::string addr = aiServer->lastClientAddr();
        if (!addr.empty()) ImGui::Text("Last client: %s", addr.c_str());
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("TCP port", &aiPortInput, 0, 0);
    if (aiPortInput < 1)     aiPortInput = 1;
    if (aiPortInput > 65535) aiPortInput = 65535;

    char tokenBuf[128];
    std::snprintf(tokenBuf, sizeof(tokenBuf), "%s", aiTokenInput.c_str());
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText("Auth token (empty = open)", tokenBuf, sizeof(tokenBuf))) {
        aiTokenInput = tokenBuf;
        aiServer->setAuthToken(aiTokenInput);
    }

    ImGui::SameLine();
    if (!running) {
        if (ImGui::Button("Start")) {
            // Re-attach in case slot cards were rebuilt by the slot config
            // panel since the last start — pointers may have moved.
            aiServer->attach(controller.get(), display.get(), diskCard, hdvCard);
            aiServer->setAuthToken(aiTokenInput);
            if (!aiServer->start(static_cast<uint16_t>(aiPortInput))) {
                tapeStatusMessage = "AI Control: bind failed (port busy?)";
                tapeStatusUntil   = lastFrameTime + 4.0;
            }
        }
    } else {
        if (ImGui::Button("Stop")) aiServer->stop();
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Drive POM2 from an AI agent (or curl/Postman) via HTTP/1.1.");
    ImGui::Spacing();
    ImGui::TextDisabled("Example:");
    ImGui::TextWrapped("  curl http://127.0.0.1:%d/status", aiPortInput);
    ImGui::TextWrapped(
        "  curl -d '{\"text\":\"PRINT 1+1\\r\"}' http://127.0.0.1:%d/keyboard",
        aiPortInput);
    ImGui::TextWrapped(
        "  curl http://127.0.0.1:%d/screen.ppm -o screen.ppm",
        aiPortInput);
    if (!aiTokenInput.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Send 'X-POM2-Token: <token>' header on each request.");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Endpoints: /status /reset /cpu /mem /keyboard /disk "
                        "/eject /snapshot/save /snapshot/load /speed /screen.ppm");

    ImGui::End();
}

void MainWindow::renderNoSlotClockPanelWindow()
{
    if (!showNoSlotClockPanel) return;

    ImGui::SetNextWindowSize(ImVec2(420, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("No-Slot Clock (Dallas DS1216E)###nsclockPanel",
                      &showNoSlotClockPanel)) {
        ImGui::End();
        return;
    }

    pom2::NoSlotClock& nsc = controller->noSlotClock();
    bool enabled = nsc.isEnabled();
    if (ImGui::Checkbox("Enabled", &enabled)) {
        nsc.setEnabled(enabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Dallas DS1216E SmartWatch — virtual chip under the\n"
            "internal ROM. AppleWin-parity placement:\n"
            "  II / II+   : under Monitor ROM at $F800-$FFFF\n"
            "  //e / //c  : under $C300 + $C800 internal ROM\n"
            "ProDOS 2.0.3+ / GS-OS walk the 64-bit magic key\n"
            "0x5CA33AC55CA33AC5 (A2=0 reads, A0 = next bit),\n"
            "then read 64 clock bits via A2=1 reads on D0.");
    }

    ImGui::Separator();
    const auto phase = nsc.phase();
    const char* phaseName = (phase == pom2::NoSlotClock::Phase::Idle)
        ? "idle (pass-through)"
        : (phase == pom2::NoSlotClock::Phase::MatchingKey)
            ? "matching magic key"
            : "reading clock register";
    ImGui::Text("Phase: %s", phaseName);
    ImGui::Text("Key bits matched : %d / 64", nsc.keyBitsMatched());
    ImGui::Text("Clock bits read  : %d / 64", nsc.clockBitsRead());

    ImGui::Separator();
    ImGui::TextWrapped(
        "Place a free clock card in a slot for older software, or "
        "leave this enabled for ProDOS 2.0.3+/GS-OS auto-detection "
        "on any profile (incl. //c, where no slot card can exist).");

    ImGui::End();
}

// ─── 3D voxel view settings (MicroM8 "Voxel Cube") ───────────────────────
//
// Live sliders for the geometry knobs the Voxel3DRenderer exposes: cube
// thickness, the per-colour forward "pop", footprint fill, supersample
// (anti-alias) factor and the ambient floor. The grid resolution is NOT a
// knob — it always tracks the live display (one voxel per Apple II pixel).
// Values persist under the `voxel_*` keys; they bind straight to `voxel3d_`
// (owned up-front at settings-load, so the panel works before the view is on).
void MainWindow::renderVoxelSettingsWindow()
{
    if (!showVoxelSettings_) return;
    if (!voxel3d_) voxel3d_ = std::make_unique<pom2::Voxel3DRenderer>();

    ImGui::SetNextWindowSize(ImVec2(360, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("3D Voxel View", &showVoxelSettings_)) {
        ImGui::End();
        return;
    }

    // Quick enable toggle, mirroring the View-menu item so the panel is usable
    // stand-alone. Greys out the knobs while the 3D view is off.
    ImGui::Checkbox("Enable 3D voxel view", &show3dVoxel_);
    ImGui::SameLine();
    ImGui::TextDisabled("(left-drag orbit · middle-drag pan · wheel zoom)");
    ImGui::Separator();

    ImGui::BeginDisabled(!show3dVoxel_);

    pom2::Voxel3DRenderer& v = *voxel3d_;
    ImGui::SliderFloat("Voxel depth",  &v.voxelDepth, 0.0f, 12.0f, "%.1f cells", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uniform Z-thickness of every cube (in pixel units).");
    ImGui::SliderFloat("Colour pop",   &v.colorShift, 0.0f, 24.0f, "%.1f cells", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("MicroM8 'Z-axis 3D offset': brighter pixels push\n"
                          "toward the viewer for pin-art relief. 0 = flat slab.");
    ImGui::SliderFloat("Cube fill",    &v.cubeFill,   0.2f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Footprint as a fraction of the cell. 1.0 = contiguous\n"
                          "(no gap grid — best against moiré); lower = visible gaps.");
    ImGui::SliderInt  ("Anti-alias",   &v.superSample, 1, 4, "%dx supersample", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("FBO render scale, minify-downsampled. Higher = smoother\n"
                          "edges (kills moiré) but more GPU. 1 = off.");
    ImGui::SliderFloat("Ambient",      &v.ambient,    0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lighting floor so no cube face goes pure black.");

    ImGui::Checkbox("Mono", &v.mono);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("MicroM8 'Voxel Cube Mono' — grey output, relief kept.");
    ImGui::SameLine();
    ImGui::Checkbox("Per-colour depth", &v.perColorDepth);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snap each pixel to the nearest lo-res palette colour and\n"
                          "drive 'Colour pop' from that — discrete, blocky relief\n"
                          "(MicroM8 per-index Z) instead of the smooth luminance field.");

    ImGui::Separator();
    if (ImGui::Button("Reset view")) {
        voxelCam_ = pom2::OrbitCamera{};
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset settings")) {
        pom2::Voxel3DRenderer def;
        v.voxelDepth    = def.voxelDepth;
        v.colorShift    = def.colorShift;
        v.cubeFill      = def.cubeFill;
        v.superSample   = def.superSample;
        v.ambient       = def.ambient;
        v.mono          = def.mono;
        v.perColorDepth = def.perColorDepth;
    }

    ImGui::EndDisabled();
    ImGui::End();
}

// ─── CRT Settings (Composite NTSC mode) ──────────────────────────────────
//
// Eight sliders that drive the OpenEmulator-style shader: standard four
// TV knobs (B/C/S/H), sharpness (chroma bandwidth), persistence (CRT
// afterglow), and two pure post-effects (scanlines + barrel). All
// values are persisted to settings.json under the `ntsc_*` keys so the
// look survives across sessions.
void MainWindow::renderNtscSettingsWindow()
{
    if (!showNtscSettings) return;

    ImGui::SetNextWindowSize(ImVec2(380, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CRT Settings (Composite NTSC)",
                      &showNtscSettings)) {
        ImGui::End();
        return;
    }

    // Master ON/OFF for every CRT effect, full-width at the top of the window.
    // Off bypasses the whole effect stack (the colour pipeline still runs);
    // the controls below grey out so it's clear they have no effect.
    {
        const bool on = crtEffectsEnabled;
        const ImVec4 col = on ? ImVec4(0.16f, 0.52f, 0.22f, 1.0f)
                              : ImVec4(0.55f, 0.18f, 0.18f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(col.x + 0.08f, col.y + 0.08f, col.z + 0.08f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
        if (ImGui::Button(on ? "CRT Effects: ON  (click to disable)"
                             : "CRT Effects: OFF  (click to enable)",
                          ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
            crtEffectsEnabled = !crtEffectsEnabled;
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::Separator();

    ImGui::BeginDisabled(!crtEffectsEnabled);

    if (display->getHiResMode()
            != Apple2Display::HiResMode::ColorCompositeOE
        && display->getHiResMode()
            != Apple2Display::HiResMode::ColorCompositeOECpu) {
        ImGui::TextWrapped(
            "CRT effects are active on this mode. All glass knobs "
            "(brightness, contrast, saturation, hue, sharpness, "
            "persistence, scanlines, barrel, shadow mask) apply. Only PAL "
            "and Sharp text are demod-only and affect just the two "
            "'Composite NTSC (OpenEmulator)' modes.");
        ImGui::Separator();
    }

    if (ntscFx && !ntscFx->available()) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1),
            "Shader unavailable: %s", ntscFx->lastError().c_str());
        ImGui::TextWrapped(
            "POM2 falls back to the standard NTSC LUT framebuffer "
            "for this mode.");
        ImGui::Separator();
    }

    pom2::NtscParams p = ntscFx ? ntscFx->getParams() : pom2::NtscParams{};
    bool changed = false;
    changed |= ImGui::SliderFloat("Brightness",  &p.brightness,  -0.5f, 0.5f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Contrast",    &p.contrast,     0.5f, 1.5f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Saturation",  &p.saturation,   0.0f, 2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Hue",         &p.hue,         -0.5f, 0.5f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Separator();
    changed |= ImGui::SliderFloat("Sharpness",   &p.sharpness,    0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Persistence", &p.persistence,  0.0f, 0.95f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Separator();
    changed |= ImGui::SliderFloat("Scanlines",   &p.scanlines,    0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Barrel",      &p.barrel,       0.0f, 0.30f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

    ImGui::Separator();
    // Shadow mask: combo + strength slider. Procedural — no texture
    // upload, no perf cost when Off.
    static const char* kMaskNames[] = {
        "Off", "Triad (3-stripe)", "Aperture grille (Trinitron)",
        "Dot mask (offset triads)"
    };
    int maskIdx = static_cast<int>(p.shadowMask);
    if (ImGui::Combo("Shadow mask", &maskIdx, kMaskNames,
                     IM_ARRAYSIZE(kMaskNames))) {
        p.shadowMask = static_cast<pom2::NtscParams::ShadowMask>(maskIdx);
        changed = true;
    }
    ImGui::BeginDisabled(p.shadowMask == pom2::NtscParams::ShadowMask::Off);
    changed |= ImGui::SliderFloat("Mask strength",
                                  &p.shadowMaskStrength, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    // Post-glass re-brighten — compensates the dimming from scanlines + mask.
    changed |= ImGui::SliderFloat("Luminance gain", &p.luminanceGain, 1.0f, 2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    // Vignette / center-lighting — 1.0 = flat (OpenEmulator default), lower
    // darkens the edges.
    changed |= ImGui::SliderFloat("Center lighting", &p.centerLighting, 0.5f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    // Phosphor response curve (CRT gamma) — 1.0 = flat (off), >1 deepens
    // shadows, <1 lifts them. Pairs with Persistence (the temporal half of the
    // phosphor model). Applies to every mode when "CRT effects on all modes"
    // is on, and always in the OE composite path.
    changed |= ImGui::SliderFloat("Phosphor curve (gamma)",
                                  &p.phosphorGamma, 0.6f, 2.6f, "%.3f", ImGuiSliderFlags_AlwaysClamp);

    ImGui::Separator();
    // PAL composite — line-phase alternation. Off by default (POM2
    // ships with the NTSC look most users associate with the Apple II).
    changed |= ImGui::Checkbox("PAL composite (line-phase alternation)",
                               &p.palMode);
    // Sharp-text bypass: keep glyphs crisp in TEXT mode by skipping
    // the shader for the whole text screen. HGR/DHGR/lo-res still run
    // through the demodulator.
    changed |= ImGui::Checkbox("Sharp text (bypass shader in TEXT mode)",
                               &p.textSharp);

    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults")) {
        p = pom2::NtscParams{};
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Saved to ntsc_* keys");

    ImGui::EndDisabled();

    if (changed) {
        if (!ntscFx) ntscFx = std::make_unique<pom2::NtscPostProcessor>();
        ntscFx->setParams(p);
    }

    ImGui::End();
}

void MainWindow::renderJoystickPanelWindow()
{
    if (!showJoystickPanel) return;

    pom2::JoystickPanel_ImGui::Snapshot snap;
    for (int h = 0; h < JoystickInput::kHostCount; ++h) {
        const auto& d = joystick->deviceState(h);
        if (!d.present) continue;
        pom2::JoystickPanel_ImGui::HostDevice hd;
        hd.index   = h;
        hd.name    = d.name;
        hd.axis    = d.axis;
        hd.buttons = d.buttons;
        snap.hosts.push_back(std::move(hd));
    }
    const auto& cf = joystick->binding();
    snap.hostIdx    = cf.hostIdx;
    snap.deadzone   = cf.deadzone;
    snap.invert     = cf.invert;
    snap.squareGate = cf.squareGate;
    for (int i = 0; i < 4; ++i) snap.appleIIPaddle[i] = joystick->paddleValue(i);
    for (int i = 0; i < 3; ++i) snap.appleIIButton[i] = joystick->buttonDown(i);

    auto result = joystickPanel->render("Joystick", showJoystickPanel, snap);
    if (result.changed) {
        auto& bind = joystick->binding();
        bind.hostIdx    = result.hostIdx;
        bind.deadzone   = result.deadzone;
        bind.invert     = result.invert;
        bind.squareGate = result.squareGate;
        if (settings) settings->setBool("joystick_square_gate", bind.squareGate);
    }
}

// ─── Mouse Inspector ─────────────────────────────────────────────────────
//
// Diagnostic panel for tuning Apple II Mouse Card alignment. Live readout
// of: host cursor (window coords + widget-local + in-widget fraction),
// Apple II Screen widget rect + per-axis logical→host scale, MouseCard's
// 8-bit running counter + sub-pixel accumulator, AppleWin HLE firmware
// state (clamp window, current iX/iY, MOUSE_READ snapshot, mode/state
// bits, PIA port latches, last command), and the AppleMouse firmware
// screen holes for the active slot. Optional CSV log at ~30 Hz so a
// session of cursor motion can be replayed offline.

void MainWindow::renderMouseInspectorWindow()
{
    if (!showMouseInspector) return;
    ImGui::SetNextWindowPos (ImVec2(40, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mouse Inspector", &showMouseInspector)) {
        ImGui::End();
        return;
    }

    const float widgetW = screenRectMax.x - screenRectMin.x;
    const float widgetH = screenRectMax.y - screenRectMin.y;
    const double hostLocalX = lastMouseHostX - double(screenRectMin.x);
    const double hostLocalY = lastMouseHostY - double(screenRectMin.y);
    const bool hostInside =
        widgetW > 0.0f && widgetH > 0.0f &&
        lastMouseHostX >= double(screenRectMin.x) &&
        lastMouseHostX <= double(screenRectMax.x) &&
        lastMouseHostY >= double(screenRectMin.y) &&
        lastMouseHostY <= double(screenRectMax.y);
    const double fracX = widgetW > 0.0f ? hostLocalX / double(widgetW) : 0.0;
    const double fracY = widgetH > 0.0f ? hostLocalY / double(widgetH) : 0.0;
    const int dispW = display->width();
    const int dispH = display->height();
    // Apple-cursor pixels per host pixel — what onMouseMove uses to scale
    // host deltas to MCU 8-bit counts. Always derived from the constant
    // kWidth/kHeight (the widget is rendered at that aspect, not at the
    // current display resolution — see the comment in onMouseMove).
    const double sxRatio =
        widgetW > 0.0f ? double(Apple2Display::kWidth)  / double(widgetW) : 0.0;
    const double syRatio =
        widgetH > 0.0f ? double(Apple2Display::kHeight) / double(widgetH) : 0.0;

    // ── Host cursor ────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Host cursor", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Window coords : (%.1f, %.1f)", lastMouseHostX, lastMouseHostY);
        ImGui::Text("Widget-local  : (%.1f, %.1f)", hostLocalX, hostLocalY);
        ImGui::Text("Fraction      : (%.3f, %.3f)", fracX, fracY);
        ImGui::Text("Button held   : %s", mouseButtonHeld ? "YES" : "no");
        ImGui::TextColored(
            hostInside ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                       : ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
            "Inside Apple II Screen widget: %s", hostInside ? "YES" : "no");
    }

    // ── Apple II Screen widget rect ───────────────────────────────────
    if (ImGui::CollapsingHeader("Apple II Screen widget",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Rect min      : (%.1f, %.1f)", screenRectMin.x, screenRectMin.y);
        ImGui::Text("Rect max      : (%.1f, %.1f)", screenRectMax.x, screenRectMax.y);
        ImGui::Text("Size          : %.1f x %.1f host px", widgetW, widgetH);
        ImGui::Text("Display res   : %d x %d (kWidth=%d kHeight=%d)",
                    dispW, dispH, Apple2Display::kWidth, Apple2Display::kHeight);
        ImGui::Text("Apple px/host : %.4f x %.4f (used by onMouseMove)",
                    sxRatio, syRatio);
    }

    // ── MouseCard 8-bit running counter (MainWindow side) ─────────────
    if (ImGui::CollapsingHeader("MouseCard input (8-bit counter)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Apple counter : (%3u, %3u)  [0x%02X, 0x%02X]",
                    mouseAppleX, mouseAppleY, mouseAppleX, mouseAppleY);
        ImGui::Text("Sub-pixel acc : (%.3f, %.3f)",
                    mouseSubAppleX, mouseSubAppleY);
        const char* cardName =
            mouseAwCard ? "AppleWin HLE (mouseaw)" :
            mouseCard   ? "MAME-faithful (mouse)" :
                          "(no card plugged)";
        ImGui::Text("Active card   : %s", cardName);
    }

    // ── AppleWin HLE card-internal state ──────────────────────────────
    if (mouseAwCard) {
        if (ImGui::CollapsingHeader("AppleWin HLE — firmware state",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto s = mouseAwCard->debugSnapshot();
            ImGui::Text("Clamp X       : [%d .. %d]", s.iMinX, s.iMaxX);
            ImGui::Text("Clamp Y       : [%d .. %d]", s.iMinY, s.iMaxY);
            ImGui::Text("Cursor iX/iY  : (%d, %d)", s.iX, s.iY);
            ImGui::Text("Read   nX/nY  : (%d, %d)  (last MOUSE_READ snap)",
                        s.nX, s.nY);
            ImGui::Text("Buttons curr  : btn0=%d btn1=%d   prev: btn0=%d btn1=%d",
                        s.bBtn0, s.bBtn1, s.bPrevBtn0, s.bPrevBtn1);
            ImGui::Text("MODE  ($00)   : 0x%02X  on=%d intMove=%d intBtn=%d intVBL=%d",
                        s.byMode,
                        (s.byMode & 0x01) ? 1 : 0,
                        (s.byMode & 0x02) ? 1 : 0,
                        (s.byMode & 0x04) ? 1 : 0,
                        (s.byMode & 0x08) ? 1 : 0);
            ImGui::Text("STATE byte    : 0x%02X  curBtn0=%d curBtn1=%d moved=%d",
                        s.byState,
                        (s.byState & 0x80) ? 1 : 0,
                        (s.byState & 0x10) ? 1 : 0,
                        (s.byState & 0x20) ? 1 : 0);
            const char* cmdName = "(unknown)";
            switch (s.lastCmd & 0xF0) {
                case 0x00: cmdName = "MOUSE_SET";   break;
                case 0x10: cmdName = "MOUSE_READ";  break;
                case 0x20: cmdName = "MOUSE_SERV";  break;
                case 0x30: cmdName = "MOUSE_CLEAR"; break;
                case 0x40: cmdName = "MOUSE_POS";   break;
                case 0x50: cmdName = "MOUSE_INIT";  break;
                case 0x60: cmdName = "MOUSE_CLAMP"; break;
                case 0x70: cmdName = "MOUSE_HOME";  break;
                case 0x90: cmdName = "MOUSE_TIME";  break;
            }
            ImGui::Text("Last cmd byte : 0x%02X (%s)  buffPos=%d dataLen=%d",
                        s.lastCmd, cmdName, s.buffPos, s.dataLen);
            ImGui::Text("PIA latches   : A=0x%02X  B=0x%02X", s.by6821A, s.by6821B);
        }
    } else if (mouseCard) {
        if (ImGui::CollapsingHeader("MAME-faithful — card state",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled(
                "Firmware position lives inside the 68705P3 MCU RAM —");
            ImGui::TextDisabled(
                "use the screen-hole readout below for the cursor state.");
        }
    }

    // ── AppleMouse firmware screen holes (per Apple II Mouse FAQ) ─────
    if (ImGui::CollapsingHeader("Screen holes (AppleMouse firmware)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const int activeSlot =
            mouseAwCard ? mouseAwCard->getSlot() :
            mouseCard   ? mouseCard  ->getSlot() : 0;
        if (activeSlot < 1 || activeSlot > 7) {
            ImGui::TextDisabled("(no mouse card plugged)");
        } else {
            int holeXlo = 0, holeXhi = 0, holeYlo = 0, holeYhi = 0;
            int holeMode = 0, holeStatus = 0;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                Memory& mem = controller->memory();
                holeXlo   = mem.peekMainRam(uint16_t(0x0478 + activeSlot));
                holeXhi   = mem.peekMainRam(uint16_t(0x0578 + activeSlot));
                holeYlo   = mem.peekMainRam(uint16_t(0x04F8 + activeSlot));
                holeYhi   = mem.peekMainRam(uint16_t(0x05F8 + activeSlot));
                holeStatus = mem.peekMainRam(uint16_t(0x0778 + activeSlot));
                holeMode  = mem.peekMainRam(uint16_t(0x07F8 + activeSlot));
            }
            const int holeX = (holeXhi << 8) | holeXlo;
            const int holeY = (holeYhi << 8) | holeYlo;
            ImGui::Text("Slot          : %d", activeSlot);
            ImGui::Text("X = $%04X     : %d  (lo $%04X=0x%02X  hi $%04X=0x%02X)",
                        0x0478 + activeSlot, holeX,
                        0x0478 + activeSlot, holeXlo,
                        0x0578 + activeSlot, holeXhi);
            ImGui::Text("Y = $%04X     : %d  (lo $%04X=0x%02X  hi $%04X=0x%02X)",
                        0x04F8 + activeSlot, holeY,
                        0x04F8 + activeSlot, holeYlo,
                        0x05F8 + activeSlot, holeYhi);
            ImGui::Text("Mode  $%04X   : 0x%02X (bit0=mouseOn=%d)",
                        0x07F8 + activeSlot, holeMode, holeMode & 0x01);
            ImGui::Text("Status $%04X  : 0x%02X (bit7=btnDown bit5=moved)",
                        0x0778 + activeSlot, holeStatus);
        }
    }

    // ── CSV logging ───────────────────────────────────────────────────
    ImGui::Separator();
    const bool logging = mouseInspectorLogStream != nullptr;
    if (!logging) {
        if (ImGui::Button("Start logging to CSV")) {
            mouseInspectorLogPath = "mouse_inspector.csv";
            mouseInspectorLogStream =
                std::make_unique<std::ofstream>(mouseInspectorLogPath);
            if (*mouseInspectorLogStream) {
                *mouseInspectorLogStream
                    << "t_s,hostX,hostY,inside,widgetMinX,widgetMinY,"
                       "widgetW,widgetH,appleCntX,appleCntY,btn,"
                       "awIX,awIY,awMinX,awMaxX,awMinY,awMaxY,"
                       "awMode,awState,holeX,holeY,holeMode\n";
                mouseInspectorLastLogTime = 0.0;
                pom2::log().info("MouseInspector",
                    "Logging to " + mouseInspectorLogPath);
            } else {
                mouseInspectorLogStream.reset();
                pom2::log().warn("MouseInspector",
                    "Cannot open " + mouseInspectorLogPath);
            }
        }
    } else {
        if (ImGui::Button("Stop logging")) {
            mouseInspectorLogStream.reset();
            pom2::log().info("MouseInspector",
                "Stopped logging to " + mouseInspectorLogPath);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("→ %s", mouseInspectorLogPath.c_str());
    }
    ImGui::TextDisabled(
        "CSV row per ~33 ms (panel-driven); flushed after each row.");

    // Rate-limit to ~30 Hz so a 5-minute capture stays small. Use
    // ImGui's frame time (monotonic, decoupled from emulated CPU
    // cycles) — the panel is paced by the UI loop, not the worker.
    if (mouseInspectorLogStream) {
        const double now = ImGui::GetTime();
        if (now - mouseInspectorLastLogTime >= 1.0 / 30.0) {
            mouseInspectorLastLogTime = now;
            int activeSlot =
                mouseAwCard ? mouseAwCard->getSlot() :
                mouseCard   ? mouseCard  ->getSlot() : 0;
            int holeX = 0, holeY = 0, holeMode = 0;
            if (activeSlot >= 1 && activeSlot <= 7) {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                Memory& mem = controller->memory();
                holeX = mem.peekMainRam(uint16_t(0x0478 + activeSlot)) |
                       (mem.peekMainRam(uint16_t(0x0578 + activeSlot)) << 8);
                holeY = mem.peekMainRam(uint16_t(0x04F8 + activeSlot)) |
                       (mem.peekMainRam(uint16_t(0x05F8 + activeSlot)) << 8);
                holeMode = mem.peekMainRam(uint16_t(0x07F8 + activeSlot));
            }
            MouseCardAppleWin::DebugSnapshot s{};
            if (mouseAwCard) s = mouseAwCard->debugSnapshot();
            auto& os = *mouseInspectorLogStream;
            os << now << ','
               << lastMouseHostX << ',' << lastMouseHostY << ','
               << (hostInside ? 1 : 0) << ','
               << screenRectMin.x << ',' << screenRectMin.y << ','
               << widgetW << ',' << widgetH << ','
               << int(mouseAppleX) << ',' << int(mouseAppleY) << ','
               << (mouseButtonHeld ? 1 : 0) << ','
               << s.iX << ',' << s.iY << ','
               << s.iMinX << ',' << s.iMaxX << ','
               << s.iMinY << ',' << s.iMaxY << ','
               << int(s.byMode) << ',' << int(s.byState) << ','
               << holeX << ',' << holeY << ',' << holeMode << '\n';
            os.flush();
        }
    }

    ImGui::End();
}

// ─── Audio Mixer ─────────────────────────────────────────────────────────
//
// Consolidated mixer panel: one row per source (Master, Speaker, Cassette,
// Mockingboard if plugged, Disk 5.25", Disk 3.5" if its sample bank
// loaded). Sliders + mute checkboxes write directly into the underlying
// atomics on each source / on AudioDevice — no UI-side cache, so cross-
// thread read-back stays consistent. Replaces the two sliders that used
// to live in the Status panel.

void MainWindow::renderAudioMixerWindow()
{
    if (!showAudioMixer) return;

    ImGui::SetNextWindowPos (ImVec2(80, 80),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 260), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Mixer", &showAudioMixer)) {
        ImGui::End();
        return;
    }

    auto channelRow = [](const char* label, float& vol, bool& mute,
                         float peak, const char* idSuffix, bool dim) {
        if (dim) ImGui::BeginDisabled();
        ImGui::PushID(idSuffix);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(110.0f);
        ImGui::SetNextItemWidth(160.0f);
        const std::string slid = std::string("##v") + idSuffix;
        ImGui::SliderFloat(slid.c_str(), &vol, 0.0f, 2.0f, "%.2f");
        // Tiny activity meter: lets the user confirm at a glance that
        // the channel is actually producing samples (addresses the
        // "doesn't seem connected to the mixer" feedback). Green for
        // safe levels, yellow approaching clip, red at clip.
        ImGui::SameLine();
        const float clamped = std::min(1.0f, std::max(0.0f, peak));
        ImVec4 col(0.20f, 0.80f, 0.20f, 1.0f);
        if      (clamped >= 0.95f) col = ImVec4(0.90f, 0.20f, 0.20f, 1.0f);
        else if (clamped >= 0.70f) col = ImVec4(0.90f, 0.75f, 0.20f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(clamped, ImVec2(40.0f, 0.0f), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const std::string muteId = std::string("Mute##") + idSuffix;
        ImGui::Checkbox(muteId.c_str(), &mute);
        ImGui::PopID();
        if (dim) ImGui::EndDisabled();
    };

    // ── Master ─────────────────────────────────────────────────────────
    AudioDevice& dev = controller->audio();
    float masterVol = dev.getMasterVolume();
    bool  masterMute = dev.isMasterMuted();
    channelRow("Master", masterVol, masterMute, dev.getMasterPeak(),
               "master", false);
    if (masterVol != dev.getMasterVolume()) dev.setMasterVolume(masterVol);
    if (masterMute != dev.isMasterMuted()) dev.setMasterMuted(masterMute);
    ImGui::Separator();

    // ── Speaker ────────────────────────────────────────────────────────
    SpeakerDevice& spk = controller->speaker();
    float spkVol = spk.getVolume();
    bool  spkMute = spk.isMuted();
    channelRow("Speaker", spkVol, spkMute,
               spk.lastBufferPeak.load(std::memory_order_relaxed),
               "spk", false);
    if (spkVol != spk.getVolume()) spk.setVolume(spkVol);
    if (spkMute != spk.isMuted()) spk.setMuted(spkMute);

    // ── Cassette ───────────────────────────────────────────────────────
    CassetteDevice& tape = controller->cassette();
    float tapeVol = tape.getVolume();
    bool  tapeMute = tape.isMuted();
    channelRow("Cassette", tapeVol, tapeMute,
               tape.lastBufferPeak.load(std::memory_order_relaxed),
               "tape", false);
    if (tapeVol != tape.getVolume()) tape.setVolume(tapeVol);
    if (tapeMute != tape.isMuted()) tape.setMuted(tapeMute);

    // ── Slot audio cards (Mockingboard / Phasor / Echo+) ──────────────
    // Only rows for plugged cards — empty slots stay out of the mixer.
    if (mockingboardCard) {
        float mbVol = mockingboardCard->getVolume();
        bool  mbMute = mockingboardCard->isMuted();
        const std::string lbl = "Mockingbd (S" +
            std::to_string(mockingboardCard->getSlot()) + ")";
        AudioSource* mbSrc = mockingboardCard->audioSource();
        const float mbPeak = mbSrc
            ? mbSrc->lastBufferPeak.load(std::memory_order_relaxed)
            : 0.0f;
        channelRow(lbl.c_str(), mbVol, mbMute, mbPeak, "mb", false);
        if (mbVol != mockingboardCard->getVolume()) mockingboardCard->setVolume(mbVol);
        if (mbMute != mockingboardCard->isMuted()) mockingboardCard->setMuted(mbMute);
    }
    if (phasorCard) {
        float phVol = phasorCard->getVolume();
        bool  phMute = phasorCard->isMuted();
        const std::string lbl = "Phasor (S" +
            std::to_string(phasorCard->getSlot()) + ")";
        AudioSource* phSrc = phasorCard->audioSource();
        const float phPeak = phSrc
            ? phSrc->lastBufferPeak.load(std::memory_order_relaxed)
            : 0.0f;
        channelRow(lbl.c_str(), phVol, phMute, phPeak, "phasor", false);
        if (phVol != phasorCard->getVolume()) phasorCard->setVolume(phVol);
        if (phMute != phasorCard->isMuted()) phasorCard->setMuted(phMute);
    }
    if (echoPlusCard) {
        float epVol = echoPlusCard->getVolume();
        bool  epMute = echoPlusCard->isMuted();
        const std::string lbl = "Echo+ (S" +
            std::to_string(echoPlusCard->getSlot()) + ")";
        AudioSource* epSrc = echoPlusCard->audioSource();
        const float epPeak = epSrc
            ? epSrc->lastBufferPeak.load(std::memory_order_relaxed)
            : 0.0f;
        channelRow(lbl.c_str(), epVol, epMute, epPeak, "echop", false);
        if (epVol != echoPlusCard->getVolume()) echoPlusCard->setVolume(epVol);
        if (epMute != echoPlusCard->isMuted()) echoPlusCard->setMuted(epMute);
    }

    // ── Disk 5.25" ─────────────────────────────────────────────────────
    {
        FloppySoundDevice& fs525 = controller->floppySound525();
        const bool dim = !fs525.isLoaded();
        float vol = fs525.getVolume();
        bool  mute = fs525.isMuted();
        channelRow(dim ? "Disk 5.25\" (samples missing)" : "Disk 5.25\"",
                   vol, mute,
                   fs525.lastBufferPeak.load(std::memory_order_relaxed),
                   "fs525", dim);
        if (!dim) {
            if (vol != fs525.getVolume()) fs525.setVolume(vol);
            if (mute != fs525.isMuted()) fs525.setMuted(mute);
        }
    }

    // ── Disk 3.5" ──────────────────────────────────────────────────────
    {
        FloppySoundDevice& fs35 = controller->floppySound35();
        const bool dim = !fs35.isLoaded();
        float vol = fs35.getVolume();
        bool  mute = fs35.isMuted();
        channelRow(dim ? "Disk 3.5\" (samples missing)" : "Disk 3.5\"",
                   vol, mute,
                   fs35.lastBufferPeak.load(std::memory_order_relaxed),
                   "fs35", dim);
        if (!dim) {
            if (vol != fs35.getVolume()) fs35.setVolume(vol);
            if (mute != fs35.isMuted()) fs35.setMuted(mute);
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Master is post-mix; per-channel knobs are pre-mix.");
    ImGui::TextDisabled("Bars show last-buffer peak with ~100 ms release.");
    ImGui::End();
}

// ─── Le Chat Mauve (slot 7) ──────────────────────────────────────────────

void MainWindow::renderChatMauvePanelWindow()
{
    if (!showChatMauvePanel) return;

    pom2::LeChatMauve_ImGui::Snapshot snap;
    if (chatMauveCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        snap.plugged         = true;
        snap.slot            = chatMauveCard->getSlot();
        snap.mode            = chatMauveCard->currentMode();
        snap.fifoBits        = chatMauveCard->fifoBits();
        snap.eightyCol       = chatMauveCard->eightyCol();
        snap.an3High         = chatMauveCard->an3High();
        snap.invertBit7      = chatMauveCard->invertBit7();
        snap.colorTextEnable = chatMauveCard->colorTextEnabled();
        snap.hgrDuochrome    = chatMauveCard->hgrDuochromeEnabled();
    }

    ImGui::SetNextWindowPos (ImVec2(1095, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  500), ImGuiCond_FirstUseEver);

    auto result = chatMauvePanel->render("Le Chat Mauve###chatMauvePanel",
                                        showChatMauvePanel, snap);

    if (chatMauveCard && result.requestOverride) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        chatMauveCard->overrideMode(result.overrideTo);
    }
    if (chatMauveCard && result.requestReset) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        chatMauveCard->onReset();
    }
    if (chatMauveCard && result.requestInvertBit7) {
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            chatMauveCard->setInvertBit7(result.invertBit7To);
        }
        if (settings) settings->setBool("chatmauve_invert_bit7", result.invertBit7To);
    }
    if (chatMauveCard && result.requestColorTextEnable) {
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            chatMauveCard->setColorTextEnabled(result.colorTextEnableTo);
        }
        if (settings) settings->setBool("chatmauve_color_text", result.colorTextEnableTo);
    }
    if (chatMauveCard && result.requestHgrDuochrome) {
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            chatMauveCard->setHgrDuochromeEnabled(result.hgrDuochromeTo);
        }
        if (settings) settings->setBool("chatmauve_hgr_duochrome", result.hgrDuochromeTo);
    }
}

// ─── Mockingboard live state panel ───────────────────────────────────────
//
// Diagnostic window for the Mockingboard A/C. Shows both 6522 VIAs' T1
// counter / IFR / IER + slot IRQ state, and both AY-3-8910 register
// banks (R0..R15). Primary use case: figuring out why an IRQ-driven
// music driver is silent. Three observable cases:
//
//   1. AY registers all stay 0 — the music handler isn't running at
//      all. Check IFR/IER + irqAsserted on the VIA. If T1 ticks but
//      IRQ never asserts, the IER is wrong or the driver hasn't
//      enabled T1 yet.
//   2. AY registers move every few frames — the driver is running and
//      the AY synth is producing samples; if you hear nothing, look at
//      AudioDevice (volume/mute) or the channel mixer R7.
//   3. AY registers load once and freeze — the install ran but only
//      one IRQ landed. Likely the handler isn't re-arming T1 or the
//      ack path is broken.
//
// The panel takes the controller state mutex for each snapshot and
// reads via the card's existing test/debug accessors
// (`peekViaRegister`, `getAyRegister`, `isIrqAsserted`).
void MainWindow::renderMockingboardPanelWindow()
{
    if (!showMockingboardPanel) return;

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mockingboard (VIA + AY state)",
                      &showMockingboardPanel,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!mockingboardCard) {
        ImGui::TextDisabled("No Mockingboard plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }

    // Snapshot all the per-chip state under one lock so the two VIAs and
    // two AYs are coherent against each other. The card's accessors are
    // const-ish reads — no side effects on the running emulator.
    struct ChipSnap {
        uint8_t t1cl, t1ch, t1ll, t1lh, sr, acr, pcr, ifr, ier;
        uint8_t ay[16];
        uint32_t viaWrites, ayWrites, ayResets;
        uint32_t cmdInactive, cmdRead, cmdWrite, cmdLatch;
    } via[2]{};
    bool slotIrq = false;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        slotIrq = mockingboardCard->isIrqAsserted();
        for (int c = 0; c < 2; ++c) {
            via[c].t1cl = mockingboardCard->peekViaRegister(c, 0x04);
            via[c].t1ch = mockingboardCard->peekViaRegister(c, 0x05);
            via[c].t1ll = mockingboardCard->peekViaRegister(c, 0x06);
            via[c].t1lh = mockingboardCard->peekViaRegister(c, 0x07);
            via[c].sr   = mockingboardCard->peekViaRegister(c, 0x0A);
            via[c].acr  = mockingboardCard->peekViaRegister(c, 0x0B);
            via[c].pcr  = mockingboardCard->peekViaRegister(c, 0x0C);
            via[c].ifr  = mockingboardCard->peekViaRegister(c, 0x0D);
            via[c].ier  = mockingboardCard->peekViaRegister(c, 0x0E);
            for (int r = 0; r < 16; ++r) {
                via[c].ay[r] = mockingboardCard->getAyRegister(c, r);
            }
            via[c].viaWrites = mockingboardCard->getViaWriteCount(c);
            via[c].ayWrites  = mockingboardCard->getAyWriteCount(c);
            via[c].ayResets  = mockingboardCard->getAyResetCount(c);
            via[c].cmdInactive = mockingboardCard->getAyCommandCount(c, 0);
            via[c].cmdRead     = mockingboardCard->getAyCommandCount(c, 1);
            via[c].cmdWrite    = mockingboardCard->getAyCommandCount(c, 2);
            via[c].cmdLatch    = mockingboardCard->getAyCommandCount(c, 3);
        }
    }

    // Slot IRQ indicator and volume readout.
    ImGui::Text("Slot IRQ line: %s", slotIrq ? "ASSERTED (low)" : "released");
    ImGui::SameLine();
    ImGui::TextDisabled(" | Volume: %.2f %s",
                        mockingboardCard->getVolume(),
                        mockingboardCard->isMuted() ? "(MUTED)" : "");
    ImGui::Separator();

    // Two-column display, one per 6522+AY pair.
    if (ImGui::BeginTable("##mb_chips", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("VIA #1 + AY #0 ($Cn00-$Cn0F)");
        ImGui::TableSetupColumn("VIA #2 + AY #1 ($Cn80-$Cn8F)");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 2; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& v = via[c];
            // Telemetry first — these counters tell you instantly
            // whether the music driver is even talking to this VIA.
            // viaWrites grows on every guest STA $CnXX (Mockingboard
            // slot-ROM window); ayWrites grows only when LATCH→WRITE
            // strobes successfully complete and a register lands in
            // the AY. Both staying near zero mid-game = the driver
            // isn't running or its IRQ handler is short-circuiting
            // before the AY phase.
            ImGui::Text("VIA writes : %u", v.viaWrites);
            ImGui::Text("AY writes  : %u  (register-store WRITE strobes)",
                        v.ayWrites);
            ImGui::Text("AY resets  : %u  (!RESET pulses, wipes all regs)",
                        v.ayResets);
            ImGui::TextDisabled("AY cmd:  LATCH=%u WRITE=%u INACT=%u READ=%u",
                                v.cmdLatch, v.cmdWrite, v.cmdInactive,
                                v.cmdRead);
            ImGui::Separator();
            ImGui::Text("T1 ctr  $%02X%02X   latch $%02X%02X",
                        v.t1ch, v.t1cl, v.t1lh, v.t1ll);
            ImGui::Text("ACR=$%02X  PCR=$%02X  SR=$%02X",
                        v.acr, v.pcr, v.sr);
            ImGui::Text("IFR=$%02X  IER=$%02X  T1en=%s",
                        v.ifr, v.ier, (v.ier & 0x40) ? "yes" : "no");
            const bool t1Fired = (v.ifr & 0x40) != 0;
            ImGui::TextColored(t1Fired
                                 ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                                 : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "IFR.T1: %s", t1Fired ? "PENDING" : "clear");
            ImGui::Separator();
            ImGui::TextDisabled("AY-3-8910 registers:");
            // Two columns inside the cell: 8 regs each.
            for (int row = 0; row < 8; ++row) {
                const int r0 = row;
                const int r1 = row + 8;
                ImGui::Text("R%-2d %3u $%02X    R%-2d %3u $%02X",
                            r0, v.ay[r0], v.ay[r0],
                            r1, v.ay[r1], v.ay[r1]);
            }
            // Friendly labels for the regs that decide audibility.
            ImGui::Separator();
            const uint16_t periodA = v.ay[0] | ((v.ay[1] & 0x0F) << 8);
            const uint16_t periodB = v.ay[2] | ((v.ay[3] & 0x0F) << 8);
            const uint16_t periodC = v.ay[4] | ((v.ay[5] & 0x0F) << 8);
            ImGui::Text("Ch A period $%03X  vol $%X", periodA, v.ay[8] & 0x1F);
            ImGui::Text("Ch B period $%03X  vol $%X", periodB, v.ay[9] & 0x1F);
            ImGui::Text("Ch C period $%03X  vol $%X", periodC, v.ay[10] & 0x1F);
            ImGui::Text("Mixer $%02X (tone %c%c%c noise %c%c%c)",
                        v.ay[7],
                        (v.ay[7] & 0x01) ? '.' : 'A',
                        (v.ay[7] & 0x02) ? '.' : 'B',
                        (v.ay[7] & 0x04) ? '.' : 'C',
                        (v.ay[7] & 0x08) ? '.' : 'A',
                        (v.ay[7] & 0x10) ? '.' : 'B',
                        (v.ay[7] & 0x20) ? '.' : 'C');
        }
        ImGui::EndTable();
    }

    // ── Sound II SSI263 section (only if this variant has one) ─────────
    MockingboardCard::Ssi263Snap ssiSnap;
    bool hasSsi = false;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        hasSsi = mockingboardCard->snapshotSsi263(&ssiSnap);
    }
    if (hasSsi) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                           "SSI263 (Sound II — speech @ $Cs40-$Cs44)");
        if (ssiSnap.powerDown) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f),
                               "  CTL=1  →  POWER DOWN (silent)");
        }
        ImGui::TextColored(ssiSnap.aRequest
                             ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                             : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "  A/!R: %s  →  VIA1.IFR.CA1 if PCR.0=0",
                           ssiSnap.aRequest ? "REQUEST" : "running");
        ImGui::Text("  Phoneme: $%02X (%d)  remaining: %d cyc",
                    ssiSnap.currentPhoneme, ssiSnap.currentPhoneme,
                    ssiSnap.phonemeRemainingCycles);
        ImGui::Text("  Writes since reset: %u  |  IRQ enable: %s",
                    ssiSnap.phonemeWriteCount,
                    ssiSnap.irqEnabled ? "yes" : "no");
        ImGui::Text("  Regs: $00=%02X  $01=%02X  $02=%02X  $03=%02X  $04=%02X",
                    ssiSnap.regs[0], ssiSnap.regs[1], ssiSnap.regs[2],
                    ssiSnap.regs[3], ssiSnap.regs[4]);
    }

    ImGui::End();
}

// ─── Phasor ──────────────────────────────────────────────────────────────

void MainWindow::renderPhasorPanelWindow()
{
    if (!showPhasorPanel) return;

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Phasor (mode + 2×VIA + 4×AY)",
                      &showPhasorPanel,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!phasorCard) {
        ImGui::TextDisabled("No Phasor plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }

    // Snapshot everything under one lock so VIAs + AYs + mode are
    // coherent. None of the read accessors mutate emulator state.
    struct ViaSnap {
        uint8_t  t1cl, t1ch, t1ll, t1lh, sr, acr, pcr, ifr, ier;
        uint32_t writes;
    } via[2]{};
    struct AySnap {
        uint8_t  regs[16];
        uint32_t writes, resets;
    } ay[4]{};
    PhasorCard::Mode mode = PhasorCard::PH_Mockingboard;
    int  clockScale = 1;
    bool slotIrq    = false;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        mode       = phasorCard->mode();
        clockScale = phasorCard->clockScale();
        slotIrq    = phasorCard->isIrqAsserted();
        for (int c = 0; c < 2; ++c) {
            via[c].t1cl = phasorCard->peekViaRegister(c, 0x04);
            via[c].t1ch = phasorCard->peekViaRegister(c, 0x05);
            via[c].t1ll = phasorCard->peekViaRegister(c, 0x06);
            via[c].t1lh = phasorCard->peekViaRegister(c, 0x07);
            via[c].sr   = phasorCard->peekViaRegister(c, 0x0A);
            via[c].acr  = phasorCard->peekViaRegister(c, 0x0B);
            via[c].pcr  = phasorCard->peekViaRegister(c, 0x0C);
            via[c].ifr  = phasorCard->peekViaRegister(c, 0x0D);
            via[c].ier  = phasorCard->peekViaRegister(c, 0x0E);
            via[c].writes = phasorCard->getViaWriteCount(c);
        }
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 16; ++r)
                ay[c].regs[r] = phasorCard->getAyRegister(c, r);
            ay[c].writes = phasorCard->getAyWriteCount(c);
            ay[c].resets = phasorCard->getAyResetCount(c);
        }
    }

    // ── Mode banner ─────────────────────────────────────────────────────
    const char* modeLabel =
        (mode == PhasorCard::PH_Phasor)       ? "PHASOR NATIVE"  :
        (mode == PhasorCard::PH_EchoPlus)     ? "ECHO+"          :
        (mode == PhasorCard::PH_Mockingboard) ? "MOCKINGBOARD"   :
                                                "(unknown)";
    const ImVec4 modeColor =
        (mode == PhasorCard::PH_Phasor)       ? ImVec4(0.30f, 0.85f, 0.45f, 1.0f) :
        (mode == PhasorCard::PH_EchoPlus)     ? ImVec4(0.40f, 0.65f, 0.95f, 1.0f) :
                                                ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    ImGui::TextColored(modeColor, "Mode: %s", modeLabel);
    ImGui::SameLine();
    ImGui::TextDisabled("  (clock ×%d, %d AYs active)",
                        clockScale,
                        (mode == PhasorCard::PH_Mockingboard) ? 2 : 4);
    ImGui::Text("Slot IRQ line: %s", slotIrq ? "ASSERTED (low)" : "released");
    ImGui::SameLine();
    ImGui::TextDisabled(" | Volume: %.2f %s",
                        phasorCard->getVolume(),
                        phasorCard->isMuted() ? "(MUTED)" : "");
    {
        // Device-select page for this slot is $C0n0..$C0nF where
        // n = 8 + slot (e.g. slot 4 → $C0C0..$C0CF). The mode soft-
        // switch responds to read OR write of those 16 addresses.
        const int devHi = 0x8 + phasorCard->getSlot();
        ImGui::TextDisabled(
            "Mode soft-switch: read/write $C0%X8 → MB, $C0%XD → Phasor",
            devHi, devHi);
    }
    ImGui::Separator();

    // ── VIA telemetry (2 cols) ─────────────────────────────────────────
    if (ImGui::BeginTable("##ph_vias", 2,
                          ImGuiTableFlags_Borders
                          | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("VIA #1 ($Cs00-$Cs0F) → AY0 / AY1");
        ImGui::TableSetupColumn("VIA #2 ($Cs80-$Cs8F) → AY2 / AY3");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 2; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& v = via[c];
            ImGui::Text("VIA writes : %u", v.writes);
            ImGui::Text("T1 ctr  $%02X%02X   latch $%02X%02X",
                        v.t1ch, v.t1cl, v.t1lh, v.t1ll);
            ImGui::Text("ACR=$%02X  PCR=$%02X  SR=$%02X",
                        v.acr, v.pcr, v.sr);
            ImGui::Text("IFR=$%02X  IER=$%02X  T1en=%s",
                        v.ifr, v.ier, (v.ier & 0x40) ? "yes" : "no");
            const bool t1Fired = (v.ifr & 0x40) != 0;
            ImGui::TextColored(t1Fired
                                 ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                                 : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "IFR.T1: %s", t1Fired ? "PENDING" : "clear");
        }
        ImGui::EndTable();
    }
    ImGui::Separator();

    // ── AY-3-8913 register banks (4 cols) ─────────────────────────────
    if (ImGui::BeginTable("##ph_ays", 4,
                          ImGuiTableFlags_Borders
                          | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("AY0 (VIA1 pri)");
        ImGui::TableSetupColumn("AY1 (VIA1 sec)");
        ImGui::TableSetupColumn("AY2 (VIA2 pri)");
        ImGui::TableSetupColumn("AY3 (VIA2 sec)");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 4; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& a = ay[c];
            // In MB-compat mode the secondary AYs (1, 3) are unreachable
            // — flag the cell so the user understands why the bank
            // stays at zero even with a music driver running.
            const bool unreachableInMb =
                (mode == PhasorCard::PH_Mockingboard) && ((c & 1) != 0);
            if (unreachableInMb) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.4f, 1.0f),
                                   "(MB-compat: silent)");
            }
            ImGui::Text("writes %u", a.writes);
            ImGui::Text("resets %u", a.resets);
            ImGui::Separator();
            // Compact reg dump: 8 rows × 2 regs per cell.
            for (int row = 0; row < 8; ++row) {
                const int r0 = row, r1 = row + 8;
                ImGui::Text("R%-2d $%02X  R%-2d $%02X",
                            r0, a.regs[r0], r1, a.regs[r1]);
            }
            ImGui::Separator();
            const uint16_t periodA = a.regs[0] | ((a.regs[1] & 0x0F) << 8);
            const uint16_t periodB = a.regs[2] | ((a.regs[3] & 0x0F) << 8);
            const uint16_t periodC = a.regs[4] | ((a.regs[5] & 0x0F) << 8);
            ImGui::Text("A $%03X v$%X", periodA, a.regs[8]  & 0x1F);
            ImGui::Text("B $%03X v$%X", periodB, a.regs[9]  & 0x1F);
            ImGui::Text("C $%03X v$%X", periodC, a.regs[10] & 0x1F);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ─── Echo+ ───────────────────────────────────────────────────────────────

void MainWindow::renderEchoPlusPanelWindow()
{
    if (!showEchoPlusPanel) return;

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Echo+ (SSI263 speech)", &showEchoPlusPanel,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!echoPlusCard) {
        ImGui::TextDisabled("No Echo+ plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }

    EchoPlusCard::ChipSnap s;
    bool slotIrq = false;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        s       = echoPlusCard->snapshotChip();
        slotIrq = echoPlusCard->isIrqAsserted();
    }

    // ── Header ────────────────────────────────────────────────────────
    ImGui::Text("Slot %d  |  Volume %.2f %s",
                echoPlusCard->getSlot(),
                echoPlusCard->getVolume(),
                echoPlusCard->isMuted() ? "(MUTED)" : "");
    ImGui::TextColored(slotIrq
                         ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "Slot IRQ: %s", slotIrq ? "ASSERTED (low)" : "released");
    ImGui::Separator();

    // ── Live chip state ───────────────────────────────────────────────
    if (s.powerDown) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f),
                           "CTL=1  →  POWER DOWN (silent)");
    } else {
        ImGui::Text("CTL=0  →  RUN");
    }
    const char* modeStr =
        (s.mode == pom2::Ssi263::MODE_IRQ_DISABLED)                    ? "00 IRQ disabled" :
        (s.mode == pom2::Ssi263::MODE_FRAME_IMMEDIATE_INFLECTION)      ? "01 Frame imm. infl." :
        (s.mode == pom2::Ssi263::MODE_PHONEME_IMMEDIATE_INFLECTION)    ? "10 Phon. imm. infl." :
        (s.mode == pom2::Ssi263::MODE_PHONEME_TRANSITIONED_INFLECTION) ? "11 Phon. trans. infl." :
                                                                          "??";
    ImGui::Text("Mode: %s  |  IRQ enable: %s",
                modeStr, s.irqEnabled ? "yes" : "no");
    ImGui::TextColored(s.aRequest
                         ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "A/!R: %s", s.aRequest ? "REQUEST (phoneme done)"
                                              : "running");
    ImGui::Text("Current phoneme: $%02X (%d)",
                s.currentPhoneme, s.currentPhoneme);
    ImGui::Text("Duration remaining: %d cycles (≈ %.1f ms)",
                s.phonemeRemainingCycles,
                s.phonemeRemainingCycles / 1022.727);
    ImGui::Text("Phoneme writes since reset: %u",
                s.phonemeWriteCount);
    ImGui::Separator();

    // ── Register dump ──────────────────────────────────────────────────
    ImGui::TextDisabled("SSI263 registers ($Cs00-$Cs04):");
    const char* labels[5] = {
        "$00 DURPHON", "$01 INFLECT", "$02 RATEINF",
        "$03 CTTRAMP", "$04 FILFREQ"
    };
    for (int r = 0; r < 5; ++r) {
        ImGui::Text("%s = $%02X (%3u)", labels[r], s.regs[r], s.regs[r]);
    }
    ImGui::Separator();

    // ── Status footer ──────────────────────────────────────────────────
    ImGui::TextWrapped(
        "Audio: live — 62-phoneme PCM blob (ported from AppleWin) "
        "resampled from 22050 Hz to the host rate, scaled by the AMP "
        "register. Power-down (CTL=1) or FILFREQ=$FF squelches output.");

    ImGui::End();
}

// ─── Disk II ─────────────────────────────────────────────────────────────

void MainWindow::updateAutoTurbo()
{
    // Auto-turbo: while a disk is actively streaming, crank the CPU to ~60x
    // emulated so loads don't crawl at 1 MHz. Two activity sources:
    //
    //   • DiskII (5.25"): the motor line. Multi-instance friendly — one card
    //     spinning up is enough to enter turbo; all must be idle to leave it.
    //   • ProDOS hard disk (ProDOSHardDiskCard): no motor line, so the byte-
    //     loop firmware streams blocks at the plain CPU rate and HD games
    //     (e.g. Nox Archaist) load far slower than a 5.25" game that gets the
    //     motor-on turbo. Treat recent HDV data-port activity as the same
    //     "busy" signal; the card decays it over a few frames so a multi-block
    //     load stays in turbo end-to-end, then drops back for gameplay.
    //
    // Called every frame from render() (NOT from renderDiskPanelWindow, which
    // early-returns when its window is hidden — the default).
    bool anyMotorOn = false;
    for (auto* c : diskCards) {
        if (c && c->isMotorOn()) { anyMotorOn = true; break; }
    }
    // Decay + poll EVERY block card (HDV + CFFA can coexist) so a load on
    // either keeps turbo engaged. tickActivityDecay() must run on each card
    // (not short-circuit) so their independent decay counters all advance.
    bool hdvBusy = false;
    const auto blocks = blockCards();
    for (auto* dev : blocks) {
        dev->tickActivityDecay();
        if (dev->isBusy()) hdvBusy = true;
    }
    const bool anyBusy       = anyMotorOn || hdvBusy;
    const bool turboEligible =
        diskTurboWhileMotor && (!diskCards.empty() || !blocks.empty());
    if (turboEligible) {
        if (anyBusy && !diskTurboActive) {
            diskSavedCyclesPerFrame = controller->getCyclesPerFrame();
            controller->setCyclesPerFrame(1'000'000);
            diskTurboActive = true;
        } else if (!anyBusy && diskTurboActive) {
            controller->setCyclesPerFrame(diskSavedCyclesPerFrame);
            diskTurboActive = false;
        }
    } else if (diskTurboActive) {
        controller->setCyclesPerFrame(diskSavedCyclesPerFrame);
        diskTurboActive = false;
    }
}

void MainWindow::renderDiskPanelWindow()
{
    if (!showDiskPanel) return;

    // Disk library is the same for every plugged DiskII (it's the
    // contents of disks_5.4/ on disk). Build it once and share via copy.
    std::vector<pom2::DiskController_ImGui::LibraryEntry> sharedLibrary;
    // Disk library — scan disks_5.4/ recursively for .dsk/.do/.po/.nib/.woz.
    // Sub-folders are honoured so users can shelve their library by
    // format (`disks_5.4/dsk/`, `disks_5.4/woz/`, …) or by collection
    // (`disks_5.4/games/`, `disks_5.4/dev/`, …) without losing the one-click
    // boot. Cheap (a few dirent reads per frame); sorted alphabetically
    // so the list doesn't reshuffle as the OS hands us a different
    // dirent order. `displayName` carries the path relative to the
    // scanned root so two files of the same name in different sub-
    // folders don't collide visually.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "disks_5.4", "../disks_5.4", "../../disks_5.4" };
        for (const char* dir : dirCandidates) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                // Skip dotfiles AND dotdirs (.git, .DS_Store, …). On a
                // dotdir we disable_recursion_pending so we don't walk
                // into it on the next ++.
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".dsk" && ext != ".do" && ext != ".po" &&
                    ext != ".nib" && ext != ".woz") continue;
                pom2::DiskController_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath    = entry.path().string();
                sharedLibrary.push_back(std::move(e));
            }
            break;     // first existing candidate dir wins
        }
        std::sort(sharedLibrary.begin(), sharedLibrary.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    // (Auto-turbo lives in updateAutoTurbo(), called every frame from
    // render() — it must run even when this panel window is hidden, which is
    // the default. See MainWindow::updateAutoTurbo.)

    // ── Render one window per plugged DiskII ──────────────────────────
    // Title carries the slot number so ImGui assigns each card its own
    // window state (position, size, dock). The primary (lowest-slot) card
    // gets the curated default position; subsequent cards cascade slightly
    // down/right so they don't perfectly overlap on first show.
    for (size_t idx = 0; idx < diskCards.size() && idx < diskPanels.size(); ++idx) {
        DiskIICard*                       card  = diskCards[idx];
        pom2::DiskController_ImGui*       panel = diskPanels[idx].get();
        if (!card || !panel) continue;

        pom2::DiskController_ImGui::DriveSnapshot snap;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            snap.bootRomLoaded     = card->hasBootRom();
            snap.diskLoaded        = card->isDiskLoaded();
            snap.motorOn           = card->isMotorOn();
            snap.track             = card->getCurrentTrack();
            snap.halfTrack         = card->getHalfTrack();
            snap.trackPos          = card->getTrackPosition();
            snap.diskPath          = card->getDiskPath();
            snap.lastError         = card->getLastError();
            snap.writeBackEnabled  = card->isWriteBackEnabled();
            snap.hasUnsavedChanges = card->hasUnsavedChanges();
        }
        snap.turboWhileMotor = diskTurboWhileMotor;
        snap.turboActive     = diskTurboActive;
        snap.library         = sharedLibrary;     // shared copy

        const float baseX = 1055.0f, baseY = 30.0f;
        ImGui::SetNextWindowPos (
            ImVec2(baseX - static_cast<float>(idx) * 30.0f,
                   baseY + static_cast<float>(idx) * 30.0f),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(705, 960), ImGuiCond_FirstUseEver);

        char title[64];
        std::snprintf(title, sizeof(title),
                      "Disk II (slot %d)", card->getSlot());
        // Only the primary card honours `showDiskPanel` (the menu toggle).
        // Secondary cards share the same toggle for simplicity — the user
        // sees them appear/disappear together. We feed the same flag to
        // each render() call.
        auto result = panel->render(title, showDiskPanel, snap);

        if (result.turboToggleChanged) {
            diskTurboWhileMotor = result.turboNewValue;
        }
        if (result.writeBackToggleChanged) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            card->setWriteBackEnabled(result.writeBackNewValue);
            tapeStatusMessage = "slot " + std::to_string(card->getSlot()) +
                (result.writeBackNewValue
                    ? ": write-back ENABLED (saves on eject)"
                    : ": write-back disabled");
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        if (result.requestEject) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            card->ejectDisk();
            tapeStatusMessage = "Disk ejected (slot " +
                std::to_string(card->getSlot()) + ")";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        if (result.requestBoot) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            card->seekTrack0();
            const uint16_t pc = static_cast<uint16_t>(
                0xC000 + (card->getSlot() << 8));
            controller->cpu().setProgramCounter(pc);
            controller->setMode(EmulationController::Mode::Running);
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Boot: PC → $%04X", pc);
            tapeStatusMessage = msg;
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        if (!result.requestInsertAndBoot.empty()) {
            const std::string path = result.requestInsertAndBoot;
            bool ok = false;
            std::string err;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = card->insertDisk(path);
                if (ok) card->seekTrack0();
                else    err = card->getLastError();
            }
            if (ok) {
                controller->coldBoot();
                controller->setMode(EmulationController::Mode::Running);
                pom2::log().info("Disk II",
                    "slot " + std::to_string(card->getSlot()) +
                    " Library click → insert + boot: " + path);
                tapeStatusMessage = "Booting: " + path;
            } else {
                tapeStatusMessage = "Boot failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        if (!result.requestInsertOnly.empty()) {
            const std::string path = result.requestInsertOnly;
            bool ok = false;
            std::string err;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = card->insertDisk(path);
                if (!ok) err = card->getLastError();
            }
            if (ok) {
                pom2::log().info("Disk II",
                    "slot " + std::to_string(card->getSlot()) +
                    " Library right-click → insert only: " + path);
                tapeStatusMessage = "Inserted (no boot): " + path;
            } else {
                tapeStatusMessage = "Insert failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 4.0;
        }
    }
}

// ─── Disk Library (unified browser: 5.25 / 3.5 / HDV) ───────────────────

void MainWindow::ejectAllDisks()
{
    // Eject under stateMutex for the things we own directly (DiskII, HDV,
    // SmartPort units). `eject35` re-locks stateMutex itself — call it
    // OUTSIDE the lock to avoid recursive locking on the non-recursive
    // std::mutex (was crashing POM2 when Eject-All was clicked).
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        for (auto* c : diskCards) if (c) c->ejectDisk();
        // Every block-device card (HDV + CFFA may both be plugged).
        for (auto* dev : blockCards()) dev->ejectImage();
        // Every SmartPort card, and every unit inside each.
        bool dirty = false;
        for (auto* sp : smartPortCards()) {
            for (size_t k = 0; k < pom2::SmartPortCard::kMaxUnits; ++k) {
                pom2::SmartPortUnit* u = sp->unit(k);
                if (u && u->isLoaded()) {
                    u->eject();
                    const std::string base =
                        "smartport_slot" + std::to_string(sp->getSlot()) +
                        "_unit" + std::to_string(k);
                    settings->setString(base + "_path", "");
                    dirty = true;
                }
            }
        }
        if (dirty) settings->save();
    }
    controller->eject35(0);
    controller->eject35(1);
    tapeStatusMessage = "Ejected all disks_5.4";
    tapeStatusUntil   = lastFrameTime + 3.0;
}

bool MainWindow::routeMount35(int driveIdx, const std::string& path,
                              std::string& errOut)
{
    // Whenever a SmartPort card is plugged its units own 3.5" media;
    // auto-create a SmartPort35Unit on the target index if it's empty or
    // holds a different kind (HDV). This now includes //c-class: the //c /
    // //c+ built-in SmartPort (slot 5) is the boot path POM2 exposes
    // (block-level — the on-board IWM/Sony GCR boot is unmodelled, see
    // project_iic_smartport_boot). (Promoted from a lambda in
    // renderDiskLibraryWindow so the CLI insert+boot path shares it.)
    if (smartPortCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        pom2::SmartPortUnit* u =
            smartPortCard->unit(static_cast<size_t>(driveIdx));
        if (!u || u->kindKey() != pom2::SmartPort35Unit::kKindKey) {
            smartPortCard->setUnit(
                static_cast<size_t>(driveIdx),
                std::make_unique<pom2::SmartPort35Unit>());
            u = smartPortCard->unit(static_cast<size_t>(driveIdx));
        }
        if (!u->loadImage(path)) {
            errOut = u->lastError();
            return false;
        }
        const std::string base =
            "smartport_slot" + std::to_string(smartPortCard->getSlot()) +
            "_unit" + std::to_string(driveIdx);
        settings->setString(base + "_type",
            std::string(pom2::SmartPort35Unit::kKindKey));
        settings->setString(base + "_path", path);
        if (!kiosk_) settings->save();   // kiosk is read-only: never touch state.cfg
        return true;
    }
    // //c+ on-board path.
    if (!controller->mount35(driveIdx, path)) {
        const auto& img = (driveIdx == 0)
            ? controller->disk35Internal()
            : controller->disk35External();
        errOut = img.lastError();
        return false;
    }
    return true;
}

bool MainWindow::routeMountHdv(const std::string& path, int& bootSlotOut,
                               std::string& errOut)
{
    // On //c-class the cffa/hdv slot cards are ROM-masked by the forced
    // INTCXROM and can't boot ($Cn00 reads internal ROM, not the card) —
    // the only bootable block device is the on-board SmartPort (slot 5),
    // so skip the dedicated-HDV-card branch there and route HDV to the
    // SmartPort unit. See project_iic_smartport_boot.
    const bool iicClass =
        (activeProfile == pom2::SystemProfile::AppleIIc ||
         activeProfile == pom2::SystemProfile::AppleIIcPlus ||
         activeProfile == pom2::SystemProfile::AppleIIcPAL);
    // Prefer a dedicated HDV-class card — the MAME-faithful CffaCard if
    // plugged, else the synthetic ProDOSHardDiskCard; else route to a
    // SmartPort card's unit 0 (auto-creating a SmartPortHdvUnit). Promoted
    // from a lambda in renderDiskLibraryWindow.
    if (!iicClass) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (!dev->loadImage(path)) {
                errOut = dev->getLastError();
                hdvStatus = "no image mounted";
                return false;
            }
            hdvPath = path;
            hdvStatus = "loaded: " + path;
            bootSlotOut = dev->getSlot();
            return true;
        }
    }
    if (smartPortCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        pom2::SmartPortUnit* u = smartPortCard->unit(0);
        if (!u || u->kindKey() != pom2::SmartPortHdvUnit::kKindKey) {
            smartPortCard->setUnit(
                0, std::make_unique<pom2::SmartPortHdvUnit>());
            u = smartPortCard->unit(0);
        }
        if (!u->loadImage(path)) {
            errOut = u->lastError();
            return false;
        }
        const std::string base =
            "smartport_slot" + std::to_string(smartPortCard->getSlot()) +
            "_unit0";
        settings->setString(base + "_type",
            std::string(pom2::SmartPortHdvUnit::kKindKey));
        settings->setString(base + "_path", path);
        if (!kiosk_) settings->save();   // kiosk is read-only: never touch state.cfg
        bootSlotOut = smartPortCard->getSlot();
        return true;
    }
    errOut = "no HDV or SmartPort card plugged";
    return false;
}

pom2::ProDOSBlockCard* MainWindow::hdvDevice() const
{
    if (cffaCard) return static_cast<pom2::ProDOSBlockCard*>(cffaCard);
    if (hdvCard)  return static_cast<pom2::ProDOSBlockCard*>(hdvCard);
    return nullptr;
}

std::vector<pom2::ProDOSBlockCard*> MainWindow::blockCards() const
{
    // Walk the bus (slots 1..7) and cross-cast each plugged peripheral to
    // the ProDOSBlockCard mix-in. Both implementers (ProDOSHardDiskCard,
    // CffaCard) inherit SlotPeripheral *and* ProDOSBlockCard, so the
    // dynamic_cast side-cast succeeds for exactly those and yields nullptr
    // for everything else. Slot order is ascending, matching the "lowest
    // slot is primary" convention used for diskCard/hdvCard.
    std::vector<pom2::ProDOSBlockCard*> out;
    SlotBus& bus = controller->memory().slotBus();
    for (int s = 1; s < SlotBus::kSlotCount; ++s) {
        if (auto* blk = dynamic_cast<pom2::ProDOSBlockCard*>(bus.peripheral(s)))
            out.push_back(blk);
    }
    return out;
}

std::vector<pom2::SmartPortCard*> MainWindow::smartPortCards() const
{
    std::vector<pom2::SmartPortCard*> out;
    SlotBus& bus = controller->memory().slotBus();
    for (int s = 1; s < SlotBus::kSlotCount; ++s) {
        if (auto* sp = dynamic_cast<pom2::SmartPortCard*>(bus.peripheral(s)))
            out.push_back(sp);
    }
    return out;
}

int MainWindow::ensureHdvCardForBoot()
{
    // On //c-class only the on-board SmartPort (slot 5) is ROM-visible /
    // bootable — cffa/hdv slot cards are masked by the forced INTCXROM, so
    // prefer the SmartPort and never plug a (dead) ProDOSHardDiskCard here.
    // See project_iic_smartport_boot.
    if (activeProfile == pom2::SystemProfile::AppleIIc ||
        activeProfile == pom2::SystemProfile::AppleIIcPlus ||
        activeProfile == pom2::SystemProfile::AppleIIcPAL) {
        if (smartPortCard) return smartPortCard->getSlot();
    }
    if (cffaCard)      return cffaCard->getSlot();
    if (hdvCard)       return hdvCard->getSlot();
    if (smartPortCard) return smartPortCard->getSlot();

    // Neither an HDV nor a SmartPort card is plugged (e.g. a saved config
    // that only has Disk II cards). Plug a ProDOSHardDiskCard into a free
    // slot for THIS session so the CLI/kiosk launcher can still boot the
    // named HDV. Prefer slot 7 (conventional HDV/SmartPort slot), else the
    // highest free slot. Not persisted — the saved config is untouched.
    int slot = -1;
    for (int s = 7; s >= 1; --s) {
        if (!controller->memory().slotBus().isPlugged(s)) { slot = s; break; }
    }
    if (slot < 0) return -1;

    // Plug under the state lock (the slot is empty, so no card destructor
    // races the worker — a held lock is enough; no stop/start needed).
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        auto card = std::make_unique<ProDOSHardDiskCard>(slot);
        hdvCard = card.get();
        controller->memory().slotBus().plug(slot, std::move(card));
        slotCards[slot] = "hdv";
        autoProvisionedHdvSlot_ = slot;   // session-local; ~MainWindow won't persist it
    }
    pom2::log().info("CLI",
        "auto-plugged ProDOS HDV card in slot " + std::to_string(slot) +
        " (saved slot config unchanged)");
    return slot;
}

bool MainWindow::insertAndBootImage(const std::string& path, std::string& errOut)
{
    // Classify by extension + size, route into the matching slot under the
    // active profile/slot config, then cold-boot. Shared by the CLI
    // positional-disk / --kiosk launcher and (potentially) any future
    // single-call boot entry point. Mirrors the Disk Library "insert +
    // boot" buttons but with no UI surface.
    switch (classifyDiskForSlot(path)) {
        case DiskSlotClass::Floppy525: {
            // Prefer the Disk II in the conventional boot slot 6; fall back
            // to the primary (lowest-slot) card. Booting a single floppy
            // from a non-6 slot is unconventional and breaks software that
            // hardcodes slot 6 for its loader — matters when the config has
            // Disk II in several slots (primary = lowest = e.g. slot 5).
            DiskIICard* target = nullptr;
            for (auto* c : diskCards) if (c && c->getSlot() == 6) { target = c; break; }
            if (!target) target = diskCard;
            if (!target) { errOut = "no Disk II card in the current config"; return false; }
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = target->insertDisk(0, path);
                if (ok) target->seekTrack0();
                else    errOut = target->getLastError(0);
            }
            if (!ok) return false;
            controller->bootFromSlot(target->getSlot());
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Sony35: {
            // Without a SmartPort card, routeMount35 falls through to the
            // //c+ on-board Sony hub — a device that only exists on the
            // //c+. On any other machine the image would "mount" into
            // hardware the guest can't see and the cold boot below would
            // land at the BASIC prompt with no error at all.
            if (!smartPortCard &&
                activeProfile != pom2::SystemProfile::AppleIIcPlus) {
                errOut = "no 3.5\" device in this config — add a SmartPort "
                         "3.5\" card in Slot Configuration";
                return false;
            }
            if (!routeMount35(0, path, errOut)) return false;
            // SmartPort card present (incl. //c-class built-in slot 5) →
            // boot it explicitly; otherwise cold-boot (//c+ on-board hub).
            if (smartPortCard) {
                controller->bootFromSlot(smartPortCard->getSlot());
            } else {
                controller->coldBoot();
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Hdv: {
            // Make sure a card exists to host the HDV (auto-plug one if the
            // saved config has none), then route + boot.
            if (ensureHdvCardForBoot() < 0) {
                errOut = "no free slot to plug an HDV card into";
                return false;
            }
            int bootSlot = 0;
            if (!routeMountHdv(path, bootSlot, errOut)) return false;
            controller->bootFromSlot(bootSlot);
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Unknown:
        default:
            errOut = "unrecognised disk image (extension/size): " + path;
            return false;
    }
}

void MainWindow::renderDiskLibraryWindow()
{
    if (!showDiskLibrary) return;

    // Default position: right column of the curated 1568×850 layout,
    // flush against the screen window. 435 px wide × 745 px tall =
    // enough for the 3-tab table + the search/sort header without
    // scroll overflow on a typical 800+ disk library.
    ImGui::SetNextWindowPos (ImVec2(1125, 90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(435,  745), ImGuiCond_FirstUseEver);

    pom2::DiskLibrary_ImGui::CurrentlyMounted mounted;
    // Currently-inserted Disk II images (per plugged card). The library
    // tags rows with `* ` when their path matches any of these — gives
    // the user a visual cue across all plugged cards at once.
    for (auto* c : diskCards) {
        if (!c) continue;
        pom2::DiskLibrary_ImGui::CurrentlyMounted::DiskIICardInfo info;
        info.slot = c->getSlot();
        if (c->isDiskLoaded(0)) {
            info.drive1 = c->getDiskPath(0);
            mounted.diskII.push_back(info.drive1);
        }
        if (c->isDiskLoaded(1)) {
            info.drive2 = c->getDiskPath(1);
            mounted.diskII.push_back(info.drive2);
        }
        mounted.diskIICards.push_back(info);
    }
    // 3.5" mount sources: the //c+ on-board hub OR a slot-plugged
    // SmartPort card's unit 0/1 (one or the other, never both on the
    // same profile). The library marks rows mounted on either, so the
    // user sees the `* ` cue regardless of which path is active.
    mounted.disk35Internal = controller->disk35Internal().isLoaded()
        ? controller->disk35Internal().path() : std::string();
    mounted.disk35External = controller->disk35External().isLoaded()
        ? controller->disk35External().path() : std::string();
    if (smartPortCard) {
        const pom2::SmartPortUnit* u0 = smartPortCard->unit(0);
        const pom2::SmartPortUnit* u1 = smartPortCard->unit(1);
        if (u0 && u0->isLoaded() &&
            u0->kindKey() == pom2::SmartPort35Unit::kKindKey &&
            mounted.disk35Internal.empty()) {
            mounted.disk35Internal = u0->path();
        }
        if (u1 && u1->isLoaded() &&
            u1->kindKey() == pom2::SmartPort35Unit::kKindKey &&
            mounted.disk35External.empty()) {
            mounted.disk35External = u1->path();
        }
    }
    if (pom2::ProDOSBlockCard* dev = hdvDevice(); dev && dev->isImageLoaded()) {
        mounted.hdv = dev->getImagePath();
    } else if (smartPortCard) {
        // SmartPort-routed HDV — show as mounted in the Library so the
        // `* ` marker matches reality regardless of which path holds it.
        const pom2::SmartPortUnit* u = smartPortCard->unit(0);
        if (u && u->isLoaded() &&
            u->kindKey() == pom2::SmartPortHdvUnit::kKindKey) {
            mounted.hdv = u->path();
        }
    }

    const auto r = diskLibrary->render("Disk Library", showDiskLibrary, mounted);

    // ── Eject-all (header-row button, moved here from the toolbar) ─────
    if (r.requestEjectAllDisks) ejectAllDisks();

    // ── 5.25" actions → the DiskII card/drive the right-click menu picked ──
    // request525Slot = -1 means "primary card" (left-click default); a real
    // slot routes to that specific DiskII card. drive 0 = drive 1, 1 = drive 2.
    auto resolve525 = [&](int slot) -> DiskIICard* {
        if (slot < 0) return diskCard;
        for (auto* c : diskCards) if (c && c->getSlot() == slot) return c;
        return diskCard;
    };
    if (!r.request525InsertAndBoot.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        const std::string path = r.request525InsertAndBoot;
        bool ok = false;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (target) {
                ok = target->insertDisk(drive, path);
                if (ok) target->seekTrack0();
                else    err = target->getLastError(drive);
            }
        }
        if (ok && target) {
            // Boot the target card's slot (its boot PROM boots drive 1).
            controller->bootFromSlot(target->getSlot());
            controller->setMode(EmulationController::Mode::Running);
            tapeStatusMessage = "Library: inserted + booted (slot " +
                std::to_string(target->getSlot()) + " drive " +
                std::to_string(drive + 1) + "): " + path;
        } else {
            tapeStatusMessage = "Library: boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.request525InsertOnly.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        if (target && target->insertDisk(drive, r.request525InsertOnly)) {
            tapeStatusMessage = "Library: inserted (slot " +
                std::to_string(target->getSlot()) + " drive " +
                std::to_string(drive + 1) + ", no boot): " +
                r.request525InsertOnly;
        } else {
            tapeStatusMessage = "Library: insert failed: " +
                (target ? target->getLastError(drive) : std::string("no DiskII card"));
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── 3.5" actions ─────────────────────────────────────────────────
    // Routing: on //c+ profile the on-board hub owns 3.5" media; on any
    // other profile with a SmartPort card plugged, the card's units do.
    // The Library click is explicit user intent to mount 3.5" here, so
    // we auto-create a SmartPort35Unit on the target index if the slot
    // is empty or holds a different kind (HDV) — the user can re-pick
    // the type later from the SmartPort Configuration panel.
    // routeMount35 / routeMountHdv are now member methods (shared with the
    // CLI insert+boot path) — see their definitions above.

    if (!r.request35MountAndBoot.empty()) {
        std::string err;
        if (routeMount35(r.request35BootDrive,
                         r.request35MountAndBoot, err)) {
            // Slot-aware boot: explicit `bootFromSlot(N)` whenever a
            // SmartPort card is plugged — now including the //c-class
            // built-in SmartPort (slot 5). Falls back to `coldBoot()`
            // only when there is no SmartPort card at all.
            if (smartPortCard) {
                controller->bootFromSlot(smartPortCard->getSlot());
            } else {
                controller->coldBoot();
            }
            tapeStatusMessage = "Library: 3.5\" drive "
                + std::string(r.request35BootDrive == 0 ? "1" : "2")
                + " booted: " + r.request35MountAndBoot;
        } else {
            tapeStatusMessage = "Library: 3.5\" boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.request35MountOnly.empty()) {
        std::string err;
        if (routeMount35(r.request35MountDrive,
                         r.request35MountOnly, err)) {
            tapeStatusMessage = "Library: 3.5\" drive "
                + std::string(r.request35MountDrive == 0 ? "1" : "2")
                + " mounted: " + r.request35MountOnly;
        } else {
            tapeStatusMessage = "Library: 3.5\" mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── HDV actions ──────────────────────────────────────────────────
    // Two routing targets: the legacy `ProDOSHardDiskCard` slot (if
    // plugged) OR a SmartPort card's unit 0 (if a SmartPort is plugged
    // but no HDV card is). The Library click is explicit intent to
    // mount HDV; on a SmartPort-only config, auto-create / replace
    // unit 0 with a SmartPortHdvUnit so users don't have to detour
    // through the SmartPort Configuration panel.
    if (!r.requestHdvMountAndBoot.empty()) {
        const std::string path = r.requestHdvMountAndBoot;
        int bootSlot = 0;
        std::string err;
        if (routeMountHdv(path, bootSlot, err)) {
            controller->bootFromSlot(bootSlot);
            tapeStatusMessage = "Library: HDV (slot " +
                std::to_string(bootSlot) + ") booted: " + path;
        } else {
            tapeStatusMessage = "Library: HDV mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.requestHdvMountOnly.empty()) {
        int bootSlot = 0;
        std::string err;
        if (routeMountHdv(r.requestHdvMountOnly, bootSlot, err)) {
            tapeStatusMessage = "Library: HDV mounted: " + r.requestHdvMountOnly;
        } else {
            tapeStatusMessage = "Library: HDV mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── Eject actions ─────────────────────────────────────────────────
    // 5.25": eject from whichever plugged DiskII holds the clicked
    // image. Match by path so multi-instance DiskII setups (the same
    // image plugged into two slots) all clear together.
    if (!r.request525EjectPath.empty()) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        for (auto* c : diskCards) {
            if (!c) continue;
            for (int d = 0; d < DiskIICard::kDriveCount; ++d) {
                if (c->isDiskLoaded(d) &&
                    c->getDiskPath(d) == r.request525EjectPath) {
                    c->ejectDisk(d);
                }
            }
        }
        tapeStatusMessage = "Library: 5.25\" disk ejected";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (r.request35EjectDrive >= 0) {
        controller->eject35(r.request35EjectDrive);
        tapeStatusMessage = "Library: 3.5\" drive "
            + std::string(r.request35EjectDrive == 0 ? "1" : "2") + " ejected";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (r.requestHdvEject) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            dev->ejectImage();
            hdvPath.clear();
            hdvStatus = "no image mounted";
            tapeStatusMessage = "Library: HDV ejected";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
    }
}

// ─── HDV (slot 5) ────────────────────────────────────────────────────────

void MainWindow::renderSmartPortPanelWindow()
{
    if (!showSmartPortPanel) return;

    // Build snapshot from the currently-plugged SmartPort card. When no
    // card is plugged the panel renders a "no card" hint.
    pom2::SmartPort_ImGui::CardSnapshot snap;
    snap.plugged = (smartPortCard != nullptr);
    if (snap.plugged) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        snap.slot = smartPortCard->getSlot();
        for (size_t k = 0; k < snap.units.size(); ++k) {
            const pom2::SmartPortUnit* u = smartPortCard->unit(k);
            auto& us = snap.units[k];
            if (!u) {
                us.kind.clear();
                us.kindLabel.clear();
                continue;
            }
            us.kind             = std::string(u->kindKey());
            us.kindLabel        = std::string(u->kindLabel());
            us.path             = u->path();
            us.lastError        = u->lastError();
            us.blockCount       = u->blockCount();
            us.loaded           = u->isLoaded();
            us.writeProtected   = u->isWriteProtected();
            us.writeBackEnabled = u->isWriteBackEnabled();
        }
    }

    char title[64];
    if (snap.plugged) {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration (slot %d)", snap.slot);
    } else {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration");
    }

    const auto r = smartPortPanel->render(title, showSmartPortPanel, snap);

    if (!snap.plugged) return;

    // Apply per-unit actions under the state mutex. Settings updates
    // (kind / path / writeback) happen here so a restart restores the
    // same layout. Eject + writeback writes pass through the unit's
    // own API; setUnit replaces the entire unit (e.g. type swap).
    const std::string slotKey =
        "smartport_slot" + std::to_string(snap.slot);
    bool dirtySettings = false;
    for (size_t k = 0; k < snap.units.size(); ++k) {
        const auto& a    = r.units[k];
        const std::string base = slotKey + "_unit" + std::to_string(k);

        if (a.clearType || !a.setType.empty()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (a.clearType) {
                smartPortCard->setUnit(k, nullptr);
                settings->setString(base + "_type", "");
                settings->setString(base + "_path", "");
                settings->setBool  (base + "_writeback", false);
                tapeStatusMessage = "SmartPort unit " +
                    std::to_string(k) + ": cleared";
            } else {
                auto unit = pom2::makeSmartPortUnit(a.setType);
                if (unit) {
                    settings->setString(base + "_type", a.setType);
                    settings->setString(base + "_path", "");
                    settings->setBool  (base + "_writeback", false);
                    smartPortCard->setUnit(k, std::move(unit));
                    tapeStatusMessage = "SmartPort unit " +
                        std::to_string(k) + ": type = " + a.setType;
                } else {
                    tapeStatusMessage = "SmartPort unit " +
                        std::to_string(k) + ": unknown type '" +
                        a.setType + "'";
                }
            }
            dirtySettings = true;
            tapeStatusUntil = lastFrameTime + 3.0;
            // Type change drops any previously-loaded media in this
            // unit — skip the rest of the actions for this row.
            continue;
        }

        pom2::SmartPortUnit* u = smartPortCard->unit(k);
        if (!u) continue;

        if (a.writeBackChanged) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            u->setWriteBackEnabled(a.writeBackOn);
            settings->setBool(base + "_writeback", a.writeBackOn);
            dirtySettings = true;
            tapeStatusMessage = "SmartPort unit " + std::to_string(k) +
                ": write-back " + (a.writeBackOn ? "ON" : "OFF");
            tapeStatusUntil = lastFrameTime + 3.0;
        }

        if (!a.mountPath.empty()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (u->loadImage(a.mountPath)) {
                settings->setString(base + "_path", a.mountPath);
                dirtySettings = true;
                tapeStatusMessage = "SmartPort unit " + std::to_string(k) +
                    ": mounted " + a.mountPath;
            } else {
                tapeStatusMessage = "SmartPort unit " + std::to_string(k) +
                    ": mount failed: " + u->lastError();
            }
            tapeStatusUntil = lastFrameTime + 4.0;
        }

        if (a.eject) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            u->eject();
            settings->setString(base + "_path", "");
            dirtySettings = true;
            tapeStatusMessage = "SmartPort unit " + std::to_string(k) +
                ": ejected";
            tapeStatusUntil = lastFrameTime + 3.0;
        }
    }
    if (dirtySettings) settings->save();
}

void MainWindow::renderFloppyEmuWindow()
{
    if (!showFloppyEmu) return;
    namespace fs = std::filesystem;
    using Mode = pom2::FloppyEmuMode;
    const Mode mode = floppyEmu->mode();

    auto baseName = [](const std::string& p) {
        return fs::path(p).filename().string();
    };
    auto human = [](uint64_t b) -> std::string {
        if (b == 0)              return std::string();
        if (b >= 1024 * 1024)    return std::to_string(b / (1024 * 1024)) + "M";
        if (b >= 1024)           return std::to_string(b / 1024) + "K";
        return std::to_string(b) + "B";
    };
    // Live-plug a SmartPort (Liron-class) card into a free slot, mirroring
    // ensureHdvCardForBoot — the empty slot means no card destructor races
    // the worker, so a held lock suffices (no stop/start). Returns slot/-1.
    auto ensureSmartPort = [&]() -> int {
        if (smartPortCard) return smartPortCard->getSlot();
        SlotBus& bus = controller->memory().slotBus();
        int slot = (!bus.isPlugged(5)) ? 5 : -1;
        if (slot < 0)
            for (int s = 7; s >= 1; --s)
                if (!bus.isPlugged(s)) { slot = s; break; }
        if (slot < 0) return -1;
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        auto card = std::make_unique<pom2::SmartPortCard>(slot);
        card->setFloppySound(&controller->floppySound35());
        if (!pom2::profileConfig(activeProfile).noPhysicalSlots) {
            const std::string r = pom2::findResource("roms/liron.rom");
            if (!r.empty()) card->loadLironRom(r);
        }
        smartPortCard = card.get();
        controller->memory().slotBus().plug(slot, std::move(card));
        slotCards[slot] = "smartport35";
        autoProvisionedSmartPortSlot_ = slot;   // session-local; not persisted
        pom2::log().info("FloppyEmu",
            "auto-plugged SmartPort card in slot " + std::to_string(slot));
        return slot;
    };
    auto controllerReady = [&](Mode m) -> bool {
        switch (m) {
            case Mode::Disk525:   return diskCard != nullptr;
            case Mode::Disk35:
            case Mode::Unidisk35: return smartPortCard != nullptr ||
                                         activeProfile == pom2::SystemProfile::AppleIIcPlus;
            case Mode::SmartportHD: return hdvDevice() != nullptr ||
                                           smartPortCard != nullptr;
        }
        return false;
    };
    auto controllerHint = [&](Mode m) -> std::string {
        switch (m) {
            case Mode::Disk525:
                return "No Disk II controller — add 'Disk II' in the Slot Manager.";
            case Mode::Disk35:
            case Mode::Unidisk35:
                return "No SmartPort/Liron controller for 3.5\" media.";
            case Mode::SmartportHD:
                return "No SmartPort or HDV controller for hard-disk media.";
        }
        return std::string();
    };
    auto insertedLabel = [&](Mode m) -> std::string {
        // Snapshot-under-lock: load state / paths are worker-mutable
        // (inserts can come from soft-switch-triggered write-back paths
        // and other panels). Lock released before any rendering.
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        switch (m) {
            case Mode::Disk525:
                return (diskCard && diskCard->isDiskLoaded())
                           ? baseName(diskCard->getDiskPath()) : std::string();
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (smartPortCard) {
                    const pom2::SmartPortUnit* u = smartPortCard->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return controller->disk35Internal().isLoaded()
                           ? baseName(controller->disk35Internal().path())
                           : std::string();
            case Mode::SmartportHD:
                if (pom2::ProDOSBlockCard* dev = hdvDevice())
                    return dev->isImageLoaded() ? baseName(dev->getImagePath())
                                                : std::string();
                if (smartPortCard) {
                    const pom2::SmartPortUnit* u = smartPortCard->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return std::string();
        }
        return std::string();
    };
    auto mountImage = [&](const std::string& path, Mode m) {
        std::string err;
        int bootSlot = 0;
        switch (m) {
            case Mode::Disk525: {
                if (!diskCard) { floppyEmuStatus = controllerHint(m); break; }
                // Same lock as ejectCurrent below (and every other insert
                // path): insertDisk replaces the DiskImage track buffers
                // the worker's LSS streams from on every CPU tick.
                bool ok;
                {
                    std::lock_guard<std::mutex> lk(controller->stateMutex());
                    ok = diskCard->insertDisk(path);
                }
                floppyEmuStatus = ok
                    ? ("Inserted " + baseName(path) +
                       " — reboot the Apple II to boot it.")
                    : ("5.25 mount failed: " + baseName(path));
                break;
            }
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (!controllerReady(m)) ensureSmartPort();
                floppyEmuStatus = routeMount35(0, path, err)
                    ? ("3.5\" mounted: " + baseName(path))
                    : ("3.5\" mount failed: " + err);
                break;
            case Mode::SmartportHD:
                if (!controllerReady(m)) ensureSmartPort();
                floppyEmuStatus = routeMountHdv(path, bootSlot, err)
                    ? ("Smartport mounted: " + baseName(path))
                    : ("Smartport mount failed: " + err);
                break;
        }
    };
    auto ejectCurrent = [&](Mode m) {
        switch (m) {
            case Mode::Disk525: {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (diskCard) diskCard->ejectDisk();
                break;
            }
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (smartPortCard) {
                    std::lock_guard<std::mutex> lk(controller->stateMutex());
                    if (pom2::SmartPortUnit* u = smartPortCard->unit(0)) u->eject();
                } else {
                    controller->eject35(0);  // re-locks the state mutex itself
                }
                break;
            case Mode::SmartportHD: {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (pom2::ProDOSBlockCard* dev = hdvDevice()) dev->ejectImage();
                else if (smartPortCard) {
                    if (pom2::SmartPortUnit* u = smartPortCard->unit(0)) u->eject();
                }
                break;
            }
        }
        floppyEmuStatus = "Ejected";
    };

    // ── Build the snapshot. ──────────────────────────────────────────────
    pom2::FloppyEmu_ImGui::Snapshot snap;
    snap.modeLabel = pom2::FloppyEmuDevice::modeLabel(mode);
    snap.sdPresent = floppyEmu->sdPresent();
    snap.sdRootDisplay = floppyEmu->sdRoot();
    {
        const std::string cur = floppyEmu->currentDir();
        const std::string root = floppyEmu->sdRoot();
        snap.dirLabel = (cur.size() >= root.size() &&
                         cur.compare(0, root.size(), root) == 0)
                            ? cur.substr(root.size()) : cur;
    }
    const auto fav = floppyEmu->favorites();
    snap.favoritesAvailable = fav.present;
    snap.favoritesActive    = floppyEmuFavActive_ && fav.present;
    if (snap.favoritesActive) {
        for (const auto& e : fav.entries) {
            pom2::FloppyEmu_ImGui::Item it;
            it.label = e.name; it.sublabel = human(e.sizeBytes);
            snap.items.push_back(std::move(it));
        }
    } else {
        for (const auto& e : floppyEmu->listing()) {
            pom2::FloppyEmu_ImGui::Item it;
            it.label = e.name; it.isDir = e.isDir; it.isUp = e.isUp;
            it.sublabel = e.isDir ? "DIR" : human(e.sizeBytes);
            snap.items.push_back(std::move(it));
        }
    }
    snap.controllerReady = controllerReady(mode);
    snap.controllerHint  = controllerHint(mode);
    snap.insertedLabel   = insertedLabel(mode);
    snap.statusLine      = floppyEmuStatus;
    for (Mode m : pom2::FloppyEmuDevice::allModes()) {
        snap.modeOptions.push_back(pom2::FloppyEmuDevice::modeLabel(m));
        if (m == mode) snap.currentModeIndex =
            static_cast<int>(snap.modeOptions.size()) - 1;
    }

    const auto r = floppyEmuPanel->render("Floppy Emu (BMOW)", showFloppyEmu, snap);

    // ── Apply actions. ───────────────────────────────────────────────────
    if (r.setModeIndex >= 0) {
        const auto modes = pom2::FloppyEmuDevice::allModes();
        if (r.setModeIndex < static_cast<int>(modes.size())) {
            floppyEmu->setMode(modes[r.setModeIndex]);
            floppyEmuFavActive_ = false;
            floppyEmuStatus = std::string("Mode: ") +
                pom2::FloppyEmuDevice::modeLabel(modes[r.setModeIndex]);
        }
    }
    if (r.toggleFavorites) floppyEmuFavActive_ = !floppyEmuFavActive_;
    if (r.requestConfigureController) {
        if (mode == Mode::Disk525)
            floppyEmuStatus = "Add a Disk II card via the Slot Manager (Apply restarts).";
        else {
            const int s = ensureSmartPort();
            floppyEmuStatus = (s >= 0)
                ? ("Added SmartPort card in slot " + std::to_string(s))
                : "No free slot for a SmartPort card.";
        }
    }
    if (r.requestEject) ejectCurrent(mode);
    if (r.activateIndex >= 0) {
        if (snap.favoritesActive) {
            if (r.activateIndex < static_cast<int>(fav.entries.size()))
                mountImage(fav.entries[r.activateIndex].fullPath, mode);
        } else {
            const auto items = floppyEmu->listing();
            if (r.activateIndex < static_cast<int>(items.size())) {
                const auto& e = items[r.activateIndex];
                if (e.isDir || e.isUp) floppyEmu->enterDir(e);
                else                   mountImage(e.fullPath, mode);
            }
        }
    }
}

void MainWindow::renderHdvPanelWindow()
{
    if (!showHdvPanel) return;

    pom2::HdvController_ImGui::DriveSnapshot snap;
    if (hdvCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        snap.imageLoaded       = hdvCard->isImageLoaded();
        snap.imagePath         = hdvCard->getImagePath();
        snap.blockCount        = hdvCard->getBlockCount();
        snap.writeBackEnabled  = hdvCard->isWriteBackEnabled();
        snap.hasUnsavedChanges = hdvCard->hasUnsavedChanges();
        snap.supportsWriteBack = hdvCard->canWriteBack();
        snap.isSynthVolume     = hdvCard->isSynthVolumeMounted();
    }

    // Library scan — hdv/ for .hdv and .2mg, sorted alphabetically so the
    // list stays stable across frames regardless of dirent order. Plus a
    // synthetic entry for prodos_folder/ if that folder exists (host-folder
    // mount: contents are synthesised into a read-only ProDOS volume on
    // click, see kProDOSHostSentinel below).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "hdv", "../hdv", "../../hdv" };
        for (const char* dir : dirCandidates) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            // Recursive walk so users can shelve images by collection /
            // OS / target machine (`hdv/prodos/`, `hdv/iigs/`, …) and
            // still get one-click mount. See the Disk II library
            // comment above for the rationale and ignore rules.
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".hdv" && ext != ".2mg") continue;
                pom2::HdvController_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath    = entry.path().string();
                snap.library.push_back(std::move(e));
            }
            break;
        }
        std::sort(snap.library.begin(), snap.library.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });

        // Synthetic entry for the host-folder mount.
        const char* prodosCandidates[] = {
            "prodos_folder", "../prodos_folder", "../../prodos_folder"
        };
        std::string prodosDir;
        for (const char* d : prodosCandidates) {
            if (fs::is_directory(d, ec)) { prodosDir = d; break; }
        }
        if (!prodosDir.empty()) {
            std::size_t fileCount = 0;
            for (const auto& e : fs::directory_iterator(prodosDir, ec)) {
                if (e.is_regular_file()) ++fileCount;
            }
            pom2::HdvController_ImGui::LibraryEntry e;
            e.displayName = "[host folder] " + prodosDir + "/  ("
                          + std::to_string(fileCount) + " files)";
            e.fullPath    = std::string(kProDOSHostSentinel) + prodosDir;
            snap.library.push_back(std::move(e));
        }
    }

    // HDV = bottom-left panel in the curated layout (under the Screen).
    ImGui::SetNextWindowPos (ImVec2(5,    600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1040, 390), ImGuiCond_FirstUseEver);
    // Title reflects the actual slot the HDV card is plugged in.
    char hdvTitle[48];
    std::snprintf(hdvTitle, sizeof(hdvTitle), "HDV (slot %d)",
                  hdvCard ? hdvCard->getSlot() : 5);
    auto result = hdvPanel->render(hdvTitle, showHdvPanel, snap);

    if (result.writeBackToggleChanged && hdvCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        hdvCard->setWriteBackEnabled(result.writeBackNewValue);
        tapeStatusMessage = result.writeBackNewValue
            ? "HDV: write-back ENABLED (saves on eject)"
            : "HDV: write-back disabled";
        tapeStatusUntil   = lastFrameTime + 4.0;
    }
    if (result.requestEject && hdvCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        hdvCard->ejectImage();
        hdvStatus = "no image mounted";
        tapeStatusMessage = "HDV ejected";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (result.requestBoot && hdvCard) {
        bootHdvImage();
    }
    if (!result.requestMountAndBoot.empty() && hdvCard) {
        const std::string path = result.requestMountAndBoot;
        const std::string sentinel(kProDOSHostSentinel);

        if (path.rfind(sentinel, 0) == 0) {
            // Host-folder mount: synthesise a read-only ProDOS volume from
            // the folder contents and load it into the slot 5 card. We do
            // NOT auto-boot — block 0 is zero, so the volume isn't
            // bootable. The user boots ProDOS from elsewhere (Disk II or
            // an HDV) and ProDOS then sees /HOST/ as a second drive.
            const std::string hostDir = path.substr(sentinel.size());
            std::vector<std::uint8_t> bytes;
            auto br = pom2::buildVolumeFromFolder(hostDir, "HOST", bytes);
            if (!br.ok) {
                tapeStatusMessage = "ProDOS synth failed: " + br.error;
                tapeStatusUntil   = lastFrameTime + 5.0;
                return;
            }
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = hdvCard->loadImageFromBytes(std::move(bytes),
                                                 std::string("[host folder] ") + hostDir,
                                                 hostDir);
                if (ok) {
                    hdvPath   = path;
                    hdvStatus = std::string("synth from ") + hostDir +
                                " (" + std::to_string(br.filesIncluded) + " files)";
                } else {
                    hdvStatus = "synth load failed";
                }
            }
            if (ok) {
                char msg[200];
                std::snprintf(msg, sizeof(msg),
                    "/HOST/ mounted from %s (%zu files, %zu skipped, %zu blocks). Boot ProDOS from another drive.",
                    hostDir.c_str(), br.filesIncluded, br.filesSkipped, br.totalBlocks);
                tapeStatusMessage = msg;
                pom2::log().info("HDV",
                    std::string("Synthesised volume from ") + hostDir +
                    " (" + std::to_string(br.filesIncluded) + " files, " +
                    std::to_string(br.totalBlocks) + " blocks)");
            } else {
                tapeStatusMessage = "Synth load failed";
            }
            tapeStatusUntil = lastFrameTime + 8.0;
            return;
        }

        // Real .hdv / .2mg / .po file: load under the lock so the card
        // has the right blocks before bootFromSlot wipes RAM and jumps
        // PC = $C(N)00 (where N is the slot the card actually lives in).
        // Two-step lock is safe — the CPU worker only resumes when
        // bootFromSlot flips mode to Running.
        bool ok = false;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            ok = hdvCard->loadImage(path);
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("loaded: ") + path;
            } else {
                err = hdvCard->getLastError();
                hdvStatus = "no image mounted";
            }
        }
        if (ok) {
            controller->bootFromSlot(hdvCard->getSlot());
            pom2::log().info("HDV",
                "slot " + std::to_string(hdvCard->getSlot()) +
                " library click → mount + boot: " + path);
            tapeStatusMessage = "Mounting + booting HDV (slot " +
                std::to_string(hdvCard->getSlot()) + "): " + path;
        } else {
            tapeStatusMessage = "Boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!result.requestMountOnly.empty() && hdvCard) {
        // Right-click "mount only": swap the image without booting.
        // Host-folder sentinel is handled the same as mount-and-boot
        // above (it never auto-boots anyway), so funnel both branches
        // here when no Apple II reset is wanted.
        const std::string path = result.requestMountOnly;
        const std::string sentinel(kProDOSHostSentinel);

        bool ok = false;
        std::string err;
        if (path.rfind(sentinel, 0) == 0) {
            const std::string hostDir = path.substr(sentinel.size());
            std::vector<std::uint8_t> bytes;
            auto br = pom2::buildVolumeFromFolder(hostDir, "HOST", bytes);
            if (!br.ok) {
                tapeStatusMessage = "ProDOS synth failed: " + br.error;
                tapeStatusUntil   = lastFrameTime + 5.0;
                return;
            }
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            ok = hdvCard->loadImageFromBytes(std::move(bytes),
                                             std::string("[host folder] ") + hostDir,
                                             hostDir);
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("synth from ") + hostDir;
            } else {
                err = "synth load failed";
                hdvStatus = err;
            }
        } else {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            ok = hdvCard->loadImage(path);
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("loaded: ") + path;
            } else {
                err = hdvCard->getLastError();
                hdvStatus = "no image mounted";
            }
        }
        if (ok) {
            pom2::log().info("HDV",
                std::string("Library right-click → mount only: ") + path);
            tapeStatusMessage = "Mounted (no boot): " + path;
        } else {
            tapeStatusMessage = "Mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}

void MainWindow::renderDiskFileDialog()
{
    // Find the panel that currently has its insertDialogOpen flag set.
    // With option C (multi-instance DiskII), any of the per-card panels
    // could have triggered the popup via its "Insert .dsk..." button —
    // we route the eventual insertDisk() to the corresponding card.
    pom2::DiskController_ImGui* triggeredPanel = nullptr;
    DiskIICard*                 triggeredCard  = nullptr;
    for (size_t i = 0; i < diskPanels.size() && i < diskCards.size(); ++i) {
        if (diskPanels[i] && diskPanels[i]->insertDialogOpen) {
            triggeredPanel = diskPanels[i].get();
            triggeredCard  = diskCards[i];
            break;
        }
    }
    // Top-level "Insert disk image..." menu (no per-panel context) routes
    // to the primary card by convention.
    if (!triggeredPanel && diskPanel && diskPanel->insertDialogOpen) {
        triggeredPanel = diskPanel;
        triggeredCard  = diskCard;
    }

    if (triggeredPanel) {
        ImGui::OpenPopup("Insert disk image");
        triggeredPanel->insertDialogOpen = false;
        // Remember which card the popup routes to until the user clicks
        // Insert / Cancel. ImGui modal state survives between frames so
        // the pointer needs to survive too.
        diskDialogTargetSlot = triggeredCard ? triggeredCard->getSlot() : -1;
    }
    if (!ImGui::BeginPopupModal("Insert disk image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    // Resolve the target card via the saved slot — the panel pointer may
    // have moved (rare profile-switch races), but the slot number is
    // stable until plugSlotsFromSettings rebuilds.
    DiskIICard*                 popupCard  = nullptr;
    pom2::DiskController_ImGui* popupPanel = nullptr;
    for (size_t i = 0; i < diskCards.size(); ++i) {
        if (diskCards[i] && diskCards[i]->getSlot() == diskDialogTargetSlot) {
            popupCard  = diskCards[i];
            popupPanel = (i < diskPanels.size()) ? diskPanels[i].get() : nullptr;
            break;
        }
    }
    if (!popupPanel) popupPanel = diskPanel;
    if (!popupCard)  popupCard  = diskCard;

    if (popupCard) {
        ImGui::Text("Target: Disk II slot %d", popupCard->getSlot());
    }
    ImGui::TextUnformatted("Path to a 5.25\" image —"
                           " .dsk / .do (DOS 3.3, 143 360 B) or"
                           " .po (ProDOS, 143 360 B) or .nib (raw"
                           " nibble stream, 232 960 B) or .woz"
                           " (bit-cell, copy-protected disks; read-only)."
                           " Write-back is opt-in via the panel checkbox.");
    if (popupPanel) {
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", popupPanel->dialogPath.c_str());
        if (ImGui::InputText("##DiskPath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            popupPanel->dialogPath = buf;
        else
            popupPanel->dialogPath = buf;
    }

    // Quick list of disk images in disks_5.4/ (mirrors the cassette dialog).
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "disks_5.4", "../disks_5.4", "../../disks_5.4" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".dsk" && ext != ".do" && ext != ".po" &&
                ext != ".nib" && ext != ".woz") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()) && popupPanel)
                popupPanel->dialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    if (ImGui::Button("Insert", ImVec2(120, 0))) {
        if (popupCard && popupPanel && !popupPanel->dialogPath.empty()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (popupCard->insertDisk(popupPanel->dialogPath)) {
                tapeStatusMessage = "Disk inserted (slot " +
                    std::to_string(popupCard->getSlot()) + "): " +
                    popupPanel->dialogPath;
            } else {
                tapeStatusMessage = "Insert failed: " + popupCard->getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
        }
        diskDialogTargetSlot = -1;   // popup closed — clear the latched slot
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        diskDialogTargetSlot = -1;   // popup closed — clear the latched slot
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::renderHdvFileDialog()
{
    if (hdvPanel->mountDialogOpen) {
        ImGui::OpenPopup("Mount HDV image");
        hdvPanel->mountDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal("Mount HDV image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("ProDOS block-device image — .hdv (raw blocks)"
                           " or .2mg (with 2IMG header, ProDOS order)");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", hdvPanel->dialogPath.c_str());
    if (ImGui::InputText("##HdvPath", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        hdvPanel->dialogPath = buf;
    else
        hdvPanel->dialogPath = buf;

    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "hdv", "../hdv", "../../hdv" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".hdv" && ext != ".2mg") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()))
                hdvPanel->dialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    const bool canMount = hdvCard && !hdvPanel->dialogPath.empty();
    ImGui::BeginDisabled(!canMount);
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        if (hdvCard->loadImage(hdvPanel->dialogPath)) {
            hdvPath   = hdvPanel->dialogPath;
            hdvStatus = std::string("loaded: ") + hdvPanel->dialogPath;
            tapeStatusMessage = "HDV mounted: " + hdvPanel->dialogPath;
        } else {
            hdvStatus = "no image mounted";
            tapeStatusMessage = "HDV mount failed: " + hdvCard->getLastError();
        }
        tapeStatusUntil = lastFrameTime + 5.0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Mount and Boot", ImVec2(160, 0))) {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            ok = hdvCard->loadImage(hdvPanel->dialogPath);
            if (ok) {
                hdvPath   = hdvPanel->dialogPath;
                hdvStatus = std::string("loaded: ") + hdvPanel->dialogPath;
            } else {
                hdvStatus = "no image mounted";
                tapeStatusMessage = "HDV mount failed: " + hdvCard->getLastError();
                tapeStatusUntil   = lastFrameTime + 5.0;
            }
        }
        if (ok) bootHdvImage();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ─── //c+ SmartPort 3.5" ─────────────────────────────────────────────────

void MainWindow::renderDisk35PanelWindow()
{
    if (!showDisk35Panel) return;

    pom2::Disk35Controller_ImGui::PanelSnapshot snap;
    // 3.5" is "supported" by the //c+ profile (on-board SmartPort + MIG)
    // OR by ANY profile where the user plugged a SmartPort 3.5" card
    // (option C 2026-05-15: //e + Liron-class card). Both paths share
    // the same Disk35Image objects so the panel doesn't have to care
    // which mux is talking.
    snap.supportedByProfile =
        (activeProfile == pom2::SystemProfile::AppleIIcPlus) ||
        (smartPortCard != nullptr);

    // Source selection: a SmartPort slot card on any NON-//c+ profile owns
    // its 3.5" media in its own per-unit `Disk35Image` (SmartPort35Unit) —
    // NOT the //c+ on-board hub's pair. The panel used to always read the
    // hub here, so on a //e + Liron-class card it showed two empty on-board
    // drives while the actual media sat in the card's units (and mount /
    // eject / write-back all hit the wrong object). Read + route from the
    // card's units when that's the source; keep the hub path for //c+.
    const bool useSmartPort35 =
        (smartPortCard != nullptr &&
         activeProfile != pom2::SystemProfile::AppleIIcPlus);
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        if (useSmartPort35) {
            for (int i = 0; i < 2; ++i) {
                auto& s = snap.drives[i];
                auto* u = dynamic_cast<pom2::SmartPort35Unit*>(
                    smartPortCard->unit(static_cast<size_t>(i)));
                if (!u) { s = {}; continue; }  // empty or non-3.5 (HDV) unit
                const pom2::Disk35Image& img = u->image();
                s.diskLoaded        = u->isLoaded();
                s.motorOn           = false;  // block-level card: no flux/motor line
                s.track             = 0;
                s.side1             = false;
                s.writeProtected    = u->isWriteProtected();
                s.diskPath          = u->path();
                s.lastError         = u->lastError();
                s.hasUnsavedChanges = img.hasUnsavedChanges();
                s.writeBackEnabled  = u->isWriteBackEnabled();
            }
        } else {
            const pom2::Sony35Drive* drives[2] = {
                &controller->sony35Internal(), &controller->sony35External(),
            };
            const pom2::Disk35Image* images[2] = {
                &controller->disk35Internal(),  &controller->disk35External(),
            };
            for (int i = 0; i < 2; ++i) {
                auto& s = snap.drives[i];
                s.diskLoaded        = drives[i]->isInserted();
                s.motorOn           = drives[i]->isMotorOn();
                s.track             = drives[i]->track();
                s.side1             = drives[i]->side1();
                s.writeProtected    = drives[i]->isWriteProtected();
                s.diskPath          = images[i]->path();
                s.lastError         = images[i]->lastError();
                s.hasUnsavedChanges = images[i]->hasUnsavedChanges();
                s.writeBackEnabled  = images[i]->isWriteBackEnabled();
            }
        }
    }

    // Library scan — mirrors the Disk II library scan but only picks
    // up files large enough to be 800K (size sniff via filesystem).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const char* dir : { "disks_3.5", "../disks_3.5", "../../disks_3.5",
                                 "disks_5.4",   "../disks_5.4",   "../../disks_5.4" }) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".po" && ext != ".2mg") continue;
                const auto sz = entry.file_size(ec);
                if (ec) continue;
                // 800K raw or 2IMG-wrapped (header + 819 200).
                if (sz != 819200 && sz != 819200 + 64 &&
                    !(sz > 819200 && sz < 819200 + 4096)) continue;
                pom2::Disk35Controller_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath = entry.path().string();
                snap.library.push_back(std::move(e));
            }
            if (!snap.library.empty()) break;
        }
        std::sort(snap.library.begin(), snap.library.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    ImGui::SetNextWindowPos (ImVec2(1055, 30),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(705,  600), ImGuiCond_FirstUseEver);
    // Title reflects where the SmartPort path lives: on-board on //c+,
    // or the explicit slot of the plugged Liron-class card on other
    // profiles. Stable ImGui window-id per slot so the user's position/
    // size choices are remembered per-configuration.
    char disk35Title[64];
    if (smartPortCard) {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (slot %d)", smartPortCard->getSlot());
    } else {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (//c+ on-board)");
    }
    auto result = disk35Panel->render(
        disk35Title, showDisk35Panel, snap);

    for (int d = 0; d < 2; ++d) {
        if (result.requestEject[d]) {
            if (useSmartPort35) {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (auto* u = dynamic_cast<pom2::SmartPort35Unit*>(
                        smartPortCard->unit(static_cast<size_t>(d)))) {
                    u->eject();
                    const std::string base =
                        "smartport_slot" +
                        std::to_string(smartPortCard->getSlot()) +
                        "_unit" + std::to_string(d);
                    settings->setString(base + "_path", "");
                    settings->save();
                }
            } else {
                controller->eject35(d);
            }
            tapeStatusMessage = std::string("3.5\" drive ") +
                (d == 0 ? "1 (internal)" : "2 (external)") + " ejected";
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        // Per-drive write-back toggle. Apply under stateMutex so a save-
        // on-eject race against the worker can't half-flip the flag.
        if (result.requestWriteBackToggle[d]) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (useSmartPort35) {
                if (auto* u = dynamic_cast<pom2::SmartPort35Unit*>(
                        smartPortCard->unit(static_cast<size_t>(d)))) {
                    u->setWriteBackEnabled(result.newWriteBack[d]);
                    const std::string base =
                        "smartport_slot" +
                        std::to_string(smartPortCard->getSlot()) +
                        "_unit" + std::to_string(d);
                    settings->setBool(base + "_writeback", result.newWriteBack[d]);
                    settings->save();
                }
            } else {
                pom2::Disk35Image& img = (d == 0)
                    ? controller->disk35Internal()
                    : controller->disk35External();
                img.setWriteBackEnabled(result.newWriteBack[d]);
            }
            tapeStatusMessage = std::string("3.5\" drive ")
                + (d == 0 ? "1" : "2")
                + (result.newWriteBack[d]
                    ? ": write-back ENABLED (saves on eject)"
                    : ": write-back disabled");
            tapeStatusUntil = lastFrameTime + 4.0;
        }
    }
    if (result.openMountDialog) {
        disk35Panel->mountDialogOpen     = true;
        disk35Panel->mountDialogForDrive = result.openMountDialogForDrive;
        if (disk35Panel->dialogPath.empty()) disk35Panel->dialogPath = "disks_3.5/";
    }
    if (!result.requestMountPath.empty()) {
        // routeMount35 sends the image to the SmartPort card's unit on
        // non-//c+ profiles, or to the on-board hub on //c+ — the same
        // routing the Disk Library + CLI use. Keeps the standalone panel
        // and the library in lock-step.
        std::string err;
        if (routeMount35(result.requestMountDrive, result.requestMountPath, err)) {
            tapeStatusMessage = "3.5\" mounted: " + result.requestMountPath;
        } else {
            tapeStatusMessage = "3.5\" mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    // Library left-click default = mount + cold boot. The //c+ ROM's
    // power-on probe scans SmartPort devices in order and boots the
    // first ready volume, so `coldBoot()` is enough — no need to
    // pre-set PC. On non-//c+ profiles `mount35` succeeds (the image
    // sits idle in Sony35Drive) but no device walker exists to read
    // it, so we still cold-boot but the user sees the Applesoft
    // prompt instead of the new image's loader.
    if (!result.requestInsertAndBoot.empty()) {
        const int d = result.insertAndBootDrive;
        std::string err;
        if (routeMount35(d, result.requestInsertAndBoot, err)) {
            // Prefer an explicit `bootFromSlot(N)` when the SmartPort
            // path is provided by a slot card on a non-//c+ profile —
            // the user picked the slot in Slot Configuration and the
            // PR#N landing should follow that. On //c+ on-board, fall
            // back to `coldBoot()` so the ROM autostart picks up the
            // built-in SmartPort firmware.
            if (useSmartPort35) {
                controller->bootFromSlot(smartPortCard->getSlot());
                tapeStatusMessage = "3.5\" drive "
                    + std::string(d == 0 ? "1" : "2")
                    + " booted (slot " + std::to_string(smartPortCard->getSlot())
                    + "): " + result.requestInsertAndBoot;
            } else {
                controller->coldBoot();
                tapeStatusMessage = "3.5\" drive "
                    + std::string(d == 0 ? "1" : "2")
                    + " booted: " + result.requestInsertAndBoot;
            }
        } else {
            tapeStatusMessage = "3.5\" boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}

void MainWindow::renderDisk35FileDialog()
{
    if (disk35Panel->mountDialogOpen) {
        ImGui::OpenPopup("Mount 3.5\" image");
        disk35Panel->mountDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal("Mount 3.5\" image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Target drive: %s",
                disk35Panel->mountDialogForDrive == 0
                    ? "1 (internal, //c+ on-board)"
                    : "2 (external, SmartPort daisy-chain)");
    ImGui::TextUnformatted("800K Sony 3.5\" image — .po (raw ProDOS blocks,"
                           " 819 200 B) or .2mg (with 2IMG header).");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", disk35Panel->dialogPath.c_str());
    if (ImGui::InputText("##Disk35Path", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        disk35Panel->dialogPath = buf;
    else
        disk35Panel->dialogPath = buf;

    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "disks_3.5", "../disks_3.5", "../../disks_3.5" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".po" && ext != ".2mg") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()))
                disk35Panel->dialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        if (!disk35Panel->dialogPath.empty()) {
            // routeMount35 dispatches to the SmartPort card unit (non-//c+)
            // or the on-board hub (//c+), matching the panel's read source.
            std::string err;
            if (routeMount35(disk35Panel->mountDialogForDrive,
                             disk35Panel->dialogPath, err)) {
                tapeStatusMessage = "3.5\" mounted: " + disk35Panel->dialogPath;
            } else {
                tapeStatusMessage = "3.5\" mount failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 5.0;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MainWindow::ensureAboutImageLoaded()
{
    if (aboutImageTried_) return;
    aboutImageTried_ = true;

    const std::string path = pom2::findResource("pic/Apple_II_plus.jpg");
    if (path.empty()) return;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) return;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    aboutImageTex_ = tex;
    aboutImageW_   = w;
    aboutImageH_   = h;
}

void MainWindow::onFileDrop(int count, const char** paths)
{
    if (count <= 0 || !paths) return;
    // Boot the first image whose extension/size we recognise; skip the
    // rest (a single Apple II has one boot path). insertAndBootImage takes
    // the state lock itself and must run on the UI thread — the GLFW drop
    // callback fires inside glfwPollEvents(), which the render loop drives,
    // so we are on that thread here.
    for (int i = 0; i < count; ++i) {
        if (!paths[i]) continue;
        const std::string path = paths[i];
        if (classifyDiskForSlot(path) == DiskSlotClass::Unknown) continue;
        std::string err;
        if (insertAndBootImage(path, err)) {
            tapeStatusMessage = "Dropped + booted: " +
                std::filesystem::path(path).filename().string();
            pom2::log().info("Drop", "booted dropped image: " + path);
        } else {
            tapeStatusMessage = "Drop failed: " + err;
            pom2::log().warn("Drop", "dropped image rejected: " + err);
        }
        tapeStatusUntil = lastFrameTime + 4.0;
        return;
    }
    // Nothing usable in the drop — tell the user rather than silently
    // ignoring it (the most common case is dropping a ROM or a .zip).
    tapeStatusMessage =
        "Dropped file not a disk image (.dsk/.do/.po/.nib/.woz/.hdv/.2mg)";
    tapeStatusUntil = lastFrameTime + 4.0;
}

void MainWindow::renderWelcomePanelWindow()
{
    if (!showWelcomePanel) return;
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Welcome to POM2###welcomePanel", &showWelcomePanel,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 580.0f);

    // ── No-ROM banner ────────────────────────────────────────────────
    // The single biggest newcomer trip-up: ROMs are user-provided and the
    // machine shows a bare "NO ROM" screen without them. Surface the fix
    // first, in red, only while no ROM is loaded.
    if (!romLoaded_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        ImGui::TextWrapped("No Apple II ROM is loaded yet.");
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "Apple II firmware is copyrighted, so POM2 does not ship it. "
            "Drop your firmware dump into a \"roms/\" folder next to POM2, "
            "then use File → Reload ROM (or relaunch).");
        ImGui::Spacing();

        // Which file the *active* profile wants, and where POM2 looks.
        const auto& cfg = pom2::profileConfig(activeProfile);
        if (!cfg.romProbeOrder.empty()) {
            ImGui::Text("Expected ROM for %.*s:",
                        static_cast<int>(cfg.displayName.size()),
                        cfg.displayName.data());
            for (const auto& cand : cfg.romProbeOrder)
                ImGui::BulletText("%s", cand.c_str());
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("POM2 searches these folders (first hit wins):");
        for (const auto& dir : pom2::resourceSearchDirs())
            ImGui::BulletText("%s", dir.string().c_str());

        ImGui::Spacing();
#ifndef __EMSCRIPTEN__
        if (ImGui::Button("Reload ROM (re-probe folders)")) {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                // Re-resolve from the active profile so dropping the
                // profile-specific dump in is picked up without a relaunch.
                std::string newRom;
                for (const auto& cand : pom2::profileConfig(activeProfile).romProbeOrder) {
                    std::string r = pom2::findResource(cand);
                    if (!r.empty()) { newRom = r; break; }
                }
                if (newRom.empty()) newRom = romPath;  // last-known path
                ok = controller->memory().loadAppleIIRom(newRom.c_str());
                if (ok) romPath = newRom;
            }
            if (ok) {
                controller->hardReset();
                romStatus  = std::string("loaded: ") + romPath;
                romLoaded_ = true;
            }
        }
        ImGui::SameLine();
#endif
        ImGui::TextDisabled("(%s)", romStatus.c_str());
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ── Loading software ─────────────────────────────────────────────
    ImGui::SeparatorText("Loading a disk");
    ImGui::BulletText("Drag a .woz / .dsk / .po / .nib / .hdv / .2mg onto this window.");
    ImGui::BulletText("Or File → Disk Library to browse bundled images.");
    ImGui::BulletText("Or launch from a terminal: POM2 path/to/game.woz");
    ImGui::TextDisabled("POM2 auto-routes each image to Disk II, SmartPort 3.5\" or ProDOS HDV.");

    ImGui::Spacing();
    ImGui::SeparatorText("Suggested media folders");
    ImGui::BulletText("roms/        Apple II firmware dumps");
    ImGui::BulletText("disks_5.4/   5.25\" disk images (.dsk/.woz/.nib)");
    ImGui::BulletText("disks_3.5/   3.5\" disk images (800K)");
    ImGui::BulletText("hdv/         ProDOS hard-disk images (.hdv/.2mg)");

    // ── Keys & signature features ────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Keys");
    ImGui::BulletText("F11  Reset (Ctrl-Reset)        F12  Hard reset");
    ImGui::BulletText("F9   Screenshot                F6   Hold to rewind");
    ImGui::BulletText("Left Alt = Open-Apple          Right Alt = Solid-Apple");
    ImGui::BulletText("Ctrl+V pastes clipboard text as keystrokes");

    ImGui::Spacing();
    ImGui::SeparatorText("Try these");
    ImGui::BulletText("Display → 3D voxel view  — MicroM8-style cube renderer.");
    ImGui::BulletText("Devices → Rewind (F6)    — scrub back through machine state.");
    ImGui::BulletText("Display → CRT Settings   — scanlines, phosphor, NTSC look.");
    ImGui::BulletText("Machine → Profile        — switch between ][ / ][+ / //e / //c / //c+.");

    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close")) showWelcomePanel = false;
    ImGui::SameLine();
    if (ImGui::Button("About POM2...")) { showAbout = true; }
    ImGui::End();
}

void MainWindow::renderAboutDialog()
{
    if (!showAbout) return;
    ensureAboutImageLoaded();
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About POM2", &showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Photo on the left, all text flowed into a column on the right.
        if (aboutImageTex_ && aboutImageW_ > 0 && aboutImageH_ > 0) {
            // Scale to a sensible width in the dialog while preserving the
            // 800×792 aspect of the original photo (≈ 1:1).
            const float displayW = 220.0f;
            const float displayH = displayW *
                static_cast<float>(aboutImageH_) /
                static_cast<float>(aboutImageW_);
            ImGui::BeginGroup();
            ImGui::Image(static_cast<ImTextureID>(
                             static_cast<intptr_t>(aboutImageTex_)),
                         ImVec2(displayW, displayH));
            ImGui::EndGroup();
            ImGui::SameLine();
        }

        ImGui::BeginGroup();
        // Constrain wrapped text to a fixed column so it stays beside the photo.
        const float textColumnW = 380.0f;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textColumnW);

        ImGui::Text("POM2 " POM2_VERSION_STRING);
        ImGui::Text("Apple II / II+ / //e / //c / //c+ emulator");
        ImGui::Text("MOS 6502 / 65C02 / Rockwell / WDC, Dear ImGui frontend");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped(
            "Hardware accuracy comes from verbatim ports of MAME's "
            "device models. Wherever POM2 emulates a chip or a "
            "peripheral, the implementation cites the MAME source "
            "file and line range it follows.");
        ImGui::Spacing();
        ImGui::TextWrapped("Subsystems ported from MAME include:");
        ImGui::BulletText("M6502 / 65C02 dispatch table and timing");
        ImGui::BulletText("IWM (Apple Integrated Woz Machine) for //c+ and 3.5\" SmartPort");
        ImGui::BulletText("AY-3-8910 PSG + 6522 VIA (Mockingboard)");
        ImGui::BulletText("uPD1990AC RTC (ThunderClock+)");
        ImGui::BulletText("M68705P3 + MC6821 PIA (Mouse Card)");
        ImGui::BulletText("WozFDC / Disk II LSS + flux event model");
        ImGui::BulletText("Sony 3.5\" zoned GCR encoder / decoder");
        ImGui::BulletText("RamWorks III aux-slot expander");
        ImGui::BulletText("Floppy mechanical sound samples + cadence");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Thanks to the MAME team for the meticulous reverse "
            "engineering work that makes POM2's parity possible. "
            "MAME is GPL-2.0 / BSD-3-Clause; POM2 is GPL-3.0.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("F11 = Reset (Ctrl-Reset)   F12 = Hard reset");
        ImGui::Text("ESC, arrows, Ctrl-A..Z map straight to the keyboard");

        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        ImGui::Spacing();
        if (ImGui::Button("Close")) showAbout = false;
    }
    ImGui::End();
}

void MainWindow::renderMemoryViewerWindow()
{
    if (!showMemViewer) return;
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Memory viewer", &showMemViewer)) {
        // Hold the state mutex briefly so the snapshot the viewer reads
        // (Memory::data()) is consistent — no torn writes mid-row.
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        memViewer->setCmosMode(
            controller->cpu().getCpuMode() == M6502::CpuMode::CMOS);
        memViewer->render();
    }
    ImGui::End();
}

// ─── Cassette deck ───────────────────────────────────────────────────────

void MainWindow::renderCassetteDeckWindow(float deltaSeconds)
{
    if (!showCassetteDeck) return;

    // Build the deck snapshot under stateMutex — cheap enough that holding
    // the emul lock for the time it takes to copy a dozen scalars is fine.
    pom2::CassetteDeck_ImGui::DeckSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        const CassetteDevice& d = controller->cassette();
        snap.loadedTape          = d.hasLoadedTape();
        snap.recordedTape        = d.hasRecordedTape();
        snap.playbackActive      = d.isPlaybackActive();
        snap.playbackArmed       = d.isPlaybackArmed();
        snap.rewinding           = d.isRewinding();
        snap.audioAvailable      = d.isAudioAvailable();
        snap.playbackPaused      = d.isPlaybackPaused();
        snap.audioStreamMode     = d.isAudioStreamMode();
        snap.queuedAudioSeconds  = d.getQueuedAudioSeconds();
        snap.playbackPositionSec = d.getPlaybackPositionSeconds();
        snap.playbackTotalSec    = d.getPlaybackTotalSeconds();
        snap.loadedTransitions   = d.getLoadedTransitionCount();
        snap.recordedTransitions = d.getRecordedTransitionCount();
        snap.volume              = d.getVolume();
        snap.loadedTapePath      = d.getLoadedTapePath();
        snap.loadInfo            = d.getLoadInfo();
    }

    ImGui::SetNextWindowSize(ImVec2(440, 720), ImGuiCond_FirstUseEver);
    auto result = cassetteDeck->render("Cassette Deck",
                                      showCassetteDeck,
                                      controller.get(),
                                      snap,
                                      deltaSeconds);

    if (!result.statusMessage.empty()) {
        tapeStatusMessage = std::move(result.statusMessage);
        tapeStatusUntil   = lastFrameTime + 4.0;  // show for 4 seconds
    }
}

void MainWindow::renderHgrPaintWindow()
{
    if (!showHgrPaintEditor) return;

    // 64 KB main-RAM (+ IIe aux) snapshot under stateMutex — the editor's
    // per-frame canvas/shadow read source (same idiom as the deck snapshot).
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        const Memory& mem = controller->memory();
        hgrPaintMem_.assign(mem.data(), mem.data() + 0x10000);
        if (mem.isIIE())
            hgrPaintAux_.assign(mem.auxData(), mem.auxData() + 0x10000);
        else
            hgrPaintAux_.clear();
    }

    const float w = hgrpaint::kHiresWidth  * 3.0f + 40.0f;
    const float h = hgrpaint::kHiresHeight * 3.0f + 180.0f;
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("HGR Paint Editor", &showHgrPaintEditor))
        hgrPaintEditor->render(hgrPaintMem_, hgrPaintAux_.empty() ? nullptr
                                                                  : &hgrPaintAux_);
    ImGui::End();
}

void MainWindow::renderHgrSpriteWindow()
{
    if (!showHgrSpriteEditor) return;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        const Memory& mem = controller->memory();
        hgrPaintMem_.assign(mem.data(), mem.data() + 0x10000);
        if (mem.isIIE())
            hgrPaintAux_.assign(mem.auxData(), mem.auxData() + 0x10000);
        else
            hgrPaintAux_.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("HGR Sprite Editor", &showHgrSpriteEditor))
        hgrSpriteEditor->render(hgrPaintMem_, hgrPaintAux_.empty() ? nullptr
                                                                   : &hgrPaintAux_);
    ImGui::End();
}

void MainWindow::driveRewindHold(bool held)
{
    // Edge-detect: hold → step the machine backwards one frame; release →
    // resume live from the rewound point. No-op when recording is off
    // (holdRewind/beginScrub bail out), so it never surprises a non-user.
    if (held)                  rewindPanel_->holdRewind(*controller, 1);
    else if (rewindHeldPrev_)  rewindPanel_->releaseHold(*controller);
    rewindHeldPrev_ = held;
}

void MainWindow::renderRewindWindow(float deltaSeconds)
{
    if (!showRewindBar) return;
    auto result = rewindPanel_->render("Rewind", showRewindBar, *controller, deltaSeconds);
    if (!result.statusMessage.empty()) {
        tapeStatusMessage = std::move(result.statusMessage);
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
}

void MainWindow::renderTapeFileDialogs()
{
    auto pathInput = [](const char* label) {
        // Minimal text-only path widget — POM2 doesn't pull in nativefiledialog.
        // The user types a path; convenience dirs/files can be appended later.
        ImGui::TextUnformatted(label);
    };

    if (cassetteDeck->loadDialogOpen) {
        ImGui::OpenPopup("Load Tape");
        cassetteDeck->loadDialogOpen = false;
    }
    if (ImGui::BeginPopupModal("Load Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Tape file path (.aci / .wav / .mp3 / .ogg / .flac)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", cassetteDeck->dialogPath.c_str());
        if (ImGui::InputText("##LoadPath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            cassetteDeck->dialogPath = buf;
        else
            cassetteDeck->dialogPath = buf;

        // Quick list of cassettes/ directory contents (one click → fill).
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const char* dir : { "cassettes", "../cassettes", "../../cassettes" }) {
            if (!fs::is_directory(dir, ec)) continue;
            ImGui::Separator();
            ImGui::TextDisabled("%s/", dir);
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".aci" && ext != ".wav" && ext != ".mp3" &&
                    ext != ".ogg" && ext != ".flac") continue;
                const std::string name = entry.path().filename().string();
                if (ImGui::Selectable(name.c_str()))
                    cassetteDeck->dialogPath = entry.path().string();
            }
            break;  // first existing candidate dir wins
        }

        ImGui::Separator();
        if (ImGui::Button("Load", ImVec2(120, 0))) {
            if (controller->loadTape(cassetteDeck->dialogPath)) {
                tapeStatusMessage = "Tape loaded: " + cassetteDeck->dialogPath;
            } else {
                tapeStatusMessage = "Load failed: " + controller->cassette().getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (cassetteDeck->saveDialogOpen) {
        ImGui::OpenPopup("Save Tape");
        cassetteDeck->saveDialogOpen = false;
    }
    if (ImGui::BeginPopupModal("Save Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Output file path (.aci or .wav)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", cassetteDeck->dialogPath.c_str());
        if (ImGui::InputText("##SavePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            cassetteDeck->dialogPath = buf;
        else
            cassetteDeck->dialogPath = buf;

        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (controller->saveTape(cassetteDeck->dialogPath)) {
                tapeStatusMessage = "Tape saved: " + cassetteDeck->dialogPath;
            } else {
                tapeStatusMessage = "Save failed: " + controller->cassette().getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // The transient tapeStatusMessage (disk load / boot / eject / screenshot
    // / paste …) is now surfaced in the bottom status bar (renderStatusBar),
    // right-aligned and auto-expiring via tapeStatusUntil — no separate
    // floating overlay.
}

void MainWindow::render()
{
    // Track wallclock between frames so the deck counter / armed pulse /
    // status overlay can age correctly.
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    const double now = std::chrono::duration<double>(clock::now() - t0).count();
    const float deltaSeconds = static_cast<float>(std::max(0.0, now - lastFrameTime));
    lastFrameTime = now;

    pollJoystickAndPushToMemory();

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
        tb.memoryGridVisible  = showMemoryGrid;
        tb.activeProfile      = activeProfile;
        tb.hasPrimaryDiskCard = (diskCard != nullptr);
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
        if (tr.requestMemoryGridToggle) showMemoryGrid = !showMemoryGrid;
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
            // picks it up next frame and routes to `diskCard`.
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
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (controller->memory().loadCharRom(newPath.c_str())) {
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
    renderScreenWindow();
    renderMemoryViewerWindow();
    if (showMemoryBar)  renderMemoryBarWindow();
    if (showMemoryBarH) renderMemoryBarHorizontalWindow();
    if (showMemoryGrid) renderMemoryGridWindow();
    renderCassetteDeckWindow(deltaSeconds);
    renderHgrPaintWindow();
    renderHgrSpriteWindow();
    if (showRewindBar) renderRewindWindow(deltaSeconds);
    renderTapeFileDialogs();
    renderPasteFileDialog();
    renderHdvFileDialog();
    renderDiskPanelWindow();
    renderDiskFileDialog();
    renderDiskLibraryWindow();
    renderDisk35PanelWindow();
    renderDisk35FileDialog();
    renderHdvPanelWindow();
    renderSmartPortPanelWindow();
    renderChatMauvePanelWindow();
    renderMockingboardPanelWindow();
    renderPhasorPanelWindow();
    renderEchoPlusPanelWindow();
    renderSscPanelWindow();
    renderPrinterPanelWindow();
    renderNoSlotClockPanelWindow();
    renderJoystickPanelWindow();
    renderMouseInspectorWindow();
    renderAudioMixerWindow();
    renderNtscSettingsWindow();
    renderVoxelSettingsWindow();
    renderAiControlPanelWindow();
    renderSlotConfigPanel();
    renderFloppyEmuWindow();
    renderAboutDialog();
    renderWelcomePanelWindow();
    renderStatusBar();

    // Hide the host OS cursor whenever the AppleWin HLE firmware is
    // driving a visible emulated cursor AND the host pointer is over the
    // Apple II Screen widget — the two cursors are otherwise stacked and
    // distracting. The ImGui-Glfw backend honours
    // ImGuiMouseCursor_None at EndFrame by calling
    // glfwSetInputMode(GLFW_CURSOR_HIDDEN); on the next frame, leaving
    // it Normal (default ImGuiMouseCursor_Arrow) brings the OS cursor
    // back. The screen rect is fresh because `renderScreenWindow()` ran
    // earlier this frame.
    if (mouseAwCard) {
        const auto s = mouseAwCard->debugSnapshot();
        const bool mouseOn = (s.byMode & 0x01) != 0;
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
