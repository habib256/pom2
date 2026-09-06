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

// Apple II built-in cassette interface, ported from POM1's ACI.

#include "CassetteDevice.h"
#include "AtomicFileReplace.h"

// miniaudio is compiled via AudioDevice.cpp (MINIAUDIO_IMPLEMENTATION lives
// there). We only need the function prototypes here.
#include "third_party/miniaudio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <type_traits>
#include <vector>

namespace {

// .aci magic — kept identical to POM1 so cassettes recorded on either
// emulator can round-trip. The format (8-byte magic + 1 version byte +
// 1 initial-level byte + 2 padding + 4 LE count + count×4 LE durations)
// is structural to the pulse-stream representation, not Apple-specific.
constexpr char kAciMagic[] = "POM1ACI1";
constexpr uint32_t kMaxRealtimeGapCycles = 50000;
constexpr uint32_t kAudioRampInSamples = 64;

template <typename Emit>
bool writeTapeAtomic(const std::string& path, std::string& error, Emit&& emit)
{
    namespace fs = std::filesystem;
    const fs::path target(path);
    const fs::path tmp(path + ".pom2tmp");
    std::error_code permEc;
    const fs::perms perms = fs::status(target, permEc).permissions();
    const bool havePerms = !permEc;
    // The temp name is ours by construction, so whatever sits on it is our own
    // debris from a crashed run or somebody else's plant — and ofstream(trunc)
    // follows a symlink like any other open, which would land the tape on the
    // link's victim and then rename the link away. Same rule as every other
    // write-back (AtomicFileReplace.h).
    {
        std::error_code tmpEc;
        if (!pom2::prepareTempPath(tmp, tmpEc)) {
            error = "Cannot use temp tape file " + tmp.string() + ": " + tmpEc.message();
            return false;
        }
    }
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "Cannot write tape file: " + tmp.string(); return false; }
        emit(out);
        out.flush();
        out.close();
        if (!out) {
            error = "Short write on tape file: " + tmp.string();
            std::error_code ignored;
            fs::remove(tmp, ignored);
            return false;
        }
    }
    std::error_code ec;
    if (havePerms) {
        fs::permissions(tmp, perms, ec);
        ec.clear();
    }
    if (!pom2::replaceFileAtomic(tmp, target, ec)) {
        error = "Cannot replace tape file " + path + ": " + ec.message();
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return false;
    }
    return true;
}

uint16_t readLe16(const uint8_t* d)
{
    return static_cast<uint16_t>(d[0] | (static_cast<uint16_t>(d[1]) << 8));
}

uint32_t readLe32(const uint8_t* d)
{
    return static_cast<uint32_t>(d[0]) |
           (static_cast<uint32_t>(d[1]) << 8) |
           (static_cast<uint32_t>(d[2]) << 16) |
           (static_cast<uint32_t>(d[3]) << 24);
}

void writeLe16(std::ofstream& f, uint16_t v)
{
    const uint8_t b[2] = { static_cast<uint8_t>(v & 0xFF),
                           static_cast<uint8_t>((v >> 8) & 0xFF) };
    f.write(reinterpret_cast<const char*>(b), 2);
}

void writeLe32(std::ofstream& f, uint32_t v)
{
    const uint8_t b[4] = { static_cast<uint8_t>(v & 0xFF),
                           static_cast<uint8_t>((v >> 8) & 0xFF),
                           static_cast<uint8_t>((v >> 16) & 0xFF),
                           static_cast<uint8_t>((v >> 24) & 0xFF) };
    f.write(reinterpret_cast<const char*>(b), 4);
}

std::string lowerExtension(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

struct PcmDurationDecoder {
    uint32_t sampleRate = 0;
    bool foundSignal = false;
    bool initialLevel = false;
    bool currentLevel = false;
    uint64_t heldSamples = 0;
    std::vector<uint32_t> durations;
};

bool emitPcmDuration(PcmDurationDecoder& d, std::string& error,
                     size_t maxTransitions)
{
    if (d.durations.size() >= maxTransitions) {
        error = "Audio tape exceeds the transition limit";
        return false;
    }
    const long double scaled = static_cast<long double>(d.heldSamples) *
        static_cast<long double>(POM2_CPU_CLOCK_HZ) /
        static_cast<long double>(d.sampleRate);
    const uint64_t rounded = static_cast<uint64_t>(std::llround(scaled));
    d.durations.push_back(static_cast<uint32_t>(std::max<uint64_t>(
        1, std::min<uint64_t>(rounded, std::numeric_limits<uint32_t>::max()))));
    return true;
}

bool consumePcm(PcmDurationDecoder& d, const float* samples, size_t count,
                std::string& error, size_t maxTransitions)
{
    for (size_t i = 0; i < count; ++i) {
        const float sample = samples[i];
        if (!d.foundSignal) {
            if (sample == 0.0f) continue;
            d.foundSignal = true;
            d.initialLevel = d.currentLevel = sample > 0.0f;
            d.heldSamples = 1;
            continue;
        }
        const bool newLevel = sample == 0.0f ? d.currentLevel : sample > 0.0f;
        if (newLevel == d.currentLevel) {
            ++d.heldSamples;
            continue;
        }
        if (!emitPcmDuration(d, error, maxTransitions)) return false;
        d.currentLevel = newLevel;
        d.heldSamples = 1;
    }
    return true;
}

bool finishPcm(PcmDurationDecoder& d, std::string& error,
               size_t maxTransitions)
{
    if (!d.foundSignal) {
        error = "Audio file does not contain a detectable cassette signal";
        return false;
    }
    return emitPcmDuration(d, error, maxTransitions);
}

} // namespace

