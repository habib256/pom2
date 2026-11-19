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

// 8 s of stereo at 44.1 kHz: the UI drains every frame, so this only
// fills when the window stalls for that long.
constexpr std::size_t kRingFrames = 8 * 44100;

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
// user data folder when there is no dialog, empty when cancelled.
std::string askSavePath(const char* title, const char* ext)
{
    char stamp[40];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "pom2-%Y%m%d-%H%M%S.", std::localtime(&now));
    const std::string name = std::string(stamp) + ext;
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
    if (wavRing_ && wavWriter_) wavRing_->drainTo(*wavWriter_);
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
                const std::string path = askSavePath(vgm ? "Save VGM" : "Save YM", vgm ? "vgm" : "ym");
                if (!path.empty()) {
                    const std::vector<uint8_t> bytes = vgm ? pom2::aymusic::encodeVgm(cap)
                                                           : pom2::aymusic::encodeYm5(cap, 0);
                    std::error_code ec;
                    recordStatus_ = pom2::writeFileAtomic(std::filesystem::u8path(path), bytes.data(),
                                                          bytes.size(), ec)
                        ? "saved " + path + " (" + std::to_string(cap.writes.size()) + " AY writes)"
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
            const auto provisional = pom2::userDataDir() / "pom2-recording.wav";
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
    } else {
        pumpRecording();
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
                                         ? "saved " + path : why;
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
