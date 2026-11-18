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

// MainWindow — the Passport MIDI card's host side: the per-frame drain into
// MidiOutputCoordinator and the "MIDI out" section of the Audio Mixer.
// Setting `midi_out_port`: the host output by display name; absent = the
// backend's first one (Apple's DLS synthesizer on macOS, the first WinMM
// device on Windows), "none" = no port.

#include "MainWindow.h"

#include "EmulationController.h"
#include "HostMidiPort.h"
#include "Memory.h"
#include "MidiOutputCoordinator.h"
#include "NativeFileDialog.h"
#include "PassportMidiCard.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SlotBus.h"

#include "imgui.h"

#include <ctime>

namespace {
constexpr const char* kMidiPortKey = "midi_out_port";
constexpr const char* kNoPort = "none";
}

std::string MainWindow::midiPortSetting()
{
    if (settings && settings->has(kMidiPortKey)) {
        const std::string v = settings->getString(kMidiPortKey, kNoPort);
        return v == kNoPort ? std::string() : v;
    }
    // Listed once and cached: an ALSA listing opens a sequencer client.
    if (!midiPortsListed_) { midiPorts_ = pom2::HostMidiPort::list(); midiPortsListed_ = true; }
    return midiPorts_.empty() ? std::string() : midiPorts_.front();
}

// Every frame: drain the card under the lock, hand the bytes over without
// it. The port only opens while a Passport is plugged, so the DLS synth's
// output unit does not run on a machine that has no MIDI card.
void MainWindow::pumpMidiOut()
{
    if (!controller || !midiOut_) return;
    bool present = false;
    {
        auto st = controller->lockState();
        auto& bus = st.memory().slotBus();
        for (int s = 1; s < SlotBus::kSlotCount; ++s)
            if (auto* card = dynamic_cast<pom2::PassportMidiCard*>(bus.peripheral(s))) {
                card->drainMidiOut(midiBytes_);
                present = true;
            }
    }
    const std::string want = present ? midiPortSetting() : std::string();
    if (want != midiOut_->portName()) midiOut_->setPort(want);
    midiOut_->feed(midiBytes_, present, controller->emulatedCpuClockHz());
}

void MainWindow::renderMidiSection()
{
    if (!midiOut_ || !ImGui::CollapsingHeader("MIDI out (Passport MIDI card)")) return;
    if (!midiPortsListed_ || ImGui::SmallButton("Refresh ports")) {
        midiPorts_ = pom2::HostMidiPort::list();
        midiPortsListed_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", pom2::HostMidiPort::backend());

    const std::string current = midiPortSetting();
    ImGui::SetNextItemWidth(18.0f * ImGui::GetFontSize());
    if (ImGui::BeginCombo("Output", current.empty() ? "(no output)" : current.c_str())) {
        if (ImGui::Selectable("(no output)", current.empty()))
            settings->setString(kMidiPortKey, kNoPort);
        for (const auto& name : midiPorts_)
            if (ImGui::Selectable(name.c_str(), name == current))
                settings->setString(kMidiPortKey, name);
        ImGui::EndCombo();
    }
    const std::string status = midiOut_->status();
    ImGui::TextDisabled("%s · %llu messages sent", status.empty() ? "idle (no Passport plugged)" : status.c_str(),
                        static_cast<unsigned long long>(midiOut_->messagesSent()));

    if (!midiOut_->recording()) {
        if (ImGui::Button("Record .mid")) { midiOut_->startRecording(); midiRecordStatus_.clear(); }
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "REC");
        ImGui::SameLine();
        ImGui::Text("%zu events", midiOut_->recordedEvents());
        ImGui::SameLine();
        if (ImGui::Button("Stop and save…")) {
            char stamp[32];
            const std::time_t now = std::time(nullptr);
            std::strftime(stamp, sizeof stamp, "pom2-%Y%m%d-%H%M%S.mid", std::localtime(&now));
            std::string path;
            const auto r = pom2::nativeFileDialogAvailable()
                ? pom2::nativeFileDialog(true, "Save MIDI recording", "mid", "", stamp, path)
                : pom2::FileDialogResult::Unavailable;
            if (r == pom2::FileDialogResult::Unavailable)
                path = (pom2::userDataDir() / stamp).u8string();
            if (r != pom2::FileDialogResult::Cancelled) {
                path = pom2::withDefaultExtension(path, "mid");
                std::string error;
                midiRecordStatus_ = midiOut_->stopRecording(std::filesystem::u8path(path), error)
                    ? "saved " + path : error;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) midiOut_->discardRecording();
    }
    if (!midiRecordStatus_.empty()) ImGui::TextWrapped("%s", midiRecordStatus_.c_str());
    ImGui::TextDisabled("Timing comes from the emulated clock: a recording keeps the program's tempo.");
}