std::string CassetteDevice::lookupTapeInfo(const std::string& path)
{
    namespace fs = std::filesystem;
    const fs::path tapePath(path);
    const fs::path dir = tapePath.parent_path();
    if (dir.empty()) return {};

    const fs::path infoFile = dir / "tapeinfo.txt";
    std::ifstream file(infoFile);
    if (!file.is_open()) return {};

    const std::string baseName = tapePath.filename().string();
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        if (key == baseName) return val;
    }
    return {};
}

CassetteDevice::CassetteDevice()
{
    // Threading-discipline guard: these flags cross the UI ↔ audio-callback
    // thread boundary (audioStreamMode is read lock-free in fillAudioBuffer;
    // the ramp counter is touched under two different mutexes). They MUST stay
    // atomic — reverting either to a plain type breaks the build here on
    // purpose (this is the pin for the round-8 data-race fix).
    static_assert(std::is_same_v<decltype(audioStreamMode), std::atomic<bool>>,
                  "audioStreamMode must be atomic (cross-thread flag)");
    static_assert(std::is_same_v<decltype(audioRampInSamplesRemaining),
                                 std::atomic<uint32_t>>,
                  "audioRampInSamplesRemaining must be atomic (cross-thread)");
    static_assert(std::is_same_v<decltype(playbackActive), std::atomic<bool>>,
                  "playbackActive must be atomic (written from the audio callback)");
    reset();
}

void CassetteDevice::fillAudioBuffer(float* output, int frameCount)
{
    // Stream mode — direct decode + resample via miniaudio.
    if (audioStreamMode) {
        std::lock_guard<std::mutex> lock(audioStreamMutex);
        if (!audioStreamDecoderOpen || !playbackActive ||
            playbackPaused.load(std::memory_order_acquire)) {
            std::fill_n(output, frameCount, 0.0f);
            return;
        }
        ma_uint64 framesRead = 0;
        ma_decoder_read_pcm_frames(&audioStreamDecoder, output,
                                   static_cast<ma_uint64>(frameCount), &framesRead);
        audioStreamCursor += framesRead;

        constexpr float kStreamGain = 0.71f;
        const float vol = muted.load(std::memory_order_relaxed)
            ? 0.0f
            : volume.load(std::memory_order_relaxed);
        const int consumed = static_cast<int>(framesRead);
        for (int i = 0; i < consumed; ++i) {
            output[i] *= kStreamGain * vol;
            if (audioRampInSamplesRemaining > 0) {
                const float ramp = 1.0f - (static_cast<float>(audioRampInSamplesRemaining) /
                                           static_cast<float>(kAudioRampInSamples));
                output[i] *= ramp;
                audioRampInSamplesRemaining--;
            }
        }
        if (consumed < frameCount) {
            std::fill_n(output + consumed, frameCount - consumed, 0.0f);
            if (framesRead == 0) playbackActive = false;
        }
        // Mix the mode-transition clunk on top.
        {
            std::lock_guard<std::mutex> clickLock(audioMutex);
            if (clickCursor < clickBuffer.size()) {
                const int mix = std::min<int>(frameCount,
                    static_cast<int>(clickBuffer.size() - clickCursor));
                for (int i = 0; i < mix; ++i) {
                    output[i] += clickBuffer[clickCursor++] * vol;
                }
            }
        }
        return;
    }

    // Pulse mode — drain the segment queue at the device sample rate.
    static constexpr float kFilterAlpha = 0.33f;
    std::lock_guard<std::mutex> lock(audioMutex);
    if (playbackPaused.load(std::memory_order_acquire)) {
        std::fill_n(output, frameCount, 0.0f);
        audioPlaybackSample = 0.0f;
        return;
    }
    const float vol = muted.load(std::memory_order_relaxed)
        ? 0.0f
        : volume.load(std::memory_order_relaxed);
    for (int i = 0; i < frameCount; ++i) {
        float targetSample = 0.0f;
        if (!audioQueue.empty()) {
            targetSample = audioQueue.front().sampleValue;
            if (audioQueue.front().remainingSamples > 0)
                audioQueue.front().remainingSamples--;
            if (audioQueue.front().remainingSamples == 0)
                audioQueue.pop_front();
        }
        if (audioRampInSamplesRemaining > 0) {
            const float ramp = 1.0f - (static_cast<float>(audioRampInSamplesRemaining) /
                                       static_cast<float>(kAudioRampInSamples));
            targetSample *= ramp;
            audioRampInSamplesRemaining--;
        }
        audioPlaybackSample += (targetSample - audioPlaybackSample) * kFilterAlpha;
        float s = audioPlaybackSample * vol;
        if (clickCursor < clickBuffer.size())
            s += clickBuffer[clickCursor++] * vol;
        output[i] = s;
    }
}

