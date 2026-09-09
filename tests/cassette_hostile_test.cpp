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

// Cassette hostile-input + I/O-fence regression test (2026-09-09 tape hunt).
//
// Four defects, all found with the SAME waveform driven four ways:
//
//  1. WAVE_FORMAT_EXTENSIBLE ($FFFE) was refused outright. It is what
//     ffmpeg / Audacity / every Windows capture app emit for float32, for
//     more than two channels, and whenever a channel mask is written; the
//     real encoding lives in the first two bytes of the SubFormat GUID.
//
//  2. A `data` header claiming more bytes than the file holds — the shape of
//     BOTH a truncated recording and every WAV streamed to a pipe, whose
//     size field is a placeholder the writer never seeks back to patch — was
//     rejected as "missing format or data chunks" with a perfectly readable
//     waveform sitting right there.
//
//  3. The zero-crossing detector compared against a hard 0. A line-in
//     capture rides on the sound card's DC bias, and once |bias| exceeds the
//     signal amplitude a sign-only comparator finds NO crossings: loadTape
//     returned TRUE with one transition and the tape played silence.
//
//  4. A capture that auto-starts on the first $C020 access came out 180° out
//     of phase with the same waveform captured after REC (armRecording):
//     `recordedInitialLevel` names the level of durations[0], and the
//     auto-start path left it at the PRE-toggle value.
//
//  5. $C020 is fenced off the //c (it is ROMBANK there, and a //c has no
//     cassette port) — but $C060 was not. On a //c that address is the
//     40/80-column switch, and reading it called into the deck: with a tape
//     loaded and PLAY armed it consumed the arm and STARTED the transport.

#include "CassetteDevice.h"
#include "M6502.h"
#include "Memory.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

int failures = 0;
void expect(bool ok, const std::string& what)
{
    if (ok) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
}

std::string firstExisting(const std::string& rel)
{
    for (const std::string& p : { rel, "../" + rel, "../../" + rel })
        if (fs::exists(p)) return p;
    return {};
}

