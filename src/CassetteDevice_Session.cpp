// POM2 — GPL-3.0-or-later
#include "CassetteDevice.h"
#include "SessionIO.h"
#include <algorithm>
#include <utility>

bool CassetteDevice::appendSessionState(std::vector<uint8_t>& out, std::string& error) const
{
    if (audioStreamMode) { error = "streamed audio cassettes cannot be embedded; use a program tape"; return false; }
    if (!loadedTapeReady && recordedDurations.empty() && !recordingStarted) return true;
    using namespace pom2::byteio;
    std::vector<uint8_t> b{'C','T','S',1};
    const uint32_t flags = uint32_t(outputLevel) | (uint32_t(recordedInitialLevel) << 1) |
        (uint32_t(recordingStarted) << 2) | (uint32_t(recordingOverflow) << 3) |
        (uint32_t(inputLevel) << 4) | (uint32_t(loadedInitialLevel) << 5) |
        (uint32_t(loadedTapeReady) << 6) | (uint32_t(playbackArmed) << 7) |
        (uint32_t(playbackActive.load()) << 8) | (uint32_t(rewinding) << 9) |
        (uint32_t(playbackPaused.load()) << 10);
    putU32(b, flags); putU64(b, currentCycle); putU64(b, lastOutputToggleCycle);
    putU64(b, lastTapeInputCycle); putU64(b, cyclesUntilInputToggle);
    putU32(b, static_cast<uint32_t>(playbackIndex)); putU64(b, rewCarryCycles);
    for (const auto* pulses : {&loadedDurations, &recordedDurations}) {
        if (pulses->size() > kMaxRecordedTransitions) { error = "cassette session exceeds transition limit"; return false; }
        putU32(b, static_cast<uint32_t>(pulses->size()));
        for (auto duration : *pulses) putU32(b, duration);
    }
    out.insert(out.end(), b.begin(), b.end()); return true;
}
bool CassetteDevice::loadSessionState(const std::vector<uint8_t>& bytes, std::string& error, bool apply)
{
    auto invalid = [&] { error = "invalid session cassette state"; return false; };
    if (bytes.empty()) {
        if (apply) { ejectTape(); reset(); detachedSession_ = true; }
        return true;
    }
    pom2::sessionio::Reader r(bytes.data(), bytes.size());
    if (r.u8() != 'C' || r.u8() != 'T' || r.u8() != 'S' || r.u8() != 1) return invalid();
    const auto flags = r.u32();
    auto u64 = [&] { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(r.u8()) << (8 * i); return v; };
    const auto cycle = u64(), outputCycle = u64(), inputCycle = u64(), remaining = u64();
    const auto index = r.u32(); const auto carry = u64();
    std::vector<uint32_t> loaded, recorded;
    for (auto* pulses : {&loaded, &recorded}) {
        const auto count = r.u32();
        if (count > kMaxRecordedTransitions || !r.need(size_t(count) * 4)) return invalid();
        pulses->reserve(count);
        for (uint32_t i = 0; i < count; ++i) pulses->push_back(r.u32());
    }
    if (!r.done() || (flags & ~2047u) || index > loaded.size() ||
        bool(flags & 64) != !loaded.empty() ||
        (!index && remaining) || (index && remaining > std::max<uint32_t>(1, loaded[index - 1])) ||
        ((flags & (128 | 256 | 512)) && loaded.empty()) ||
        (!index && carry) || (index && carry >= std::max<uint32_t>(1, loaded[index - 1]))) return invalid();
    if (!apply) return true;
    // No host filename/decoder survives. The saved pulse source is complete.
    closeAudioStream(); stopPulseAudio();
    loadedDurations = std::move(loaded); recordedDurations = std::move(recorded);
    currentCycle = cycle; lastOutputToggleCycle = outputCycle; lastTapeInputCycle = inputCycle;
    cyclesUntilInputToggle = remaining; playbackIndex = index; rewCarryCycles = carry;
    outputLevel = flags & 1; recordedInitialLevel = flags & 2; recordingStarted = flags & 4;
    recordingOverflow = flags & 8; inputLevel = flags & 16; loadedInitialLevel = flags & 32;
    loadedTapeReady = flags & 64; playbackArmed = flags & 128; playbackActive = bool(flags & 256);
    rewinding = flags & 512; playbackPaused = bool(flags & 1024);
    loadedTapePath = loadedTapeReady ? "(session tape)" : ""; loadInfo.clear();
    audioStreamMode = false; detachedSession_ = true; lastError.clear();
    return true;
}
