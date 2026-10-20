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

// CassetteDevice WAV tail-flush regression test.
//
// PCM pulse extraction emitted a transition-duration only ON a sign change, so the
// held segment after the LAST zero-crossing (trailing leader / final
// half-bit — saveWavTape appends a 0.1 s trailer) was never converted,
// truncating the recovered transition stream by one segment. This builds a
// tiny WAV whose level pattern is +,−,+ and ends held on +, so the correct
// transition count is 3 (the two mid sign-changes PLUS the final tail);
// the bug produced 2.

#include "CassetteDevice.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
namespace fs = std::filesystem;

void putU16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8));
}
void putU32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}

// Minimal 16-bit PCM mono WAV from int16 samples.
std::string writeWav(const std::vector<int16_t>& samples) {
    const uint32_t rate = 44100;
    const uint32_t dataSize = static_cast<uint32_t>(samples.size() * 2);
    std::vector<uint8_t> w;
    w.insert(w.end(), {'R','I','F','F'}); putU32(w, 36 + dataSize);
    w.insert(w.end(), {'W','A','V','E'});
    w.insert(w.end(), {'f','m','t',' '}); putU32(w, 16);
    putU16(w, 1);            // PCM
    putU16(w, 1);            // mono
    putU32(w, rate);
    putU32(w, rate * 2);     // byte rate
    putU16(w, 2);            // block align
    putU16(w, 16);           // bits/sample
    w.insert(w.end(), {'d','a','t','a'}); putU32(w, dataSize);
    for (int16_t s : samples) putU16(w, static_cast<uint16_t>(s));

    const auto p = fs::temp_directory_path() / "pom2_cassette_tail.wav";
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp WAV");
    f.write(reinterpret_cast<const char*>(w.data()),
            static_cast<std::streamsize>(w.size()));
    return p.string();
}

}  // namespace

int main()
{
    // Level pattern +,−,+ (4 samples each), ending held on +.
    constexpr int16_t HI = 16000, LO = -16000;
    std::vector<int16_t> samples;
    for (int i = 0; i < 4; ++i) samples.push_back(HI);   // initial level +
    for (int i = 0; i < 4; ++i) samples.push_back(LO);   // transition 1: +→−
    for (int i = 0; i < 4; ++i) samples.push_back(HI);   // transition 2: −→+, then held tail

    const std::string path = writeWav(samples);

    CassetteDevice cass;
    if (!cass.loadTape(path)) {
        std::printf("FAIL: loadTape(%s)\n", path.c_str());
        return 1;
    }
    const size_t n = cass.getLoadedTransitionCount();
    // 2 mid sign-changes + 1 flushed tail segment = 3. (Bug = 2.)
    if (n != 3) {
        std::printf("FAIL: transition count = %zu (expected 3 — tail not flushed?)\n", n);
        return 1;
    }
    std::printf("OK cassette_wav_tail (3 transitions incl. flushed tail)\n");

    // A corrupt replacement must not discard the valid tape already loaded.
    const std::string badPath = path + ".bad.wav";
    { std::ofstream bad(badPath, std::ios::binary); bad << "not a wav"; }
    if (cass.loadTape(badPath) || !cass.hasLoadedTape() ||
        cass.getLoadedTapePath() != path ||
        cass.getLoadedTransitionCount() != n) {
        std::printf("FAIL: invalid replacement destroyed current tape\n");
        return 1;
    }
    std::printf("OK invalid cassette replacement preserves current tape\n");

    // Leader auto-rewind (`cassette_auto_rewind`, 3f42efc) — POM2 convenience
    // that re-arms playback when the Monitor READ routine stops polling. It
    // MUST default OFF: a custom loader polling its own way would otherwise get
    // rewound underneath it. Pin the default + the toggle accessors.
    if (cass.isAutoRewindEnabled()) {
        std::printf("FAIL: auto-rewind must default OFF\n");
        return 1;
    }
    cass.setAutoRewind(true);
    if (!cass.isAutoRewindEnabled()) {
        std::printf("FAIL: setAutoRewind(true) had no effect\n");
        return 1;
    }
    cass.setAutoRewind(false);
    if (cass.isAutoRewindEnabled()) {
        std::printf("FAIL: setAutoRewind(false) had no effect\n");
        return 1;
    }
    std::printf("OK cassette auto-rewind (default-off + toggle)\n");

    // A single huge held level used to allocate hundreds of MiB before the
    // WAV-size guard ran. It must be rejected during the checked size pass.
    CassetteDevice huge;
    huge.advanceCycles(1);
    huge.toggleOutput();
    huge.advanceCycles(INT_MAX);
    huge.toggleOutput();
    const std::string hugePath = path + ".huge.wav";
    if (huge.saveTape(hugePath)) {
        std::printf("FAIL: oversized WAV recording was accepted\n");
        return 1;
    }
    std::printf("OK cassette oversized WAV rejected before allocation\n");

    // ACI's count field is untrusted. Reject it from the 16-byte header,
    // before reserving count*4 bytes or slurping the remainder of the file.
    const std::string hostileAci = path + ".hostile.aci";
    {
        std::vector<uint8_t> a{'P','O','M','1','A','C','I','1', 1, 0, 0, 0};
        putU32(a, 0xFFFFFFFFu);
        std::ofstream out(hostileAci, std::ios::binary);
        out.write(reinterpret_cast<const char*>(a.data()),
                  static_cast<std::streamsize>(a.size()));
    }
    if (huge.loadTape(hostileAci) ||
        huge.getLastError().find("transition limit") == std::string::npos) {
        std::printf("FAIL: hostile ACI count was not bounded\n");
        return 1;
    }
    std::printf("OK cassette ACI count rejected before allocation\n");

    // Both output formats publish through a sibling temp and only report
    // success after flush/close/rename.
    CassetteDevice recorded;
    recorded.advanceCycles(1);
    recorded.toggleOutput();
    recorded.advanceCycles(1000);
    recorded.toggleOutput();
    const std::string aciPath = path + ".recorded.aci";
    const std::string wavPath = path + ".recorded.wav";
    if (!recorded.saveTape(aciPath) || !recorded.saveTape(wavPath) ||
        std::filesystem::exists(aciPath + ".pom2tmp") ||
        std::filesystem::exists(wavPath + ".pom2tmp")) {
        std::printf("FAIL: atomic cassette save failed\n");
        return 1;
    }
    CassetteDevice roundTrip;
    if (!roundTrip.loadTape(aciPath)) {
        std::printf("FAIL: atomically saved ACI did not reload\n");
        return 1;
    }
    std::printf("OK cassette ACI/WAV atomic save round-trip\n");
    return 0;
}
