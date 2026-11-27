// POM2 — GPL-3.0-or-later
#include "MainWindow.h"
#include "Version.h"
#include "SessionFiles.h"
#include "InputRecording.h"
#include "NativeFileDialog.h"
#include "ResourcePaths.h"
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
#include "PorteParoleCard.h"
#include "PrinterSoundDevice.h"
#include "Settings.h"
#include "Apple2Display.h"
#include "AudioCoordinator.h"
#include <GLFW/glfw3.h>
#include "BrowserFiles.h"
#include "EmulationController.h"
#include "imgui.h"
#include "IconsFontAwesome6.h"
#include <cstdio>
#include <ctime>
#include <filesystem>

void MainWindow::holdBrowserSession()
{
#ifdef __EMSCRIPTEN__
    browserSession_.initializeWithoutRestore();
#endif
}
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
    if (!ImGui::BeginMenu(ICON_FA_CLOCK_ROTATE_LEFT "  Automatic resume")) return;
    bool enabled = browserSession_.enabled();
    if (ImGui::MenuItem(ICON_FA_ROTATE "  Resume this machine on reload", nullptr, &enabled))
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
    pollInputRecording();
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
                // insertDriveRackImage has already explained its own refusal.
                if (!ok && !browserDiskTargetsDrive_) showDiskRefusal(file->path, error);
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
        display->setChatMauveCard(nullptr); clearCardVideoSources();
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
            if (auto* a = dynamic_cast<pom2::PorteParoleCard*>(card)) mix(a, Kind::PorteParole, 0.7f);
            if (card->audioSource()) registerAudioSource(card->audioSource());
            if (live.keys[i] == "diskii") {
                diskPanels.push_back(std::make_unique<pom2::DiskController_ImGui>());
                if (!diskPanel) diskPanel = diskPanels.front().get();
            }
            if (auto* rgb = dynamic_cast<LeChatMauveCard*>(card)) {
                // A user setting, not machine state: the session builds the
                // card without it (bug hunt 2026-11-18).
                if (settings) rgb->setInvertBit7(settings->getBool("chatmauve_invert_bit7", false));
                display->setChatMauveCard(rgb);
            }
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

// ── Host input recording (File menu) ───────────────────────────────────
// Record starts at once; Stop saves at once, into <user data>/recordings/,
// named by the date — no dialog between the user and the machine. Play
// picks a .pom2rec (or drop one on the window). The status bar shows REC /
// PLAY while one runs; the outcome goes to the status line.
namespace {
std::string newRecordingPath()
{
    std::error_code ec;
    const std::filesystem::path dir = pom2::userDataDir() / "recordings";
    std::filesystem::create_directories(dir, ec);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[64];
    std::strftime(name, sizeof name, "rec-%Y%m%d-%H%M%S.pom2rec", &local);
    return (dir / name).u8string();
}
} // namespace

void MainWindow::renderInputRecordingMenu()
{
#ifndef __EMSCRIPTEN__
    const auto mode = controller->inputJournalMode();
    if (mode == EmulationController::InputJournalMode::Recording) {
        if (ImGui::MenuItem(ICON_FA_STOP "  Stop recording input")) {
            std::string why, error;
            const std::string path = newRecordingPath();
            const bool ok = pom2::saveInputRecording(*controller, path, why, error);
            tapeStatusMessage = ok ? "Recording saved: " + path : "Recording not saved: " + error;
            tapeStatusUntil = lastFrameTime + (ok ? 8.0 : 10.0);
        }
    } else if (mode == EmulationController::InputJournalMode::Playing) {
        if (ImGui::MenuItem(ICON_FA_STOP "  Stop playing the recording")) {
            auto st = controller->lockState();
            controller->endInputJournalLocked("stopped by the user");
        }
    } else {
        const char* refusal = nullptr;
        { auto st = controller->lockState(); refusal = controller->inputRecordingRefusalLocked(); }
        ImGui::BeginDisabled(refusal != nullptr);
        if (ImGui::MenuItem(ICON_FA_CIRCLE "  Record input")) {
            std::string error;
            if (!pom2::startInputRecording(*controller, error)) {
                tapeStatusMessage = "Recording not started: " + error;
                tapeStatusUntil = lastFrameTime + 10.0;
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", refusal ? refusal
                : "Record every key, paste, joystick and mouse input, with the machine as it\n"
                  "stands now: played back, it re-runs this session exactly, and checks it.\n"
                  "For a bug report that reproduces itself, or a test made by playing.");
        if (ImGui::MenuItem(ICON_FA_PLAY "  Play a recording...")) {
            std::string picked;
            if (pom2::nativeFileDialogAvailable() &&
                pom2::nativeFileDialog(false, "Play a recording", "pom2rec",
                                       (pom2::userDataDir() / "recordings").u8string(), "", picked)
                    == pom2::FileDialogResult::Selected)
                (void)playInputRecordingFile(picked);
        }
    }
#endif
}

bool MainWindow::playInputRecordingFile(const std::string& path)
{
    std::string error;
    const bool ok = pom2::playInputRecording(*controller, path, error);
    tapeStatusMessage = ok ? "Playing " + std::filesystem::u8path(path).filename().u8string()
                           : "Recording not played: " + error;
    tapeStatusUntil = lastFrameTime + (ok ? 5.0 : 10.0);
    return ok;
}

void MainWindow::pollInputRecording()
{
    // A recording that ended by itself (a disk change, bytes from a peer…)
    // is saved at once: what it holds up to there is good.
    if (controller->inputRecordingEndedWaiting() &&
        controller->inputJournalMode() == EmulationController::InputJournalMode::Off) {
        std::string why, error;
        const std::string path = newRecordingPath();
        const bool ok = pom2::saveInputRecording(*controller, path, why, error);
        tapeStatusMessage = ok ? "Recording ended (" + why + "), saved: " + path
                               : "Recording not saved: " + error;
        tapeStatusUntil = lastFrameTime + 12.0;
    }
    // A playback's outcome, once.
    if (controller->inputJournalMode() == EmulationController::InputJournalMode::Off) {
        const std::string status = controller->inputJournalStatus();
        if (status != inputRecordingStatusShown_ && status.rfind("Playback ended", 0) == 0) {
            tapeStatusMessage = status;
            tapeStatusUntil = lastFrameTime + 12.0;
        }
        inputRecordingStatusShown_ = status;
    }
}