void u16v(std::vector<uint8_t>& o, uint16_t v)
{
    o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8));
}
void u32v(std::vector<uint8_t>& o, uint32_t v)
{
    for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
void tagv(std::vector<uint8_t>& o, const char* t)
{
    for (int i = 0; i < 4; ++i) o.push_back(uint8_t(t[i]));
}

std::string tmpPath(const char* name)
{
    return (fs::temp_directory_path() / name).string();
}

std::string writeBlob(const char* name, const std::vector<uint8_t>& w)
{
    const std::string p = tmpPath(name);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
    return p;
}

// The reference waveform: 40 square half-cycles of 100 samples each. A
// correct decode is 39 sign changes + the flushed tail = 40 transitions
// (the tail flush itself is pinned by cassette_wav_tail).
constexpr int kHalves = 40;
constexpr int kSamplesPerHalf = 100;
constexpr size_t kExpectedTransitions = 40;

// 16-bit mono PCM at 44100, optionally as WAVE_FORMAT_EXTENSIBLE and/or with
// a forced (wrong) `data` size and/or a DC bias in units of full scale.
std::vector<uint8_t> buildWav(bool extensible, int32_t forcedDataSize,
                              float dcOffset)
{
    std::vector<uint8_t> data;
    bool lvl = true;
    for (int h = 0; h < kHalves; ++h) {
        const float v = (lvl ? 0.45f : -0.45f) + dcOffset;
        for (int i = 0; i < kSamplesPerHalf; ++i)
            u16v(data, uint16_t(int16_t(v * 32000.0f)));
        lvl = !lvl;
    }
    std::vector<uint8_t> fmt;
    u16v(fmt, extensible ? uint16_t(0xFFFE) : uint16_t(1));
    u16v(fmt, 1);                    // mono
    u32v(fmt, 44100);
    u32v(fmt, 44100 * 2);
    u16v(fmt, 2);                    // block align
    u16v(fmt, 16);                   // bits
    if (extensible) {
        u16v(fmt, 22);               // cbSize
        u16v(fmt, 16);               // wValidBitsPerSample
        u32v(fmt, 4);                // dwChannelMask (SPEAKER_FRONT_CENTER)
        u16v(fmt, 1); u16v(fmt, 0);  // SubFormat = KSDATAFORMAT_SUBTYPE_PCM
        const uint8_t guid[12] = { 0x00,0x00,0x10,0x00,0x80,0x00,
                                   0x00,0xAA,0x00,0x38,0x9B,0x71 };
        fmt.insert(fmt.end(), guid, guid + 12);
    }
    std::vector<uint8_t> body;
    tagv(body, "fmt "); u32v(body, uint32_t(fmt.size()));
    body.insert(body.end(), fmt.begin(), fmt.end());
    tagv(body, "data");
    u32v(body, forcedDataSize >= 0 ? uint32_t(forcedDataSize)
                                   : uint32_t(data.size()));
    body.insert(body.end(), data.begin(), data.end());

    std::vector<uint8_t> w;
    tagv(w, "RIFF"); u32v(w, uint32_t(4 + body.size())); tagv(w, "WAVE");
    w.insert(w.end(), body.begin(), body.end());
    return w;
}

size_t loadCount(const std::string& path, std::string& err)
{
    CassetteDevice c;
    if (!c.loadTape(path)) { err = c.getLastError(); return 0; }
    err.clear();
    return c.getLoadedTransitionCount();
}

// ── 1 + 2 + 3: the WAV loader ────────────────────────────────────────────
void testWavVariants()
{
    std::string err;

    const size_t ref = loadCount(writeBlob("pom2_ch_ref.wav",
                                           buildWav(false, -1, 0.0f)), err);
    expect(ref == kExpectedTransitions,
           "reference 16-bit mono WAV: " + std::to_string(ref) +
           " transitions (expected 40) " + err);

    const size_t ext = loadCount(writeBlob("pom2_ch_ext.wav",
                                           buildWav(true, -1, 0.0f)), err);
    expect(ext == kExpectedTransitions,
           "WAVE_FORMAT_EXTENSIBLE refused or mis-decoded (" +
           std::to_string(ext) + " transitions): " + err);

    // A writer that streamed to a pipe leaves the placeholder size behind.
    const size_t stream = loadCount(
        writeBlob("pom2_ch_stream.wav", buildWav(false, 0x7FFFFFFF, 0.0f)), err);
    expect(stream == kExpectedTransitions,
           "streamed/placeholder `data` size refused (" +
           std::to_string(stream) + " transitions): " + err);

    // A recording cut short must yield the transitions it DOES hold, not an
    // error blaming a data chunk that is plainly present.
    {
        auto w = buildWav(false, -1, 0.0f);
        w.resize(w.size() * 7 / 10);        // 70 % of the samples survive
        const size_t trunc = loadCount(writeBlob("pom2_ch_trunc.wav", w), err);
        expect(trunc >= 25 && trunc <= 29,
               "truncated WAV: " + std::to_string(trunc) +
               " transitions (expected ~28) " + err);
    }

    // DC bias larger than the signal amplitude: every sample is positive, so
    // a sign-only comparator sees one flat segment.
    const size_t dc = loadCount(writeBlob("pom2_ch_dc.wav",
                                          buildWav(false, -1, 0.50f)), err);
    expect(dc == kExpectedTransitions,
           "DC-biased WAV decoded as " + std::to_string(dc) +
           " transitions (expected 40) " + err);
}

// ── 4: recording polarity ────────────────────────────────────────────────
struct Aci { bool initialLevel = false; std::vector<uint32_t> durations; };

Aci readAci(const std::string& path)
{
    Aci a;
    std::ifstream f(path, std::ios::binary);
    uint8_t h[16]{};
    f.read(reinterpret_cast<char*>(h), 16);
    a.initialLevel = h[9] != 0;
    const uint32_t n = uint32_t(h[12] | (h[13] << 8) | (h[14] << 16) |
                                (uint32_t(h[15]) << 24));
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t d[4]{};
        if (!f.read(reinterpret_cast<char*>(d), 4)) break;
        a.durations.push_back(uint32_t(d[0] | (d[1] << 8) | (d[2] << 16) |
                                       (uint32_t(d[3]) << 24)));
    }
    return a;
}

// The line starts LOW, goes HIGH at cycle 1000, and alternates every 500.
void driveWaveform(CassetteDevice& c)
{
    c.advanceCycles(1000);
    for (int i = 0; i < 4; ++i) { c.toggleOutput(); c.advanceCycles(500); }
}

