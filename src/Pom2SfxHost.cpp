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

#include "Pom2SfxHost.h"

#include "CpuClock.h"
#include "EmulationController.h"
#include "NativeFileDialog.h"
#include "sfxbeep/SfxPulse.h"

#include <algorithm>

namespace {
// The speaker's level in the mix, a little under the real beeper's so a
// preview over a running game does not jump out.
constexpr float kAmplitude = 0.20f;
}  // namespace

void SfxPreviewAudio::play(const std::vector<std::pair<uint32_t, bool>>& pulses, double cpuClockHz)
{
    std::lock_guard<std::mutex> lock(mutex_);
    segments_.clear();
    if (cpuClockHz <= 0.0) return;
    const double samplesPerCycle = sampleRate_ / cpuClockHz;
    for (const auto& pulse : pulses)
        if (pulse.first) segments_.emplace_back(pulse.first * samplesPerCycle, pulse.second);
}

void SfxPreviewAudio::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    segments_.clear();
}

void SfxPreviewAudio::setSampleRate(uint32_t hz)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (hz) sampleRate_ = static_cast<double>(hz);
}

void SfxPreviewAudio::fillAudioBuffer(float* output, int frameCount)
{
    // try_lock: the audio callback never waits on the UI thread. A missed
    // buffer (the UI is queueing a sound right now) is 6 ms of the previous
    // level easing to silence.
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    for (int i = 0; i < frameCount; ++i) {
        float target = 0.0f;
        if (lock.owns_lock()) {
            while (!segments_.empty() && segments_.front().first <= 0.0) segments_.pop_front();
            if (!segments_.empty()) {
                target = segments_.front().second ? kAmplitude : -kAmplitude;
                segments_.front().first -= 1.0;
                level_ = target;                  // a square wave: no easing between its edges
                output[i] = level_;
                continue;
            }
        }
        level_ += (target - level_) * 0.02f;      // idle: ease to zero, no click at the end
        output[i] = level_;
    }
}

void Pom2SfxHost::previewSfx(const std::vector<sfxbeep::Step>& steps)
{
    // The pitch a program will make: the routine's cycle counts at THIS
    // machine's clock (PAL and NTSC differ by 0.7 %).
    const VideoTiming& timing =
        pom2VideoTiming(controller_ ? controller_->getVideoStandard() : VideoStandard::NTSC);
    audio_.play(sfxbeep::sfxToPulses(steps), static_cast<double>(timing.cpuClockHz));
}

void Pom2SfxHost::stopPreview() { audio_.stop(); }

bool Pom2SfxHost::pickFilePath(bool forSave, const std::string& title, const std::string& filterDesc,
                               const std::string& extCsv, const std::string& defaultDir,
                               const std::string& defaultName, std::string& outPath)
{
    (void)filterDesc;
    return pom2::nativeFileDialog(forSave, title, extCsv, defaultDir, defaultName, outPath) ==
           pom2::FileDialogResult::Selected;
}

bool Pom2SfxHost::nativeFilePickerAvailable() const { return pom2::nativeFileDialogAvailable(); }