double CassetteDevice::getQueuedAudioSeconds() const
{
    std::lock_guard<std::mutex> lock(audioMutex);
    uint64_t queued = 0;
    for (const auto& seg : audioQueue) queued += seg.remainingSamples;
    return static_cast<double>(queued) / static_cast<double>(audioOutputSampleRate);
}

void CassetteDevice::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    volume.store(v, std::memory_order_relaxed);
}

void CassetteDevice::resetPlaybackState()
{
    // Always leave the deck DISARMED. Arming is the user's responsibility
    // (PLAY button only).
    playbackArmed   = false;
    playbackActive  = false;
    playbackIndex   = 0;
    cyclesUntilInputToggle = 0;
    inputLevel      = loadedInitialLevel;
    rewinding       = false;
    rewCarryCycles  = 0;
    lastTapeInputCycle = currentCycle;
}

void CassetteDevice::clearLiveAudioState()
{
    std::lock_guard<std::mutex> lock(audioMutex);
    audioSampleRemainder = 0.0;
    audioPlaybackSample  = 0.0f;
    audioRampInSamplesRemaining = kAudioRampInSamples;
    audioQueue.clear();
}

void CassetteDevice::stopPulseAudio()
{
    clearLiveAudioState();
}

void CassetteDevice::playMechanicalClick()
{
    // ~70 ms damped thud + noise burst, mixed on top of the deck output.
    std::lock_guard<std::mutex> lock(audioMutex);
    const uint32_t rate = std::max<uint32_t>(1, audioOutputSampleRate);
    const uint32_t durSamples = rate / 14;
    clickBuffer.assign(durSamples, 0.0f);
    uint32_t lcg = 0xC7E5A5B7u;
    constexpr float kTwoPi = 6.28318530718f;
    for (uint32_t i = 0; i < durSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        const float attack = std::min(1.0f, t * 400.0f);
        const float decay  = std::exp(-t * 30.0f);
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = (static_cast<float>(static_cast<int32_t>(lcg)) / 2147483648.0f);
        const float thud = std::sin(kTwoPi * 95.0f * t);
        const float tick = std::sin(kTwoPi * 1300.0f * t) * std::exp(-t * 120.0f);
        clickBuffer[i] = (0.45f * thud + 0.30f * tick + 0.25f * noise) * attack * decay * 0.35f;
    }
    clickCursor = 0;
}

void CassetteDevice::fireClickIfModeChanged()
{
    const DeckMode m = getDeckMode();
    if (m == lastDeckMode) return;
    lastDeckMode = m;
    playMechanicalClick();
}

void CassetteDevice::reset()
{
    currentCycle = 0;
    outputLevel  = false;
    recordedInitialLevel = false;
    recordingStarted = false;
    lastOutputToggleCycle = 0;
    recordedDurations.clear();
    recordingOverflow = false;
    playbackPaused.store(false, std::memory_order_release);
    resetPlaybackState();
    stopPulseAudio();
}

void CassetteDevice::resetCpuSide()
{
    // Apple II hard-reset clobbers the cassette OUTPUT flip-flop and the
    // CPU-cycle timebase only. Tape position, recording buffer and
    // mechanical state survive — a real deck doesn't rewind because the
    // computer was reset.
    currentCycle = 0;
    outputLevel  = false;
    lastOutputToggleCycle = 0;
    // Re-base the input-side stamp too: it is compared against
    // `currentCycle` with unsigned subtraction (leader-rewind gap check),
    // so leaving it at the pre-reset value would wrap to a huge gap.
    lastTapeInputCycle = 0;
}

void CassetteDevice::resetForTimeJump()
{
    // See the header. Same re-base as resetCpuSide (the stamps are compared
    // with unsigned subtraction, so a backwards CPU jump wraps them), plus a
    // drop of the live audio queue, which holds segments generated for cycles
    // that are no longer going to happen.
    currentCycle          = 0;
    outputLevel           = false;
    lastOutputToggleCycle = 0;
    lastTapeInputCycle    = 0;
    stopPulseAudio();
}

void CassetteDevice::setPlaybackPaused(bool paused)
{
    const bool prev = playbackPaused.exchange(paused, std::memory_order_acq_rel);
    if (prev == paused || paused) return;
    if (audioStreamMode) {
        std::lock_guard<std::mutex> lock(audioStreamMutex);
        audioRampInSamplesRemaining = kAudioRampInSamples;
    } else {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioRampInSamplesRemaining = kAudioRampInSamples;
    }
}

