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

// MainWindow_Session — everything the next launch needs to look like this
// one: the mounted media, the video mode, the panels, the audio levels, the
// window geometry.
//
// It lived inline in `~MainWindow()` until 2026-09-01, which was fine on the
// desktop and unreachable in the browser: the WASM build hands its frame loop
// to `emscripten_set_main_loop_arg(..., simulate_infinite_loop=1)`, and that
// call unwinds `main()` WITHOUT running the destructors of its locals — by
// design, so the loop's captured state stays alive for the life of the tab.
// The consequence is that the browser never destroys its MainWindow, so this
// block never ran, so nothing a browser visitor changed was ever written down.
// Persisting to IDBFS (2026-09-01) would have been plumbing to an unreachable
// writer without this extraction.
//
// Everything here is idempotent and safe to run mid-session: the media
// flushes only write media that is dirty, and the settings writes are into
// the in-memory store, which `Settings::save()` commits only when it differs
// from what is already on disk.

#include "MainWindow.h"

#include "AiControlServer.h"
#include "Apple2Display.h"
#include "AudioCoordinator.h"
#include "CharRomCatalog.h"
#include "CffaCard.h"
#include "CrtEffectStack.h"
#include "Disk35Image.h"
#include "EmulationController.h"
#include "FujiNetCard.h"
#include "HgrPaintEditor.h"
#include "FloppyEmuDevice.h"
#include "ImageWriter.h"
#include "Logger.h"
#include "NtscPostProcessor.h"
#include "NetworkCoordinator.h"
#include "PrinterCoordinator.h"
#include "PrinterSoundDevice.h"
#include "Pom2Theme.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SettingsList.h"
#include "StorageCoordinator.h"
#include "SuperSerialCard.h"
#include "SystemProfile.h"
#include "Voxel3DRenderer.h"

#include <array>
#include <mutex>
#include <string>