void testRecordingPolarity()
{
    CassetteDevice autoDeck;                  // capture auto-starts on $C020
    driveWaveform(autoDeck);
    const std::string ap = tmpPath("pom2_ch_auto.aci");
    expect(autoDeck.saveTape(ap), "auto-start capture did not save");
    const Aci autoAci = readAci(ap);

    CassetteDevice armedDeck;                 // REC pressed first
    armedDeck.armRecording();
    driveWaveform(armedDeck);
    const std::string rp = tmpPath("pom2_ch_armed.aci");
    expect(armedDeck.saveTape(rp), "REC-armed capture did not save");
    const Aci armedAci = readAci(rp);

    expect(armedAci.durations.size() == 4 && armedAci.durations[0] == 1000,
           "REC-armed capture lost the pre-toggle segment");
    expect(!armedAci.initialLevel,
           "REC-armed capture: the line is LOW before the first toggle");
    expect(autoAci.durations.size() == 3,
           "auto-started capture: expected 3 durations, got " +
           std::to_string(autoAci.durations.size()));

    // The two decks saw the SAME waveform. The auto-started file simply
    // starts one segment later, so segment i there must describe the same
    // physical level as segment i+1 of the armed file.
    for (size_t i = 0; i < autoAci.durations.size(); ++i) {
        const bool autoLevel  = autoAci.initialLevel  != ((i & 1u) != 0);
        const bool armedLevel = armedAci.initialLevel != (((i + 1) & 1u) != 0);
        expect(autoLevel == armedLevel,
               "auto-started capture is inverted at segment " +
               std::to_string(i));
        expect(autoAci.durations[i] == armedAci.durations[i + 1],
               "capture durations diverge at segment " + std::to_string(i));
    }
    // Belt and braces: the first recorded segment IS the high half-cycle.
    expect(autoAci.initialLevel,
           "auto-started capture: durations[0] is the HIGH half-cycle the "
           "first $C020 toggle established");
}

// ── 5: the //c must not reach the deck through $C060 ─────────────────────
void testIicC060IsNotTheDeck()
{
    const std::string rom = firstExisting("roms/apple2c-32Kv0.rom");
    if (rom.empty()) {
        std::printf("  (//c $C060 fence SKIPPED — no //c ROM)\n");
        return;
    }
    // A one-transition tape is enough: what is pinned is that the deck does
    // not MOVE, not what it would have played.
    CassetteDevice rec;
    rec.armRecording();
    for (int i = 0; i < 8; ++i) { rec.advanceCycles(500); rec.toggleOutput(); }
    const std::string tape = tmpPath("pom2_ch_iic.aci");
    expect(rec.saveTape(tape), "could not build the //c probe tape");

    Memory mem;
    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.setIIEMode(true);
    expect(mem.loadAppleIIRom(rom.c_str(), /*pickLower16KFor32K=*/true) != 0,
           "//c ROM did not load");
    mem.resetSoftSwitches();
    CassetteDevice deck;
    deck.setAudioAvailable(false);
    mem.setCassetteDevice(&deck);

    // $C02x is ROMBANK on a //c and must not record.
    deck.armRecording();
    for (int i = 0; i < 4; ++i) { mem.advanceCycles(100); (void)mem.memRead(0xC020); }
    expect(deck.getRecordedTransitionCount() == 0,
           "//c $C02x reached the cassette OUTPUT flip-flop");

    expect(deck.loadTape(tape), "//c probe tape did not load");
    deck.playTape();
    expect(deck.isPlaybackArmed() && !deck.isPlaybackActive(),
           "PLAY should arm, not start, a program tape");
    (void)mem.memRead(0xC060);
    (void)mem.memRead(0xC068);
    expect(deck.isPlaybackArmed() && !deck.isPlaybackActive(),
           "//c $C060/$C068 started the cassette transport — that address is "
           "the 40/80-column switch on a //c, not a comparator");

    // …and on a II+ it still IS the comparator: the fence must be //c-only.
    const std::string iip = firstExisting("roms/apple2p.rom");
    if (!iip.empty()) {
        Memory m2;
        M6502 c2(&m2);
        m2.setCpu(&c2);
        m2.clearRam();
        expect(m2.loadAppleIIRom(iip.c_str()) != 0, "II+ ROM did not load");
        CassetteDevice d2;
        d2.setAudioAvailable(false);
        m2.setCassetteDevice(&d2);
        expect(d2.loadTape(tape), "II+ probe tape did not load");
        d2.playTape();
        (void)m2.memRead(0xC060);
        expect(d2.isPlaybackActive(),
               "II+ $C060 no longer starts the transport (fence too wide)");
    }
}

}  // namespace

int main()
{
    testWavVariants();
    testRecordingPolarity();
    testIicC060IsNotTheDeck();
    if (failures) {
        std::printf("cassette_hostile FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("cassette_hostile OK\n");
    return 0;
}