void CassetteDevice::seekRelativeSeconds(double deltaSeconds)
{
    if (!audioStreamMode) return;
    std::lock_guard<std::mutex> lock(audioStreamMutex);
    if (!audioStreamDecoderOpen || audioOutputSampleRate == 0) return;

    const int64_t rate = static_cast<int64_t>(audioOutputSampleRate);
    int64_t newFrame = static_cast<int64_t>(audioStreamCursor) +
                       static_cast<int64_t>(std::llround(deltaSeconds * static_cast<double>(rate)));
    if (newFrame < 0) newFrame = 0;
    if (audioStreamTotalFrames > 0 &&
        newFrame >= static_cast<int64_t>(audioStreamTotalFrames)) {
        newFrame = static_cast<int64_t>(audioStreamTotalFrames) - 1;
    }
    if (ma_decoder_seek_to_pcm_frame(&audioStreamDecoder,
                                     static_cast<ma_uint64>(newFrame)) != MA_SUCCESS)
        return;
    audioStreamCursor = static_cast<uint64_t>(newFrame);
    audioRampInSamplesRemaining = kAudioRampInSamples;
}

double CassetteDevice::getPlaybackPositionSeconds() const
{
    if (!audioStreamMode) return 0.0;
    std::lock_guard<std::mutex> lock(audioStreamMutex);
    if (audioOutputSampleRate == 0) return 0.0;
    return static_cast<double>(audioStreamCursor) / static_cast<double>(audioOutputSampleRate);
}

double CassetteDevice::getPlaybackTotalSeconds() const
{
    if (!audioStreamMode) return 0.0;
    std::lock_guard<std::mutex> lock(audioStreamMutex);
    if (audioOutputSampleRate == 0) return 0.0;
    return static_cast<double>(audioStreamTotalFrames) / static_cast<double>(audioOutputSampleRate);
}

void CassetteDevice::queueAudioSegment(uint32_t cycles, bool level)
{
    if (!audioAvailable || cycles == 0) return;

    const double totalSamples = audioSampleRemainder +
        (static_cast<double>(cycles) * static_cast<double>(audioOutputSampleRate) /
         realtimeTimebaseHz_.load(std::memory_order_relaxed));
    const uint32_t sampleCount = static_cast<uint32_t>(totalSamples);
    audioSampleRemainder = totalSamples - static_cast<double>(sampleCount);

    if (sampleCount == 0) return;

    const float sampleValue = level ? 0.22f : -0.22f;
    std::lock_guard<std::mutex> lock(audioMutex);
    if (!audioQueue.empty() && audioQueue.back().sampleValue == sampleValue) {
        audioQueue.back().remainingSamples += sampleCount;
    } else {
        audioQueue.push_back({sampleCount, sampleValue});
    }

    static constexpr size_t kMaxQueuedSegments = 8192;
    while (audioQueue.size() > kMaxQueuedSegments) audioQueue.pop_front();
}

void CassetteDevice::advancePlayback(uint32_t cycles)
{
    if (rewinding) { advanceRewind(cycles); return; }
    if (!playbackActive || loadedDurations.empty() || cycles == 0) return;
    if (playbackPaused.load(std::memory_order_acquire)) return;

    uint64_t remaining = cycles;
    while (remaining > 0 && playbackActive) {
        if (cyclesUntilInputToggle == 0) {
            if (playbackIndex >= loadedDurations.size()) {
                playbackActive = false;
                break;
            }
            cyclesUntilInputToggle = std::max<uint32_t>(1, loadedDurations[playbackIndex++]);
        }
        if (remaining < cyclesUntilInputToggle) {
            cyclesUntilInputToggle -= remaining;
            break;
        }
        remaining -= cyclesUntilInputToggle;
        queueAudioSegment(std::max<uint32_t>(1, loadedDurations[playbackIndex - 1]),
                          inputLevel);
        cyclesUntilInputToggle = 0;
        inputLevel = !inputLevel;
        if (playbackIndex >= loadedDurations.size()) playbackActive = false;
    }
}

void CassetteDevice::advanceRewind(uint32_t cycles)
{
    if (playbackPaused.load(std::memory_order_acquire)) return;
    if (!loadedTapeReady || loadedDurations.empty() || playbackIndex == 0) {
        rewinding = false;
        rewCarryCycles = 0;
        resetPlaybackState();
        return;
    }
    uint64_t budget = static_cast<uint64_t>(cycles) * kRewSpeedFactor + rewCarryCycles;
    while (playbackIndex > 0) {
        const uint32_t segDur = std::max<uint32_t>(1, loadedDurations[playbackIndex - 1]);
        if (budget < segDur) {
            rewCarryCycles = budget;
            return;
        }
        budget -= segDur;
        --playbackIndex;
        inputLevel = !inputLevel;
    }
    resetPlaybackState();
    clearLiveAudioState();
}