void MainWindow::persistSession(bool flushMedia)
{
    // Persist the current state so the next launch restores the same
    // mounted disks, video mode, panels, and audio levels.
    // Skip persisting an HDV card that ensureHdvCardForBoot auto-plugged for
    // a one-shot `POM2 <image.hdv>` boot — it's session-local by contract.
    const bool hdvIsAutoProvisioned =
        primaryHdvCard() && primaryHdvCard()->getSlot() == storageCoordinator_->autoProvisionedHdvSlot();
    if (!hdvIsAutoProvisioned && primaryHdvCard() && primaryHdvCard()->isImageLoaded()) {
        // Don't persist the synthesised host-folder volume — the path is
        // a sentinel, not a real file. Re-synthesis happens on click.
        const std::string& p = primaryHdvCard()->getImagePath();
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
    //
    // Flush the 5.25" media FIRST — the 3.5" block below has always done
    // this, and its comment claimed to "mirror the Disk II save-on-shutdown
    // hook", but no such hook existed: quitting with write-back on threw
    // away every sector DOS had written since the last eject. The card's
    // destructor now flushes too (covering profile switches, which rebuild
    // the slot cards without ejecting), but doing it here keeps it ordered
    // before the settings write and inside the same teardown the user can
    // see in the log.
    {
        // ONE flush per quit, and it goes through `flushSlotMedia` — which
        // takes `stateMutex` around the capture and writes the deferred half
        // with it released. This used to call `flushAll` directly and with NO
        // lock at all, a second flush after ~MainWindow's own; the browser
        // heartbeat below then ran that unlocked walk over live cards every
        // ten seconds with the machine running.
        //
        // `flushMedia` is false exactly once: the desktop quit path, which
        // flushed under the lock moments earlier. Everything else — the WASM
        // 10 s heartbeat and its `pagehide` sibling — is a mid-session save
        // and must flush here.
        if (flushMedia) {
            std::string flushError;
            if (!flushSlotMedia(flushError))
                pom2::log().warn("Storage", "session flush: " + flushError);
        }

        // Both drives, and the legacy unsuffixed aliases from the lowest-slot
        // card so an older POM2 build reading this settings.ini still finds
        // the disk. The loop this replaces called isDiskLoaded()/getDiskPath()
        // with their default arguments, so drive 2's path was never written on
        // exit — the last of the five places that mistake was made.
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        const auto snapshot = storageCoordinator_->captureRebuildSnapshot(
            controller->memory().slotBus());
        storageCoordinator_->persistSessionSettings(*settings, snapshot);
    }

    // Persist mounted 3.5" disks across restarts AND flush any firmware-
    // driven write-backs (format / save / etc.) that arrived after the
    // user opted in to write-back. Mirrors the Disk II save-on-shutdown
    // hook so changes survive a hard quit.
    //
    // Two-phase, and under the lock: `saveDirty()` writes 800 KB plus two
    // fsyncs, and this ran with no lock at all against a machine the browser
    // heartbeat leaves RUNNING. Capture with the lock (a memcpy), write with
    // it released, put the dirty flag back if the write failed.
    struct Disk35SessionState {
        bool        loaded = false;
        bool        writeBack = false;
        std::string path;
    };
    std::array<Disk35SessionState, 2> onboard35{};
    std::array<pom2::Disk35Image::PendingWriteBack, 2> pending35{};
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        pom2::Disk35Image* images[2] = { &controller->disk35Internal(),
                                         &controller->disk35External() };
        for (std::size_t i = 0; i < 2; ++i) {
            onboard35[i].loaded    = images[i]->isLoaded();
            onboard35[i].writeBack = images[i]->isWriteBackEnabled();
            if (onboard35[i].loaded) onboard35[i].path = images[i]->path();
            pending35[i] = images[i]->takeWriteBack();
        }
    }
    for (std::size_t i = 0; i < 2; ++i) {
        if (!pending35[i].valid) continue;
        const std::string path = pending35[i].path;
        std::string error;
        if (pom2::Disk35Image::commitWriteBack(std::move(pending35[i]), error))
            continue;
        // A failed flush leaves the file untouched (atomic temp+rename), but
        // the only copy of the session's writes dies with the process — say
        // so, like the Disk II shutdown path does, and put the flag back so a
        // heartbeat save retries.
        pom2::log().warn("Disk35",
                         "session flush failed for " + path + ": " + error);
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        pom2::Disk35Image& img = i == 0 ? controller->disk35Internal()
                                        : controller->disk35External();
        if (img.isLoaded() && img.path() == path) img.restoreDirty();
    }
    // Paths AND the write-back opt-in: only the paths were persisted, so a
    // 3.5" drive the user had opted in came back write-protected after every
    // restart (and silently discarded the next session's writes on eject).
    settings->setString("disk35_path_1",
                        onboard35[0].loaded ? onboard35[0].path
                                            : std::string());
    settings->setString("disk35_path_2",
                        onboard35[1].loaded ? onboard35[1].path
                                            : std::string());
    settings->setBool("disk35_writeback_1", onboard35[0].writeBack);
    settings->setBool("disk35_writeback_2", onboard35[1].writeBack);

    // Same auto-provision guard as `hdv_path` above: a card that
    // ensureHdvCardForBoot plugged for a one-shot drag-drop / CLI boot is
    // session-local, so its write-back flag must not overwrite the one the
    // user configured for their real HDV card. Without the guard, a single
    // dropped .hdv persisted `hdv_writeback = false` and silently disarmed
    // write-back for unrelated media on the next launch.
    if (primaryHdvCard() && !hdvIsAutoProvisioned) {
        settings->setBool("hdv_writeback", primaryHdvCard()->isWriteBackEnabled());
    }

    // CFFA per-slot image + write-back for EVERY plugged CFFA card. `cffa`
    // is multi-instance, so persist each (not just the primary `primaryCffaCard()`),
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
    for (auto* ssc : serialCards()) {
        if (!ssc) continue;
        const std::string sk = "_slot" + std::to_string(ssc->getSlot());
        settings->setBool("ssc_listening" + sk, ssc->isListening());
        settings->setInt ("ssc_port"      + sk, ssc->getPort());
        settings->setBool("ssc_raw_mode"  + sk, ssc->rawMode());
        settings->setBool("ssc_printer_tap" + sk, ssc->printerTap());
    }
    if (primarySerialCard()) {
        settings->setBool("ssc_listening", primarySerialCard()->isListening());
        settings->setInt ("ssc_port",      primarySerialCard()->getPort());
        settings->setBool("ssc_raw_mode",  primarySerialCard()->rawMode());
    }

    // FujiNet relay — transport choice and its parameters, per slot.
    // Resolved from the live bus rather than an alias: the destructor runs
    // after controller->stop(), so this is a UI-thread topology read.
    pom2::FujiNetCard* fujiNet = nullptr;
    for (int s = 1; s < SlotBus::kSlotCount && !fujiNet; ++s)
        fujiNet = dynamic_cast<pom2::FujiNetCard*>(
            controller->memory().slotBus().peripheral(s));
    if (fujiNet) {
        const std::string sk = "_slot" + std::to_string(fujiNet->getSlot());
        const auto& link = fujiNet->transportLink();
        settings->setBool("fujinet_enabled" + sk, link.isRunning());
        settings->setInt ("fujinet_timeout_ms" + sk, link.timeoutMs());
        settings->setString("fujinet_transport" + sk,
                            link.mode() == pom2::FujiNetTransport::Mode::Serial
                                ? "serial" : "tcp");
        settings->setInt   ("fujinet_port" + sk, link.tcpPort());
        settings->setString("fujinet_serial_path" + sk, link.serialPath());
        settings->setInt   ("fujinet_serial_baud" + sk, link.serialBaud());
        settings->setString("fujinet_helper_path" + sk,
                            networkCoordinator_->helperPath());
    }

    // AI control listener — persist enable, port, token, and the panel
    // visibility flag. Re-armed on next launch by the constructor.
    settings->setBool  ("ai_control_enable", aiServer->isRunning());
    settings->setInt   ("ai_control_port",   aiServer->getPort());
    settings->setString("ai_control_token",  aiTokenInput);
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
            if (s == storageCoordinator_->autoProvisionedHdvSlot()) continue;   // session-local auto-plug
            if (s == storageCoordinator_->autoProvisionedSmartPortSlot()) continue;   // idem (Floppy Emu)
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
    savePanelVisibility();
    settings->setString("floppyemu_mode",
                        pom2::FloppyEmuDevice::modeKey(floppyEmu->mode()));
    settings->setString("floppyemu_sd_root", floppyEmu->sdRoot());
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
    settings->setBool  ("rewind_enabled",  controller->rewind().enabled());
    settings->setInt   ("imagewriter_paper",
                        static_cast<int>(imageWriter->paperSize()));
    settings->setInt   ("imagewriter_dpi",    imageWriter->dpi());
    settings->setInt   ("imagewriter_model",
                        static_cast<int>(imageWriter->model()));
    settings->setBool  ("imagewriter_backpressure", printerBackPressure);
    settings->setInt   ("imagewriter_ribbon",
                        static_cast<int>(imageWriter->ribbon()));
    settings->setInt   ("imagewriter_autolf_mode",
                        static_cast<int>(imageWriter->autoFeedMode()));
    settings->setInt   ("imagewriter_speed",
                        static_cast<int>(imageWriter->speed()));
    // No-ops when no Grappler+ is plugged, so the keys keep their previous
    // values rather than being overwritten with a default.
    printerCoordinator_->persistGrappler(*settings, *controller);
    settings->setBool  ("nsclock_enable",  controller->noSlotClock().isEnabled());
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
        settings->setFloat("ntsc_rgb_bandwidth_mhz", p.rgbBandwidthMHz);
        settings->setInt  ("ntsc_shadow_mask", static_cast<int>(p.shadowMask));
        settings->setBool ("ntsc_pal",         p.palMode);
        settings->setBool ("ntsc_text_sharp",  p.textSharp);
    }
    settings->setBool  ("crt_effects_enabled", crtEffectsEnabled);
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
    settings->setString("ui_accent", pom2::accentKey(uiAccent_));
    settings->setFloat ("ui_scale",  uiScale_);
    settings->setBool  ("ui_dock_seeded", dockSeeded_);
    settings->setString("library_favourites", pom2::joinSettingList(libraryFavourites_));
    settings->setString("library_recents",    pom2::joinSettingList(libraryRecents_));
    settings->setBool  ("library_hide_sizedate", libraryHideSizeDate_);
    settings->setBool  ("disk_turbo",      diskTurboWhileMotor);
    // One call for the whole audio block, host controls and slot cards alike.
    // The slot-card half is why it matters: the old code persisted a single
    // `mockingboard_volume` read through the last-plugged alias, so with two
    // Mockingboard variants on the bus one of them silently inherited the
    // other's level on the next launch. Each live card now gets its own
    // per-slot key, and the highest slot of each type still writes the legacy
    // type-wide key so existing state.cfg files keep working.
    audioCoordinator_->persist(*settings,
                               controller->speaker(),
                               controller->cassette(),
                               controller->floppySound525(),
                               controller->floppySound35(),
                               *printerSound);
    settings->setString("char_rom_locale",        pom2::charRomLocaleKey(charRomLocale));

    // Kiosk is a read-only launcher: don't write state.cfg, so the disk it
    // booted (and any HDV card auto-plugged for it by ensureHdvCardForBoot)
    // never leak into the user's saved GUI config. The setString calls
    // above are in-memory only and discarded with `settings` here.
    // Record where the window ended up so the next launch (and any later
    // kiosk round-trip) reopens at the same size and place. Skipped while
    // in kiosk — the live geometry is full-screen, not what we want to
    // restore; the value captured on the way INTO kiosk still stands.
    // NB the geometry itself was captured by main() via
    // captureWindowGeometryNow() while GLFW was still up — measuring here
    // would be too late (see that function).
    if (!settingsReadOnly()) {
        saveWindowGeometryToSettings();
        settings->save();
    }
}
