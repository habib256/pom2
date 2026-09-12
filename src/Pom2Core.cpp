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

#include <pom2/core.hpp>

#include "Apple2Display.h"
#include "AudioMix.h"
#include "CassetteDevice.h"
#include "CpuClock.h"
#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "SlotBus.h"
#include "SpeakerDevice.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace pom2 {

class Core::Impl {
public:
    explicit Impl(const CoreConfig& config)
        : cpu(&memory)
    {
        memory.setCpu(&cpu);
        memory.setIIEMode(config.iieMemory);
        memory.setVideoStandard(config.timing == VideoTiming::PAL
            ? ::VideoStandard::PAL
            : ::VideoStandard::NTSC);
        cpu.setCpuMode(config.cpu == CpuModel::CMOS65C02
            ? M6502::CpuMode::CMOS
            : M6502::CpuMode::NMOS);
        display.setAuxMemory(memory.auxData());
        cpuClockHz = static_cast<double>(pom2VideoTiming(
            config.timing == VideoTiming::PAL
                ? ::VideoStandard::PAL
                : ::VideoStandard::NTSC).cpuClockHz);
        speaker.setCpuClock(cpuClockHz);
        cassette.setCpuClock(cpuClockHz);
        cassette.setAudioAvailable(true);
        cassette.setAudioOutputSampleRate(audioSampleRate);
        memory.setSpeakerDevice(&speaker);
        memory.setCassetteDevice(&cassette);
    }

    ~Impl()
    {
        memory.setCassetteDevice(nullptr);
        memory.setSpeakerDevice(nullptr);
    }

    Memory memory;
    M6502  cpu;
    Apple2Display display;
    SpeakerDevice speaker;
    CassetteDevice cassette;
    DiskIICard* diskII = nullptr; // non-owning; slot 6 owns the card
    MockingboardCard* mockingboard = nullptr; // non-owning; its slot owns it
    double cpuClockHz = static_cast<double>(POM2_CPU_CLOCK_HZ);
    std::uint32_t audioSampleRate = 44100;
    std::vector<float> speakerScratch;
    std::vector<float> cassetteScratch;
    std::vector<float> leftScratch;
    std::vector<float> rightScratch;
    /// The bus this core mixes into — the same law the emulator's own audio
    /// device plays through (`AudioMix.h`). `mixList` is refilled per buffer
    /// and keeps its capacity, so the audio path stays allocation-free.
    pom2::MixBusState         mixBus;
    std::vector<AudioSource*> mixList;
    std::string error;
};

Core::Core(CoreConfig config)
    : impl_(std::make_unique<Impl>(config))
{
}

Core::~Core() = default;
Core::Core(Core&&) noexcept = default;
Core& Core::operator=(Core&&) noexcept = default;

bool Core::loadRom(std::string_view path, bool lowerBankFor32K)
{
    const std::string ownedPath(path);
    const bool loaded = impl_->memory.loadAppleIIRom(
        ownedPath.c_str(), lowerBankFor32K) != 0;
    impl_->error = loaded ? std::string{} : impl_->memory.getLastError();
    return loaded;
}

bool Core::loadCharacterRom(std::string_view path, int bank)
{
    const std::string ownedPath(path);
    const bool loaded = impl_->memory.loadCharRom(ownedPath.c_str(), bank) != 0;
    impl_->error = loaded ? std::string{} : impl_->memory.getLastError();
    return loaded;
}

std::string Core::lastError() const
{
    return impl_->error;
}

bool Core::attachDiskII(std::string_view bootRom,
                        std::string_view sequencerRom)
{
    SlotPeripheral* existing = impl_->memory.slotBus().peripheral(
        DiskIICard::kDefaultSlot);
    if (existing && existing != impl_->diskII) {
        impl_->error = "Slot 6 is already occupied";
        return false;
    }
    auto card = std::make_unique<DiskIICard>(DiskIICard::kDefaultSlot);
    card->setCpu(&impl_->cpu);

    if (!card->loadBootRom(std::string(bootRom))) {
        impl_->error = "Cannot load Disk II boot ROM: " + std::string(bootRom);
        return false;
    }
    if (!sequencerRom.empty()
        && !card->loadLssRom(std::string(sequencerRom))) {
        impl_->error = "Cannot load Disk II sequencer ROM: "
                     + std::string(sequencerRom);
        return false;
    }

    impl_->diskII = card.get();
    impl_->memory.slotBus().plug(DiskIICard::kDefaultSlot, std::move(card));
    impl_->error.clear();
    return true;
}

bool Core::insertDisk(int drive, std::string_view path)
{
    if (!impl_->diskII) {
        impl_->error = "Disk II controller is not attached";
        return false;
    }
    if (!DiskIICard::validDrive(drive)) {
        impl_->error = "Disk II drive index must be 0 or 1";
        return false;
    }
    const bool inserted = impl_->diskII->insertDisk(drive, std::string(path));
    impl_->error = inserted ? std::string{}
                            : impl_->diskII->getLastError(drive);
    return inserted;
}