void CassetteDevice::beginRecordingIfNeeded()
{
    if (!recordingStarted) {
        clearLiveAudioState();
        recordedInitialLevel = outputLevel;
        lastOutputToggleCycle = currentCycle;
        recordingStarted = true;
    }
}

uint8_t CassetteDevice::toggleOutput()
{
    beginRecordingIfNeeded();

    if (currentCycle > lastOutputToggleCycle) {
        const uint64_t delta = currentCycle - lastOutputToggleCycle;
        if (!(recordedDurations.empty() && delta == 0)) {
            const uint32_t clamped = static_cast<uint32_t>(
                std::min<uint64_t>(UINT32_MAX, std::max<uint64_t>(1, delta)));
            if (recordedDurations.size() < kMaxRecordedTransitions)
                recordedDurations.push_back(clamped);
            else
                recordingOverflow = true;
            if (clamped > kMaxRealtimeGapCycles) {
                clearLiveAudioState();
            } else {
                queueAudioSegment(clamped, outputLevel);
            }
        }
    }

    lastOutputToggleCycle = currentCycle;
    outputLevel = !outputLevel;
    return outputLevel ? 0x80 : 0x00;
}

void CassetteDevice::armPlaybackAtStart()
{
    playbackIndex = 0;
    cyclesUntilInputToggle = 0;
    inputLevel = loadedInitialLevel;
    const bool becameActive = loadedTapeReady && !loadedDurations.empty();
    playbackActive = becameActive;
    playbackArmed  = false;
    clearLiveAudioState();
}

uint8_t CassetteDevice::readTapeInput()
{
    // During REW, freeze the tape input — REW flips inputLevel as it walks
    // playbackIndex backward, the read sees that frozen state.
    if (rewinding) return inputLevel ? 0x80 : 0x00;

    // Leader-preservation (POM2-only, opt-in): if the Monitor's READ
    // routine ($FEFD) hasn't polled $C060 for a long while, assume the
    // user was typing in BASIC / Monitor (which doesn't touch the
    // cassette input). Rewind + reactivate so the next READ sees the
    // leader from the start and can synchronise to the 770 Hz sync
    // tone. MAME never rewinds; some custom loaders (Penguin Software
    // fast loaders, etc.) poll $C060 sporadically and are broken by
    // this re-arm. Gated behind `autoRewindEnabled`, default off.
    constexpr uint64_t kLeaderRewindGapCycles = POM2_CPU_CLOCK_HZ / 2;  // 500 ms
    const bool leaderRewind =
        autoRewindEnabled &&
        loadedTapeReady && !loadedDurations.empty() && playbackIndex > 0 &&
        (currentCycle - lastTapeInputCycle) > kLeaderRewindGapCycles;
    if (leaderRewind || playbackArmed) armPlaybackAtStart();
    lastTapeInputCycle = currentCycle;
    return inputLevel ? 0x80 : 0x00;
}

void CassetteDevice::rewindTape()
{
    if (audioStreamMode) {
        std::lock_guard<std::mutex> lock(audioStreamMutex);
        if (audioStreamDecoderOpen) ma_decoder_seek_to_pcm_frame(&audioStreamDecoder, 0);
        audioStreamCursor = 0;
        playbackActive = false;
        clearLiveAudioState();
        return;
    }
    if (!loadedTapeReady || loadedDurations.empty() || playbackIndex == 0) {
        resetPlaybackState();
        stopPulseAudio();
        return;
    }
    rewinding = true;
    rewCarryCycles = 0;
    playbackActive = false;
    playbackArmed  = false;
    stopPulseAudio();
}

void CassetteDevice::playTape()
{
    if (!loadedTapeReady) return;
    playbackPaused.store(false, std::memory_order_release);
    if (audioStreamMode) {
        std::lock_guard<std::mutex> lock(audioStreamMutex);
        if (!audioStreamDecoderOpen) return;
        playbackActive = true;
        playbackArmed  = false;
        audioRampInSamplesRemaining = kAudioRampInSamples;
        return;
    }
    if (loadedDurations.empty()) return;
    // Pulse-mode PLAY: arms the deck but doesn't start advancing pulses.
    // The tape only begins moving when the Monitor's READ routine reads
    // $C060 for the first time (readTapeInput's armed→active transition).
    // Without this, advancePlayback would chew through the whole tape on
    // every CPU slice regardless of whether the routine was actually
    // reading — a 30 s tape would disappear in <1 s at MAX speed while
    // the user was typing the address range.
    resetPlaybackState();
    playbackArmed = true;
}

void CassetteDevice::stopTape()
{
    playbackActive = false;
    playbackArmed  = false;
    rewinding      = false;
    rewCarryCycles = 0;
    cyclesUntilInputToggle = 0;
    stopPulseAudio();
}

void CassetteDevice::ejectTape()
{
    closeAudioStream();
    stopPulseAudio();
    audioStreamMode = false;
    loadedDurations.clear();
    loadedTapePath.clear();
    loadInfo.clear();
    loadedTapeReady    = false;
    loadedInitialLevel = false;
    resetPlaybackState();
    fireClickIfModeChanged();
}

