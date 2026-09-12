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

// The cassette stream decoder follows the host sample rate — bug hunt #19.
//
// A stream-mode tape is decoded by miniaudio into the rate the decoder was
// OPENED with (`ma_decoder_config_init(..., audioOutputSampleRate)` in
// `loadAudioStream`). `setAudioOutputSampleRate` only assigned the field, so
// once a decoder was open the rate was baked in: every later change — and
// AudioDevice issues one through `RateAware::setSampleRate` whenever it
// negotiates the device's real rate, which on this machine is 48000 against
// the 44100 default — left the decoder converting to the OLD rate while all
// the cassette's timing used the NEW one.
//
// The audible result is a tape that plays at the wrong speed: 44100/48000 =
// 8.8 % slow, which drags the 770 Hz / 2 kHz half-cycles a guest LOAD
// measures straight out of their windows. An I/O ERROR on a tape that is
// fine.
//
// The invariant that catches it needs no ear: a tape is a fixed number of
// SECONDS. Changing the host rate must not change how long the tape is.

#include "CassetteDevice.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

void putU16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8));
}
void putU32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}

// Exactly one second of 16-bit mono PCM at 44100 Hz: a 1 kHz square, which
// is a plausible tape signal and gives the decoder real transitions to find.
std::string writeOneSecondWav(const fs::path& path)
{
    constexpr uint32_t rate = 44100;
    std::vector<int16_t> samples(rate);
    for (uint32_t i = 0; i < rate; ++i)
        samples[i] = ((i / 22) % 2) ? 16000 : -16000;

    const uint32_t dataSize = static_cast<uint32_t>(samples.size() * 2);
    std::vector<uint8_t> w;
    w.insert(w.end(), {'R','I','F','F'}); putU32(w, 36 + dataSize);
    w.insert(w.end(), {'W','A','V','E'});
    w.insert(w.end(), {'f','m','t',' '}); putU32(w, 16);
    putU16(w, 1);             // PCM
    putU16(w, 1);             // mono
    putU32(w, rate);
    putU32(w, rate * 2);      // byte rate
    putU16(w, 2);             // block align
    putU16(w, 16);            // bits/sample
    w.insert(w.end(), {'d','a','t','a'}); putU32(w, dataSize);
    for (int16_t s : samples) putU16(w, static_cast<uint16_t>(s));

    std::ofstream f(path, std::ios::binary);
    assert(f && "open temp WAV");
    f.write(reinterpret_cast<const char*>(w.data()),
            static_cast<std::streamsize>(w.size()));
    return path.string();
}

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (ok) { std::printf("  ok: %s\n", what); return; }
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}

}  // namespace

int main()
{
    const fs::path wav =
        fs::temp_directory_path() / "pom2_cassette_stream_rate.wav";
    writeOneSecondWav(wav);

    CassetteDevice tape;
    tape.setAudioAvailable(true);
    tape.setAudioOutputSampleRate(44100);
    if (!tape.loadAudioStream(wav.string())) {
        std::printf("SKIP cassette_stream_rate: miniaudio cannot decode the "
                    "generated WAV (%s)\n", tape.getLastError().c_str());
        std::error_code ec; fs::remove(wav, ec);
        return 77;
    }

    const double lengthAt44 = tape.getPlaybackTotalSeconds();
    check(std::fabs(lengthAt44 - 1.0) < 0.01,
          "a one-second tape reads as one second at 44100");
    check(tape.getLoadedTransitionCount() == 44100,
          "…which is 44100 frames at 44100 Hz");

    // The device negotiates its real rate. On this machine that is 48000.
    tape.setAudioOutputSampleRate(48000);

    const double lengthAt48 = tape.getPlaybackTotalSeconds();
    std::printf("  tape length: %.4f s at 44100, %.4f s at 48000\n",
                lengthAt44, lengthAt48);
    // THE defect. Without the reopen the frame count stays 44100 while the
    // divisor becomes 48000, so the tape claims to be 0.919 s long and plays
    // 8.8 % fast.
    check(std::fabs(lengthAt48 - 1.0) < 0.01,
          "changing the host rate does not change how long the tape is");
    check(tape.getLoadedTransitionCount() == 48000,
          "the decoder was reopened AT the new rate (48000 frames)");

    // The reopen goes through the mount path, which disarms the deck; a rate
    // change is not a mount, so the loaded tape must still be there.
    check(tape.isAudioStreamMode() && tape.hasLoadedTape(),
          "the tape is still loaded after the reopen");
    check(tape.getLoadedTapePath() == wav.string(),
          "…and it is still the same tape");

    // A rate that changes nothing must not churn the decoder either.
    tape.setAudioOutputSampleRate(48000);
    check(tape.getLoadedTransitionCount() == 48000,
          "setting the same rate again is a no-op");

    std::error_code ec; fs::remove(wav, ec);
    if (g_failures) return 1;
    std::puts("cassette_stream_rate OK");
    return 0;
}
