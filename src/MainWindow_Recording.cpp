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

// MainWindow — the Audio Mixer's "Recording" section (AUDIO-013): the
// Mockingboard's AY writes as VGM or YM, and the mixed bus as WAV.
//
// The AY capture lives in the card (MockingboardCard::startAyCapture), fed
// by the same cycle-stamped event every AY write already produces, so it
// costs nothing when idle and is exact when running. The WAV take is the
// bus the speakers get: the audio thread pushes into a lock-free ring
// (WavRecorder.h) and `pumpRecording` drains it to disk once a frame,
// outside `stateMutex`. Both ask for the file name when the take ends, as
// the MIDI recorder does.

#include "MainWindow.h"

#include "AtomicFileReplace.h"
#include "BrowserFiles.h"
#include "AyMusicExport.h"
#include "CpuClock.h"
#include "EmulationController.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "NativeFileDialog.h"
#include "ResourcePaths.h"
#include "SlotBus.h"
#include "WavRecorder.h"

#include "imgui.h"

#include <algorithm>
#include <ctime>

namespace {

// 30 s of stereo at 44.1 kHz (10 MB): the UI drains every frame, but a
// native file dialog blocks the UI thread while the machine and the audio
// keep running, and an 8 s ring lost frames behind one left open (bug hunt
// 2026-11-19). The panel drains it before it opens a dialog of its own.
constexpr std::size_t kRingFrames = 30 * 44100;

// Where a file is written before it is handed over. On the desktop the
// user data folder (the dialog picks the real place); in the browser /tmp,
// in memory — not /persistent, which IDBFS would copy into IndexedDB on the
// next heartbeat — and the file then goes to the visitor as a download.
std::filesystem::path scratchFile(const std::string& name)
{
#ifdef __EMSCRIPTEN__
    return std::filesystem::path("/tmp") / name;
#else
    return pom2::userDataDir() / name;
#endif
}

// The browser has no save dialog and its files live in memory: hand the
// file to the visitor as a download, then let it go. The desktop wrote it
// where the user asked: nothing to do. Returns the status to show.
std::string deliver(const std::string& path, const std::string& saved)
{
#ifdef __EMSCRIPTEN__
    std::string err;
    const std::string name = std::filesystem::u8path(path).filename().u8string();
    const bool ok = pom2::downloadBrowserFile(path, name, err);
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(path), ec);
    return ok ? "downloaded " + name : "download failed: " + err;
#else
    (void)path;
    return saved;
#endif
}

MockingboardCard* findMockingboard(Memory& mem, int wantSlot)
{
    auto& bus = mem.slotBus();
    if (wantSlot > 0)
        return dynamic_cast<MockingboardCard*>(bus.peripheral(wantSlot));
    for (int s = 0; s < SlotBus::kSlotCount; ++s)
        if (auto* mb = dynamic_cast<MockingboardCard*>(bus.peripheral(s))) return mb;
    return nullptr;
}

// The MIDI recorder's dialog, for another extension: a picked path, the
// user data folder when there is no dialog (in the browser, a scratch file
// `deliver` downloads), empty when cancelled.
std::string askSavePath(const char* title, const char* ext)
{
    char stamp[40];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "pom2-%Y%m%d-%H%M%S.", std::localtime(&now));
    const std::string name = std::string(stamp) + ext;
#ifdef __EMSCRIPTEN__
    (void)title;
    return scratchFile(name).u8string();
#endif
    std::string path;
    const auto r = pom2::nativeFileDialogAvailable()
        ? pom2::nativeFileDialog(true, title, ext, "", name, path)
        : pom2::FileDialogResult::Unavailable;
    if (r == pom2::FileDialogResult::Cancelled) return {};
    if (r == pom2::FileDialogResult::Unavailable) path = (pom2::userDataDir() / name).u8string();
    return pom2::withDefaultExtension(path, ext);
}

} // namespace

void MainWindow::pumpRecording()
{
    if (!wavRing_ || !wavWriter_) return;
    wavRing_->drainTo(*wavWriter_);
    // The output device was re-opened at another rate (AudioDeviceWatch):
    // the mix now runs at it, and a WAV has ONE rate in its header. End the
    // take where it is, under its provisional name, rather than letting the
    // rest play at the wrong speed and pitch (bug hunt 2026-11-19).
    if (controller && controller->audio().getActualSampleRate() != wavWriter_->sampleRate()) {
        controller->audio().setCaptureRing(nullptr);
        wavRing_->drainTo(*wavWriter_);
        std::string why;
        char stamp[48];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "pom2-%Y%m%d-%H%M%S.wav", std::localtime(&now));
        const std::filesystem::path kept = scratchFile(stamp);
        recordStatus_ = wavWriter_->close(why, kept)
            ? deliver(kept.u8string(), "the audio device changed rate: the take ended here and was kept as " +
                                       kept.u8string())
            : why;
        wavWriter_.reset();
        wavRing_.reset();
    }
}