void CassetteDevice::clearRecordedTape()
{
    recordedDurations.clear();
    recordingOverflow = false;
    recordedInitialLevel = outputLevel;
    recordingStarted = false;
    lastOutputToggleCycle = 0;
    clearLiveAudioState();
}

bool CassetteDevice::loadPlaybackDurations(std::vector<uint32_t> durations,
                                           bool initialLevel,
                                           const std::string& path)
{
    if (durations.empty()) {
        lastError = "Tape file does not contain any signal transitions";
        return false;
    }
    // Candidate parsing happens before this commit point. Only now retire an
    // existing streaming decoder, so a corrupt replacement leaves the old
    // tape fully playable.
    closeAudioStream();
    stopPulseAudio();
    loadedDurations    = std::move(durations);
    loadedInitialLevel = initialLevel;
    loadedTapePath     = path;
    loadedTapeReady    = true;
    audioStreamMode    = false;
    resetPlaybackState();
    fireClickIfModeChanged();
    lastError.clear();
    return true;
}

bool CassetteDevice::loadTape(const std::string& path)
{
    const std::string ext = lowerExtension(path);
    const std::string oldInfo = loadInfo;
    bool ok = false;
    if (ext == ".aci") ok = loadAciTape(path);

    // Default: program tape via pulse extraction. .wav routes through the
    // hand-rolled WAV loader (no decoder dependency); the rest go through
    // miniaudio (mp3/ogg/flac).
    else if (ext == ".wav") ok = loadWavTape(path);
    else if (ext == ".mp3" || ext == ".ogg" || ext == ".flac")
        ok = loadMiniaudioTape(path);
    else
        lastError = "Unsupported tape extension (expected .aci/.wav/.mp3/.ogg/.flac).";
    if (ok) loadInfo = lookupTapeInfo(path);
    else loadInfo = oldInfo;
    return ok;
}

bool CassetteDevice::saveTape(const std::string& path) const
{
    const std::string ext = lowerExtension(path);
    if (ext == ".wav") return saveWavTape(path);
    return saveAciTape(path);
}

bool CassetteDevice::loadAciTape(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { lastError = "Cannot open tape file: " + path; return false; }
    uint8_t header[16]{};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        std::memcmp(header, kAciMagic, 8) != 0) {
        lastError = "Invalid .aci tape file";
        return false;
    }
    if (header[8] != 1) {
        lastError = "Unsupported .aci tape version";
        return false;
    }
    const bool initialLevel = header[9] != 0;
    const uint32_t count = readLe32(header + 12);
    if (count > kMaxRecordedTransitions) {
        lastError = ".aci tape exceeds the transition limit";
        return false;
    }

    std::vector<uint32_t> durations;
    durations.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t raw[4];
        file.read(reinterpret_cast<char*>(raw), sizeof(raw));
        if (file.gcount() != static_cast<std::streamsize>(sizeof(raw))) {
            lastError = "Truncated .aci tape file";
            return false;
        }
        durations.push_back(std::max<uint32_t>(1, readLe32(raw)));
    }
    return loadPlaybackDurations(std::move(durations), initialLevel, path);
}

bool CassetteDevice::saveAciTape(const std::string& path) const
{
    if (recordedDurations.empty()) {
        lastError = "No cassette output has been recorded yet";
        return false;
    }
    if (recordingOverflow) {
        lastError = "Cassette recording exceeded the transition limit";
        return false;
    }
    if (!writeTapeAtomic(path, lastError, [&](std::ofstream& file) {
            file.write(kAciMagic, 8);
            file.put(1);
            file.put(recordedInitialLevel ? 1 : 0);
            file.put(0); file.put(0);
            writeLe32(file, static_cast<uint32_t>(recordedDurations.size()));
            for (uint32_t d : recordedDurations) writeLe32(file, d);
        })) return false;

    lastError.clear();
    return true;
}

