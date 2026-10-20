// POM2 — GPL-3.0-or-later
#include "MainWindow.h"
#include "Version.h"
#include "SessionFiles.h"
#include "SessionMachine.h"
#include "SessionLoadQueue.h"
#include "SlotConfigurationCoordinator.h"
#include "StorageCoordinator.h"
#include "DevicePanelCoordinator.h"
#include "AiControlServer.h"
#include "DiskController_ImGui.h"
#include "LeChatMauveCard.h"
#include "Mockingboard.h"
#include "PhasorCard.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "SamCard.h"
#include "PrinterSoundDevice.h"
#include "Apple2Display.h"
#include "AudioCoordinator.h"
#include <GLFW/glfw3.h>
#include "BrowserFiles.h"
#include "EmulationController.h"
#include "imgui.h"
#include <cstdio>

void MainWindow::restoreBrowserSession()
{
#ifdef __EMSCRIPTEN__
    browserSession_.initialize(*controller);
    if (!browserSession_.error().empty()) {
        tapeStatusMessage = "Automatic resume failed: " + browserSession_.error();
        tapeStatusUntil = lastFrameTime + 15.0;
    }
#endif
}
void MainWindow::checkpointBrowserSession(bool running)
{
#ifdef __EMSCRIPTEN__
    browserSession_.checkpoint(*controller, running);
#else
    (void)running;
#endif
}
void MainWindow::renderBrowserSessionMenu()
{
#ifdef __EMSCRIPTEN__
    if (!ImGui::BeginMenu("Automatic resume")) return;
    bool enabled = browserSession_.enabled();
    if (ImGui::MenuItem("Resume this machine on reload", nullptr, &enabled))
        browserSession_.setEnabled(*controller, enabled);
    ImGui::TextWrapped("Checkpoints every 10 seconds and when leaving the tab. Saves the supported machine configuration and media. Serial ports must be idle. Restored writable floppy changes stay in the session.");
    if (browserSession_.suspended()) {
        ImGui::TextWrapped("Checkpointing suspended: %s", browserSession_.error().c_str());
        if (ImGui::MenuItem("Retry with current configuration")) browserSession_.setEnabled(*controller, true);
    }
    ImGui::EndMenu();
#endif
}

void MainWindow::renderSessionFileDialog()
{
#ifdef __EMSCRIPTEN__
    if (const auto file = pom2::takeBrowserFile()) {
        if (!file->error.empty()) {
            tapeStatusMessage = file->error;
            tapeStatusUntil = lastFrameTime + 10.0;
        } else if (!file->path.empty()) {
            if (file->kind == pom2::BrowserFileKind::Text) pasteDialogPath = file->path;
            else if (file->kind == pom2::BrowserFileKind::Disk) {
                bool ok = false;
                std::string error;
                if (browserDiskProfile_ != activeProfile) {
                    error = "Machine changed. Import the disk again.";
                } else if (browserDiskTargetsDrive_) {
                    ok = insertDriveRackImage(browserDiskTarget_, file->path);
                    if (!ok) error = tapeStatusMessage;
                } else {
                    ok = insertAndBootImage(file->path, error);
                }
                tapeStatusMessage = ok ? (browserDiskTargetsDrive_ ? "Disk imported into drive." : "Disk imported and booted.")
                                       : "Disk import failed: " + error;
                tapeStatusUntil = lastFrameTime + (ok ? 5.0 : 10.0);
                // Keep the unique MEMFS file: normal media write-back and session
                // capture use it. The original local file is never modified.
            } else {
                std::string error;
                const bool ok = pom2::loadSessionFile(*controller, file->path, error);
                tapeStatusMessage = ok ? "Session imported." : "Session failed: " + error;
                tapeStatusUntil = lastFrameTime + (ok ? 5.0 : 10.0);
                std::remove(file->path.c_str());
            }
        }
    }
#endif
    if (const auto picked = sessionFilePicker_.render()) {
        std::string error;
        bool ok = picked->save
            ? pom2::saveSessionFile(*controller, picked->path, error)
            : pom2::loadSessionFile(*controller, picked->path, error);
#ifdef __EMSCRIPTEN__
        if (ok && picked->save)
            ok = pom2::downloadBrowserFile(picked->path, "session.pom2session", error);
        if (ok) std::remove(picked->path.c_str());
#endif
        sessionFilePicker_.complete(ok, error);
#ifdef __EMSCRIPTEN__
        tapeStatusMessage = ok ? "Session download started." : "Session failed: " + error;
#else
        tapeStatusMessage = ok ? (picked->save ? "Session saved: " : "Session loaded: ") + picked->path
                               : "Session failed: " + error;
#endif
        tapeStatusUntil = lastFrameTime + (ok ? 5.0 : 10.0);
    }
}