void MainWindow::renderRecordingSection()
{
    if (!controller || !ImGui::CollapsingHeader("Recording (VGM / YM / WAV)")) return;

    // ── Mockingboard music ──────────────────────────────────────────────
    int slotPresent = 0;
    bool capturing = false;
    {
        auto st = controller->lockState();
        if (auto* mb = findMockingboard(st.memory(), ayCaptureSlot_)) {
            slotPresent = mb->getSlot();
            capturing = ayCaptureSlot_ != 0 && mb->ayCaptureActive();
        }
    }
    if (ayCaptureSlot_ != 0 && !capturing) {
        // The card went with a profile switch or a slot change.
        ayCaptureSlot_ = 0;
        recordStatus_ = "the Mockingboard was unplugged: the music capture is lost";
    }
    if (!slotPresent) {
        ImGui::TextDisabled("Music: no Mockingboard plugged");
    } else if (!capturing) {
        if (ImGui::Button("Record Mockingboard music")) {
            auto st = controller->lockState();
            if (auto* mb = findMockingboard(st.memory(), slotPresent)) {
                mb->startAyCapture(pom2VideoTiming(controller->getVideoStandard()).cyclesPerFrame);
                ayCaptureSlot_ = slotPresent;
                recordStatus_.clear();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(slot %d)", slotPresent);
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "REC");
        ImGui::SameLine();
        ImGui::Text("AY music, slot %d", ayCaptureSlot_);
        const bool vgm = ImGui::Button("Stop and save .vgm…");
        ImGui::SameLine();
        const bool ym = ImGui::Button("Stop and save .ym…");
        ImGui::SameLine();
        const bool drop = ImGui::Button("Discard##ay");
        if (vgm || ym || drop) {
            pom2::aymusic::Capture cap;
            {
                auto st = controller->lockState();
                if (auto* mb = findMockingboard(st.memory(), ayCaptureSlot_)) cap = mb->stopAyCapture();
            }
            ayCaptureSlot_ = 0;
            if (!drop) {
                pumpRecording();   // a WAV take in progress, before a dialog blocks this thread
                const std::string path = askSavePath(vgm ? "Save VGM" : "Save YM", vgm ? "vgm" : "ym");
                if (!path.empty()) {
                    const std::vector<uint8_t> bytes = vgm ? pom2::aymusic::encodeVgm(cap)
                                                           : pom2::aymusic::encodeYm5(cap, 0);
                    std::error_code ec;
                    recordStatus_ = pom2::writeFileAtomic(std::filesystem::u8path(path), bytes.data(),
                                                          bytes.size(), ec)
                        ? deliver(path, "saved " + path + " (" + std::to_string(cap.writes.size()) +
                                        " AY writes)")
                        : "cannot write " + path + ": " + ec.message();
                }
            }
        }
    }

    // ── Mixed audio ─────────────────────────────────────────────────────
    if (!wavWriter_) {
        if (ImGui::Button("Record audio (.wav)")) {
            // Opened under a provisional name in the user data folder; the
            // real one is asked for when the take ends.
            auto writer = std::make_unique<pom2::WavFileWriter>();
            const auto provisional = scratchFile("pom2-recording.wav");
            std::string why;
            if (writer->open(provisional, controller->audio().getActualSampleRate(),
                             AudioDevice::kChannels, why)) {
                wavRing_ = std::make_unique<pom2::AudioCaptureRing>(kRingFrames);
                wavWriter_ = std::move(writer);
                controller->audio().setCaptureRing(wavRing_.get());
                recordStatus_.clear();
            } else {
                recordStatus_ = why;
            }
        }
    } else if (pumpRecording(), !wavWriter_) {
        // The pump just ended the take (the device changed rate): its
        // status is shown below; nothing left to draw (round 2: this read
        // wavWriter_ after it had been reset).
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "REC");
        ImGui::SameLine();
        const double secs = static_cast<double>(wavWriter_->framesWritten()) /
                            static_cast<double>(std::max<uint32_t>(1, wavWriter_->sampleRate()));
        ImGui::Text("audio %.1f s", secs);
        if (const uint64_t lost = wavRing_->droppedFrames()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "(%llu frames dropped)",
                               static_cast<unsigned long long>(lost));
        }
        const bool save = ImGui::Button("Stop and save .wav…");
        ImGui::SameLine();
        const bool drop = ImGui::Button("Discard##wav");
        if (save || drop) {
            controller->audio().setCaptureRing(nullptr);   // no push in flight after this
            wavRing_->drainTo(*wavWriter_);
            if (save) {
                const std::string path = askSavePath("Save WAV", "wav");
                std::string why;
                if (path.empty()) wavWriter_->discard();
                else recordStatus_ = wavWriter_->close(why, std::filesystem::u8path(path))
                                         ? deliver(path, "saved " + path) : why;
            } else {
                wavWriter_->discard();
            }
            wavWriter_.reset();
            wavRing_.reset();
        }
    }
    if (!recordStatus_.empty()) ImGui::TextWrapped("%s", recordStatus_.c_str());
    ImGui::TextDisabled("VGM keeps every AY write at its sample (both chips); YM is the first\n"
                        "chip, one frame a step. WAV is what the speakers get.");
}