bool CassetteDevice::loadWavTape(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { lastError = "Cannot open tape file: " + path; return false; }
    // Bounded streaming read, including non-seekable/special files. A prior
    // seek/tell gate still fed an endless FIFO or /dev/zero to an unbounded
    // istreambuf_iterator when tellg() returned -1.
    constexpr size_t kMaxTapeBytes = 64u * 1024u * 1024u;
    std::vector<uint8_t> bytes;
    bytes.reserve(64u * 1024u);
    uint8_t readChunk[64u * 1024u];
    while (file) {
        file.read(reinterpret_cast<char*>(readChunk), sizeof(readChunk));
        const size_t got = static_cast<size_t>(file.gcount());
        if (got > kMaxTapeBytes - bytes.size()) {
            lastError = "WAV tape exceeds 64 MiB: " + path;
            return false;
        }
        bytes.insert(bytes.end(), readChunk, readChunk + got);
        if (got < sizeof(readChunk)) break;
    }
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        lastError = "Invalid WAV file";
        return false;
    }

    uint16_t audioFormat = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t* dataChunk = nullptr;
    uint32_t dataSize = 0;

    size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const uint8_t* chunk = bytes.data() + offset;
        const uint32_t chunkSize = readLe32(chunk + 4);
        offset += 8;
        if (chunkSize > bytes.size() - offset) break;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
            audioFormat   = readLe16(bytes.data() + offset + 0);
            channels      = readLe16(bytes.data() + offset + 2);
            sampleRate    = readLe32(bytes.data() + offset + 4);
            bitsPerSample = readLe16(bytes.data() + offset + 14);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            dataChunk = bytes.data() + offset;
            dataSize  = chunkSize;
        }
        const size_t padded = static_cast<size_t>(chunkSize) + (chunkSize & 1u);
        if (padded > bytes.size() - offset) break;
        offset += padded;
    }
    if (!dataChunk || channels == 0 || sampleRate == 0) {
        lastError = "WAV file is missing format or data chunks";
        return false;
    }
    if (audioFormat != 1 && audioFormat != 3) {
        lastError = "Unsupported WAV format (only PCM and float are supported)";
        return false;
    }
    const bool supportedSamples =
        (audioFormat == 1 && (bitsPerSample == 8 || bitsPerSample == 16)) ||
        (audioFormat == 3 && bitsPerSample == 32);
    if (!supportedSamples) {
        lastError = "Only WAV PCM 8/16-bit and float32 are supported";
        return false;
    }
    const size_t bytesPerSample = bitsPerSample / 8;
    if (bytesPerSample == 0 || dataSize < bytesPerSample * channels) {
        lastError = "Unsupported WAV sample format";
        return false;
    }

    const size_t frames = dataSize / (bytesPerSample * channels);
    PcmDurationDecoder decoded;
    decoded.sampleRate = sampleRate;
    decoded.durations.reserve(std::min<size_t>(frames / 16,
                                               kMaxRecordedTransitions));
    for (size_t f = 0; f < frames; ++f) {
        float mixed = 0.0f;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            const uint8_t* p = dataChunk + (f * channels + ch) * bytesPerSample;
            float v = 0.0f;
            if      (audioFormat == 1 && bitsPerSample == 8)  v = (static_cast<int>(p[0]) - 128) / 128.0f;
            else if (audioFormat == 1 && bitsPerSample == 16) v = static_cast<float>(static_cast<int16_t>(readLe16(p))) / 32768.0f;
            else if (audioFormat == 3 && bitsPerSample == 32) std::memcpy(&v, p, 4);
            mixed += v;
        }
        const float mono = mixed / static_cast<float>(channels);
        if (!consumePcm(decoded, &mono, 1, lastError,
                        kMaxRecordedTransitions)) return false;
    }
    if (!finishPcm(decoded, lastError, kMaxRecordedTransitions)) return false;
    return loadPlaybackDurations(std::move(decoded.durations),
                                 decoded.initialLevel, path);
}

bool CassetteDevice::pcmToDurations(const std::vector<float>& mono,
                                    uint32_t sampleRate,
                                    std::vector<uint32_t>& outDurations,
                                    bool& outInitialLevel,
                                    std::string& outErr)
{
    outDurations.clear();
    if (mono.empty())    { outErr = "Audio file does not contain samples"; return false; }
    if (sampleRate == 0) { outErr = "Audio file has an invalid sample rate"; return false; }
    PcmDurationDecoder decoded;
    decoded.sampleRate = sampleRate;
    if (!consumePcm(decoded, mono.data(), mono.size(), outErr,
                    kMaxRecordedTransitions) ||
        !finishPcm(decoded, outErr, kMaxRecordedTransitions)) return false;
    outInitialLevel = decoded.initialLevel;
    outDurations = std::move(decoded.durations);
    return true;
}

bool CassetteDevice::loadAudioStream(const std::string& path)
{
    closeAudioStream();
    loadedDurations.clear();
    std::lock_guard<std::mutex> lock(audioStreamMutex);

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, audioOutputSampleRate);
    if (ma_decoder_init_file(path.c_str(), &cfg, &audioStreamDecoder) != MA_SUCCESS) {
        lastError = "Cannot decode audio: " + path;
        return false;
    }
    audioStreamDecoderOpen = true;
    audioStreamCursor      = 0;
    ma_uint64 total = 0;
    if (ma_decoder_get_length_in_pcm_frames(&audioStreamDecoder, &total) != MA_SUCCESS) total = 0;
    audioStreamTotalFrames = total;
    audioStreamMode        = true;
    loadedTapePath         = path;
    loadedTapeReady        = true;
    playbackArmed          = false;
    playbackActive         = false;
    loadedInitialLevel     = false;
    stopPulseAudio();
    fireClickIfModeChanged();
    lastError.clear();
    return true;
}