#ifdef __EMSCRIPTEN__
void MainWindow::chooseBrowserDisk(const pom2::DrivePicture* target)
{
    if (!pom2::chooseBrowserFile(pom2::BrowserFileKind::Disk)) return;
    browserDiskTargetsDrive_ = target != nullptr;
    if (target) browserDiskTarget_ = *target;
    browserDiskProfile_ = activeProfile;
}
#endif


void MainWindow::initializeSessionLoader()
{
    controller->setSessionLoadQueue(std::make_shared<pom2::SessionLoadQueue>(
        [this](const pom2::SessionArchive& session, std::string& error) {
            return reconstructSessionMachine(session, error);
        }));
}
bool MainWindow::reconstructSessionMachine(const pom2::SessionArchive& session, std::string& error)
{
    const auto previousMix = audioCoordinator_->captureMixerCards();
    pom2::SessionMachineHooks hooks;
    hooks.detach = [this](const pom2::StateAccess&) {
        aiServer->detach();
        unregisterAllAudioSources();
        diskPanels.clear(); diskPanel = nullptr;
        display->setChatMauveCard(nullptr); display->setCardVideoSource(nullptr);
    };
    hooks.publish = [this, &previousMix](const pom2::StateAccess& state, pom2::SystemProfile profile) {
        activeProfile = profile; sessionMachineActive_ = true;
        const auto& cfg = pom2::profileConfig(profile);
        display->setAuxMemory(cfg.iieMode ? state.memory().auxData() : nullptr);
        const auto live = slotConfigCoordinator_->captureLive(state.memory().slotBus());
        for (int i = 1; i <= 7; ++i) {
            slotCards[i] = live.keys[i];
            auto* card = state.memory().slotBus().peripheral(i);
            if (!card) continue;
            auto mix = [&](auto* audioCard, pom2::AudioCoordinator::CardKind kind, float gain) {
                auto policy = audioCoordinator_->restoreCardSettings(*settings, kind, i, gain);
                for (const auto& old : previousMix) if (old.slot == i && old.kind == kind) {
                    policy.volume = old.volume; policy.muted = old.muted; break;
                }
                audioCard->setVolume(policy.volume); audioCard->setMuted(policy.muted);
            };
            using Kind = pom2::AudioCoordinator::CardKind;
            if (auto* a = dynamic_cast<MockingboardCard*>(card)) mix(a, Kind::Mockingboard, 0.35f);
            if (auto* a = dynamic_cast<PhasorCard*>(card)) mix(a, Kind::Phasor, 0.5f);
            if (auto* a = dynamic_cast<EchoPlusCard*>(card)) mix(a, Kind::EchoPlus, 0.7f);
            if (auto* a = dynamic_cast<EchoPlusTMS5220Card*>(card)) mix(a, Kind::EchoPlusTms5220, 0.7f);
            if (auto* a = dynamic_cast<pom2::SamCard*>(card)) mix(a, Kind::Sam, 0.7f);
            if (card->audioSource()) registerAudioSource(card->audioSource());
            if (live.keys[i] == "diskii") {
                diskPanels.push_back(std::make_unique<pom2::DiskController_ImGui>());
                if (!diskPanel) diskPanel = diskPanels.front().get();
            }
            if (auto* rgb = dynamic_cast<LeChatMauveCard*>(card)) display->setChatMauveCard(rgb);
        }
        registerAudioSource(printerSound.get());
        slotConfigCoordinator_->draft() = live.keys;
        slotDraftInited_ = true; ramWorksDraft_ = -1; chatMauveVariantDraft_.clear();
        ++mediaPanelSeedGen_;
        storageCoordinator_->clearAutoProvisioned();
        diskSavedCyclesPerFrame = cfg.defaultCyclesPerFrame;
        romLoaded_ = true; romStatus = "Firmware restored from session";
        romPath.clear(); charRomPath.clear();
        aiServer->attach(controller.get(), display.get(), primaryDiskII(), primaryHdvCard());
        aiServer->setProfileLabel(std::string(cfg.displayName));
        if (window) {
            std::string title = "POM2 " POM2_VERSION_STRING " — "; title.append(cfg.displayName);
            glfwSetWindowTitle(window, title.c_str());
        }
    };
    const bool ok = pom2::reconstructSession(*controller, session, error, hooks);
    if (ok) sessionMachineActive_ = true;
    return ok;
}
