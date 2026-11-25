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

// Pom2SfxHost — POM2's side of the beeper SFX editor (src/sfxbeep/, shared
// verbatim with POM1; ISfxHost.h is the seam). The editor owns the model, the
// UI and the assembler export; the host plays a sound effect and picks files.
//
// The preview is its own audio source: a square wave built from the effect's
// (cycles, level) pulses, at the active profile's CPU clock so the pitch is
// the one a program playing it through $C030 will make. It needs no CPU
// time and runs with the machine stopped — the editor is a tool, and a tool
// that only sounds while the guest runs is half a tool.

#ifndef POM2_SFX_HOST_H
#define POM2_SFX_HOST_H

#include "AudioSource.h"
#include "sfxbeep/ISfxHost.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class EmulationController;

/// The preview's audio source: pulses in, mono samples out.
class SfxPreviewAudio final : public AudioSource, public RateAware
{
public:
    /// Replace whatever is playing with `pulses` ((CPU cycles, level) pairs)
    /// played at `cpuClockHz`.
    void play(const std::vector<std::pair<uint32_t, bool>>& pulses, double cpuClockHz);
    void stop();

    void fillAudioBuffer(float* output, int frameCount) override;
    void setSampleRate(uint32_t hz) override;

private:
    std::mutex mutex_;                               // UI thread ⇄ audio callback
    std::deque<std::pair<double, bool>> segments_;   // (samples left, level)
    double sampleRate_ = 44100.0;
    float  level_ = 0.0f;                            // eased toward 0 when idle: no click
};

class Pom2SfxHost final : public sfxbeep::ISfxHost
{
public:
    explicit Pom2SfxHost(EmulationController* controller) : controller_(controller) {}

    void previewSfx(const std::vector<sfxbeep::Step>& steps) override;
    void stopPreview() override;
    bool pickFilePath(bool forSave, const std::string& title, const std::string& filterDesc,
                      const std::string& extCsv, const std::string& defaultDir,
                      const std::string& defaultName, std::string& outPath) override;
    bool nativeFilePickerAvailable() const override;

    /// The source to register on the audio bus.
    SfxPreviewAudio& audio() { return audio_; }

private:
    EmulationController* controller_;
    SfxPreviewAudio audio_;
};

#endif // POM2_SFX_HOST_H