void CassetteDevice::closeAudioStream()
{
    std::lock_guard<std::mutex> lock(audioStreamMutex);
    if (audioStreamDecoderOpen) {
        ma_decoder_uninit(&audioStreamDecoder);
        audioStreamDecoderOpen = false;
    }
    audioStreamCursor      = 0;
    audioStreamTotalFrames = 0;
}

bool CassetteDevice::loadMiniaudioTape(const std::string& path)
{
    // 30-minute cap — same as POM1.
    static constexpr uint64_t kMaxFrames = 30ull * 60ull * 96000ull;

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &cfg, &decoder) != MA_SUCCESS) {
        lastError = "Cannot decode audio file: " + path;
        return false;
    }

    const uint32_t sampleRate = decoder.outputSampleRate;
    if (sampleRate == 0) {
        ma_decoder_uninit(&decoder);
        lastError = "Decoded audio reports an invalid sample rate";
        return false;
    }

    constexpr size_t kChunkFrames = 4096;
    float chunk[kChunkFrames];
    uint64_t totalFrames = 0;
    PcmDurationDecoder decoded;
    decoded.sampleRate = sampleRate;
    while (totalFrames < kMaxFrames) {
        ma_uint64 framesRead = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&decoder, chunk, kChunkFrames, &framesRead);
        if (framesRead == 0) {
            if (r != MA_SUCCESS && r != MA_AT_END) {
                ma_decoder_uninit(&decoder);
                lastError = "Error while decoding audio file: " + path;
                return false;
            }
            break;
        }
        if (!consumePcm(decoded, chunk, static_cast<size_t>(framesRead),
                        lastError, kMaxRecordedTransitions)) {
            ma_decoder_uninit(&decoder);
            return false;
        }
        totalFrames += framesRead;
        if (r != MA_SUCCESS && r != MA_AT_END) {
            ma_decoder_uninit(&decoder);
            lastError = "Error while decoding audio file: " + path;
            return false;
        }
        if (r == MA_AT_END) break;
    }
    ma_decoder_uninit(&decoder);

    if (totalFrames >= kMaxFrames) {
        lastError = "Audio file exceeds 30-minute tape limit";
        return false;
    }

    if (!finishPcm(decoded, lastError, kMaxRecordedTransitions)) return false;
    return loadPlaybackDurations(std::move(decoded.durations),
                                 decoded.initialLevel, path);
}

bool CassetteDevice::saveWavTape(const std::string& path) const
{
    if (recordedDurations.empty()) {
        lastError = "No cassette output has been recorded yet";
        return false;
    }
    if (recordingOverflow) {
        lastError = "Cassette recording exceeded the transition limit";
        return false;
    }
    constexpr uint64_t kMaxWavSamples =
        static_cast<uint64_t>(kWavFileSampleRate) * 30u * 60u;
    uint64_t sampleCount = kWavFileSampleRate / 10;
    for (uint32_t d : recordedDurations) {
        const uint64_t n = std::max<uint64_t>(1, static_cast<uint64_t>(
            std::llround(static_cast<double>(d) * kWavFileSampleRate /
                         static_cast<double>(kTapeFileTimebaseHz))));
        if (n > kMaxWavSamples - sampleCount) {
            lastError = "Recording exceeds the 30-minute WAV limit";
            return false;
        }
        sampleCount += n;
    }
    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<size_t>(sampleCount));
    bool level = recordedInitialLevel;
    for (uint32_t d : recordedDurations) {
        const uint32_t n = std::max<uint32_t>(1, static_cast<uint32_t>(
            std::llround(static_cast<double>(d) * static_cast<double>(kWavFileSampleRate) /
                         static_cast<double>(kTapeFileTimebaseHz))));
        const int16_t s = level ? 14000 : -14000;
        pcm.insert(pcm.end(), n, s);
        level = !level;
    }
    pcm.insert(pcm.end(), kWavFileSampleRate / 10, level ? 14000 : -14000);

    const size_t fullSize = pcm.size() * sizeof(int16_t);
    if (fullSize > UINT32_MAX - 36) {
        lastError = "Recording too large to save as WAV";
        return false;
    }
    const uint32_t dataSize = static_cast<uint32_t>(fullSize);
    const uint32_t riffSize = 36 + dataSize;

    if (!writeTapeAtomic(path, lastError, [&](std::ofstream& f) {
            f.write("RIFF", 4); writeLe32(f, riffSize);
            f.write("WAVE", 4);
            f.write("fmt ", 4); writeLe32(f, 16);
            writeLe16(f, 1); writeLe16(f, 1);
            writeLe32(f, kWavFileSampleRate);
            writeLe32(f, kWavFileSampleRate * sizeof(int16_t));
            writeLe16(f, sizeof(int16_t)); writeLe16(f, 16);
            f.write("data", 4); writeLe32(f, dataSize);
            f.write(reinterpret_cast<const char*>(pcm.data()),
                    static_cast<std::streamsize>(dataSize));
        })) return false;

    lastError.clear();
    return true;
}