bool Core::ejectDisk(int drive)
{
    if (!impl_->diskII) {
        impl_->error = "Disk II controller is not attached";
        return false;
    }
    if (!DiskIICard::validDrive(drive)) {
        impl_->error = "Disk II drive index must be 0 or 1";
        return false;
    }
    const bool ejected = impl_->diskII->ejectDisk(drive);
    impl_->error = ejected ? std::string{}
                           : impl_->diskII->getLastError(drive);
    return ejected;
}

bool Core::diskInserted(int drive) const
{
    return impl_->diskII && impl_->diskII->isDiskLoaded(drive);
}

bool Core::bootDisk()
{
    if (!impl_->diskII || !impl_->diskII->hasBootRom()
        || (!impl_->diskII->isDiskLoaded(0)
            && !impl_->diskII->isDiskLoaded(1))) {
        impl_->error = "Disk II boot requires a controller, boot ROM and disk";
        return false;
    }
    coldBoot();
    impl_->diskII->seekTrack0();
    impl_->cpu.setProgramCounter(0xC600);
    impl_->error.clear();
    return true;
}

bool Core::loadCassette(std::string_view path)
{
    const bool loaded = impl_->cassette.loadTape(std::string(path));
    impl_->error = loaded ? std::string{} : impl_->cassette.getLastError();
    return loaded;
}

bool Core::saveCassette(std::string_view path)
{
    const bool saved = impl_->cassette.saveTape(std::string(path));
    impl_->error = saved ? std::string{} : impl_->cassette.getLastError();
    return saved;
}

void Core::playCassette()
{
    impl_->cassette.playTape();
}

void Core::stopCassette()
{
    impl_->cassette.stopTape();
}

void Core::pauseCassette(bool paused)
{
    impl_->cassette.setPlaybackPaused(paused);
}

void Core::rewindCassette()
{
    impl_->cassette.rewindTape();
}

void Core::ejectCassette()
{
    impl_->cassette.ejectTape();
}

void Core::armCassetteRecording()
{
    impl_->cassette.armRecording();
}

void Core::clearCassetteRecording()
{
    impl_->cassette.clearRecordedTape();
}

CassetteState Core::cassetteState() const
{
    return {
        impl_->cassette.hasLoadedTape(),
        impl_->cassette.isPlaybackActive()
            || impl_->cassette.isPlaybackArmed(),
        impl_->cassette.isPlaybackPaused(),
        impl_->cassette.isRewinding(),
        impl_->cassette.hasRecordedTape(),
        impl_->cassette.isAudioStreamMode(),
    };
}

void Core::setCassetteVolume(float volume)
{
    impl_->cassette.setVolume(volume);
}

bool Core::attachMockingboard(int slot, MockingboardModel model)
{
    if (slot < 1 || slot > 7) {
        impl_->error = "Mockingboard slot must be between 1 and 7";
        return false;
    }
    if (impl_->memory.slotBus().isPlugged(slot)) {
        impl_->error = "Requested Mockingboard slot is already occupied";
        return false;
    }
    auto card = std::make_unique<MockingboardCard>(
        slot, model == MockingboardModel::SoundII
            ? MockingboardCard::Variant::SoundII
            : MockingboardCard::Variant::AC);
    card->setCpu(&impl_->cpu);
    card->setCpuClock(impl_->cpuClockHz);
    card->setSampleRate(impl_->audioSampleRate);
    impl_->mockingboard = card.get();
    impl_->memory.slotBus().plug(slot, std::move(card));
    impl_->error.clear();
    return true;
}

bool Core::mockingboardAttached() const
{
    return impl_->mockingboard != nullptr;
}

void Core::setMockingboardVolume(float volume)
{
    if (impl_->mockingboard) impl_->mockingboard->setVolume(volume);
}

void Core::softReset()
{
    impl_->memory.setIicSmartPortArmed(false);
    impl_->memory.resetSoftSwitchesWarm();
    impl_->memory.slotBus().reset();
    impl_->cpu.softReset();
}

void Core::hardReset()
{
    impl_->memory.setIicSmartPortArmed(false);
    impl_->memory.resetSoftSwitchesWarm();
    impl_->memory.slotBus().reset();
    impl_->speaker.reset();
    impl_->cassette.resetCpuSide();
    impl_->cpu.hardReset();
}

void Core::coldBoot()
{
    impl_->memory.setIicSmartPortArmed(false);
    impl_->memory.clearRam();
    impl_->memory.resetSoftSwitches();
    impl_->memory.slotBus().reset();
    impl_->speaker.reset();
    impl_->cassette.resetCpuSide();
    impl_->cpu.hardReset();
}

int Core::run(int minCycles)
{
    return minCycles > 0 ? impl_->cpu.run(minCycles) : 0;
}

void Core::step()
{
    impl_->cpu.step();
}

std::uint8_t Core::read(std::uint16_t address)
{
    return impl_->memory.memRead(address);
}

void Core::write(std::uint16_t address, std::uint8_t value)
{
    impl_->memory.memWrite(address, value);
}

void Core::queueKey(std::uint8_t ascii)
{
    impl_->memory.queueKey(ascii);
}

std::size_t Core::pasteText(std::string_view text)
{
    return impl_->memory.pasteText(text.data(), text.size());
}

bool Core::setPaddle(int paddle, std::uint8_t value)
{
    if (paddle < 0 || paddle > 3) {
        impl_->error = "Paddle index must be between 0 and 3";
        return false;
    }
    impl_->memory.setPaddle(paddle, value);
    impl_->error.clear();
    return true;
}

bool Core::setPaddleButton(int button, bool pressed)
{
    if (button < 0 || button > 2) {
        impl_->error = "Paddle button index must be between 0 and 2";
        return false;
    }
    impl_->memory.setPaddleButton(button, pressed);
    impl_->error.clear();
    return true;
}

void Core::setJoystickAxes(float x, float y)
{
    const auto axisToPaddle = [](float axis) -> std::uint8_t {
        if (!std::isfinite(axis)) return 128;
        const float mapped = (std::clamp(axis, -1.0f, 1.0f) + 1.0f) * 127.5f;
        return static_cast<std::uint8_t>(mapped + 0.5f);
    };
    impl_->memory.setPaddle(0, axisToPaddle(x));
    impl_->memory.setPaddle(1, axisToPaddle(y));
}

FramebufferView Core::renderFrame()
{
    impl_->display.render(impl_->memory);
    return {
        impl_->display.pixels(),
        impl_->display.width(),
        impl_->display.height(),
    };
}

void Core::setAudioSampleRate(std::uint32_t sampleRate)
{
    if (sampleRate == 0) return;
    impl_->audioSampleRate = sampleRate;
    impl_->speaker.setSampleRate(sampleRate);
    impl_->cassette.setAudioOutputSampleRate(sampleRate);
    if (impl_->mockingboard) impl_->mockingboard->setSampleRate(sampleRate);
}

std::size_t Core::pullSpeakerAudio(float* monoOutput,
                                   std::size_t frameCount,
                                   std::uint32_t sampleRate)
{
    if (!monoOutput || frameCount == 0 || sampleRate == 0) return 0;

    setAudioSampleRate(sampleRate);
    std::size_t produced = 0;
    constexpr std::size_t kMaxChunk =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    while (produced < frameCount) {
        const std::size_t remaining = frameCount - produced;
        const int chunk = static_cast<int>(std::min(remaining, kMaxChunk));
        impl_->speaker.fillAudioBuffer(monoOutput + produced, chunk);
        produced += static_cast<std::size_t>(chunk);
    }
    return produced;
}

void Core::setSpeakerVolume(float volume)
{
    impl_->speaker.setVolume(volume);
}

std::size_t Core::pullAudio(float* interleavedStereo,
                            std::size_t frameCount,
                            std::uint32_t sampleRate)
{
    if (!interleavedStereo || frameCount == 0 || sampleRate == 0) return 0;

    setAudioSampleRate(sampleRate);

    // THE mixer — the same law `AudioDevice` plays through (`AudioMix.h`),
    // over this core's own bus state. This used to be a SECOND, divergent
    // mixer: speaker + cassette + one Mockingboard, summed and clamped, with
    // no pan, no master gain, no mute, no mono downmix, no suspend, no
    // meters and no second sound card. Every later fix to the real mixer
    // missed it, which is what makes divergence worth REMOVING rather than
    // patching.
    impl_->mixList.clear();
    impl_->mixList.push_back(&impl_->speaker);
    impl_->mixList.push_back(&impl_->cassette);
    for (int slot = 1; slot <= 7; ++slot) {
        if (SlotPeripheral* card = impl_->memory.slotBus().peripheral(slot)) {
            if (AudioSource* source = card->audioSource())
                impl_->mixList.push_back(source);
        }
    }

    // The core exposes no master controls, so: unity gain, unmuted, stereo —
    // the signal the GUI plays at its own defaults.
    pom2::MixParams params;
    params.sampleRate = sampleRate;

    constexpr std::size_t kMixChunk = 4096;
    std::size_t produced = 0;
    while (produced < frameCount) {
        const std::size_t count = std::min(frameCount - produced, kMixChunk);
        pom2::mixSourcesInto(interleavedStereo + 2 * produced,
                             static_cast<int>(count), impl_->mixList,
                             params, impl_->mixBus);
        produced += count;
    }
    return produced;
}

CpuState Core::cpuState() const
{
    return {
        impl_->cpu.getAccumulator(),
        impl_->cpu.getXRegister(),
        impl_->cpu.getYRegister(),
        impl_->cpu.getStatusRegister(),
        impl_->cpu.getStackPointer(),
        impl_->cpu.getProgramCounter(),
        impl_->memory.getCycleCounter(),
        impl_->cpu.isHalted(),
    };
}

} // namespace pom2
