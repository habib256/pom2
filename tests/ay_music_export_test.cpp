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

// AUDIO-013 — the Mockingboard's music out of POM2, and back in.
//
// The VGM round trip is the acceptance: a real MockingboardCard is driven
// through its 6522s the way a music driver does, the capture is encoded, and
// a VGM READER WRITTEN HERE FROM THE SPECIFICATION (not POM2's encoder run
// backwards) must find the same register writes at the same 44.1 kHz sample
// positions. Then the YM5 frame view, the WAV writer and the capture ring.

#include "AyMusicExport.h"
#include "Mockingboard.h"
#include "WavRecorder.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

namespace aym = pom2::aymusic;
namespace fs = std::filesystem;

namespace {

// ── An independent VGM reader ([B] VGM 1.71 specification) ─────────────────
struct Event { uint64_t sample; int chip; int reg; int val; };
bool operator==(const Event& a, const Event& b)
{
    return std::tie(a.sample, a.chip, a.reg, a.val) == std::tie(b.sample, b.chip, b.reg, b.val);
}

uint32_t le32(const std::vector<uint8_t>& b, std::size_t at)
{
    return static_cast<uint32_t>(b[at]) | static_cast<uint32_t>(b[at + 1]) << 8 |
           static_cast<uint32_t>(b[at + 2]) << 16 | static_cast<uint32_t>(b[at + 3]) << 24;
}

struct Vgm { uint32_t ayClock = 0; bool dual = false; uint32_t totalSamples = 0;
             std::vector<Event> events; uint64_t endSample = 0; };

Vgm readVgm(const std::vector<uint8_t>& b)
{
    Vgm v;
    assert(b.size() >= 0x100);
    assert(b[0] == 'V' && b[1] == 'g' && b[2] == 'm' && b[3] == ' ');
    assert(le32(b, 0x04) + 4 == b.size() && "EOF offset");
    assert(le32(b, 0x08) >= 0x151 && "AY8910 needs VGM 1.51+");
    v.totalSamples = le32(b, 0x18);
    const uint32_t clk = le32(b, 0x74);
    v.ayClock = clk & 0x3FFFFFFF;
    v.dual = (clk & 0x40000000) != 0;
    std::size_t p = 0x34 + le32(b, 0x34);
    uint64_t now = 0;
    for (;;) {
        assert(p < b.size());
        const uint8_t cmd = b[p++];
        if (cmd == 0x66) break;
        if (cmd == 0xA0) {
            const uint8_t aa = b[p], dd = b[p + 1];
            p += 2;
            v.events.push_back({ now, (aa & 0x80) ? 1 : 0, aa & 0x7F, dd });
        } else if (cmd == 0x61) { now += static_cast<uint64_t>(b[p] | b[p + 1] << 8); p += 2; }
        else if (cmd == 0x62) now += 735;
        else if (cmd == 0x63) now += 882;
        else if (cmd >= 0x70 && cmd <= 0x7F) now += static_cast<uint64_t>(cmd & 0x0F) + 1;
        else { std::fprintf(stderr, "unexpected VGM command $%02X\n", cmd); assert(false); }
    }
    assert(p == b.size());
    v.endSample = now;
    return v;
}

// ── Driving the card as a music driver does ────────────────────────────────
// [B] Mockingboard: VIA port A = the AY data bus, port B bits 0-2 = BC1,
// BDIR, /RESET. $07 latch address, $06 write, $04 inactive, $00 reset.
void via(MockingboardCard& c, int chip, uint8_t reg, uint8_t v)
{
    c.slotRomWrite(static_cast<uint8_t>((chip ? 0x80 : 0x00) | reg), v);
}
void ayWrite(MockingboardCard& c, int chip, uint8_t reg, uint8_t val)
{
    via(c, chip, 0x01, reg);  via(c, chip, 0x00, 0x07);  via(c, chip, 0x00, 0x04);
    via(c, chip, 0x01, val);  via(c, chip, 0x00, 0x06);  via(c, chip, 0x00, 0x04);
}
void ayReset(MockingboardCard& c, int chip)
{
    via(c, chip, 0x00, 0x00);  via(c, chip, 0x00, 0x04);
}
void setupVias(MockingboardCard& c)
{
    for (int chip = 0; chip < 2; ++chip) { via(c, chip, 0x03, 0xFF); via(c, chip, 0x02, 0x07); via(c, chip, 0x00, 0x04); }
}

uint64_t expectSample(uint64_t cyclesFromStart, double hz)
{
    return static_cast<uint64_t>(std::floor(static_cast<double>(cyclesFromStart) * 44100.0 / hz));
}

void testVgmRoundTrip()
{
    MockingboardCard card(4);
    // A PAL machine, set as setVideoStandard sets every card: the CPU and
    // the bus at the standard's clock, so a CPU cycle is a bus tick.
    card.setCpuClock(1015625.0);
    card.setStandardClock(1015625.0);
    setupVias(card);
    ayWrite(card, 0, 7, 0x38);          // before the capture: its initial state
    ayWrite(card, 1, 8, 0x0F);
    card.startAyCapture(20280);

    std::vector<Event> want;
    // Initial state at sample 0: both chips, R0-R13.
    for (int chip = 0; chip < 2; ++chip)
        for (int r = 0; r < 14; ++r)
            want.push_back({ 0, chip, r, card.getAyRegister(chip, r) });
    assert(want[7].val == 0x38 && want[14 + 8].val == 0x0F);

    // A tune: a few writes at irregular gaps, a long silence that needs more
    // than one $61, a gap of exactly 735 and 882 samples' worth, a reset.
    struct Step { uint64_t gap; int chip; int reg; int val; bool reset; };
    const Step steps[] = {
        { 100, 0, 0, 0x5D, false }, { 3, 0, 1, 0x01, false }, { 20000, 1, 2, 0xEF, false },
        { 1, 1, 3, 0x00, false }, { 2000000, 0, 8, 0x0C, false }, { 16929, 0, 13, 0x0E, false },
        { 20313, 1, 13, 0x0E, false }, { 7, 0, 0, 0x00, true }, { 5, 1, 9, 0x0A, false },
    };
    uint64_t t = 0;
    for (const Step& s : steps) {
        card.advanceCycles(static_cast<int>(s.gap));
        t += s.gap;
        const uint64_t at = expectSample(t, 1015625.0);
        if (s.reset) {
            ayReset(card, s.chip);
            for (int r = 0; r < 14; ++r) want.push_back({ at, s.chip, r, 0 });
        } else {
            ayWrite(card, s.chip, static_cast<uint8_t>(s.reg), static_cast<uint8_t>(s.val));
            want.push_back({ at, s.chip, s.reg, s.val });
        }
    }
    card.advanceCycles(5000);
    t += 5000;
    const aym::Capture cap = card.stopAyCapture();
    assert(!card.ayCaptureActive());
    assert(cap.endCycle - cap.startCycle == t);

    const Vgm v = readVgm(aym::encodeVgm(cap));
    assert(v.ayClock == 1015625 && v.dual);
    assert(v.events.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!(v.events[i] == want[i]))
            std::fprintf(stderr, "event %zu: got (%llu,%d,%d,$%02X) want (%llu,%d,%d,$%02X)\n", i,
                         (unsigned long long)v.events[i].sample, v.events[i].chip, v.events[i].reg,
                         v.events[i].val, (unsigned long long)want[i].sample, want[i].chip,
                         want[i].reg, want[i].val);
        assert(v.events[i] == want[i]);
    }
    assert(v.endSample == expectSample(t, 1015625.0));
    assert(v.totalSamples == v.endSample);
    std::printf("  ok: VGM round trip, %zu writes at their 44.1 kHz samples (PAL bus clock)\n",
                want.size());

    // A card that is not recording hands back nothing.
    assert(card.stopAyCapture().writes.empty());
}

void testYm5()
{
    aym::Capture c;
    c.busHz = 1022727.0;
    c.cyclesPerFrame = 17030;
    c.startCycle = 1000;
    c.endCycle = 1000 + 3 * 17030;               // three frames
    c.initial[0][0] = 0x12;  c.initial[0][13] = 0x08;
    c.initial[0][1] = 0xF3;                       // junk in the bits R1 does not hold
    c.writes = { { 1000 + 17030 + 5, 0, 0, 0x34 },   // frame 1
                 { 1000 + 17030 + 9, 1, 0, 0x99 },   // the other chip: not in a YM of chip 0
                 { 1000 + 2 * 17030 + 1, 0, 13, 0x0A } };   // frame 2
    const std::vector<uint8_t> y = aym::encodeYm5(c, 0, "T");
    assert(std::string(y.begin(), y.begin() + 12) == "YM5!LeOnArD!");
    auto be32 = [&](std::size_t at) { return static_cast<uint32_t>(y[at]) << 24 | y[at + 1] << 16 | y[at + 2] << 8 | y[at + 3]; };
    assert(be32(12) == 3);                        // frames
    assert(be32(16) == 1);                        // interleaved
    assert(be32(22) == 1022727);                  // master clock
    assert((y[26] << 8 | y[27]) == 60);           // frame rate
    std::size_t p = 34;                           // after loop frame + extra size
    for (int s = 0; s < 3; ++s) { while (y[p]) ++p; ++p; }   // three strings
    const uint8_t* d = &y[p];
    assert(d[0 * 3 + 0] == 0x12 && d[0 * 3 + 1] == 0x34 && d[0 * 3 + 2] == 0x34);   // R0 by frame
    assert(d[1 * 3 + 0] == 0x03);                 // R1 masked to its 4 bits
    assert(d[13 * 3 + 0] == 0x08 && d[13 * 3 + 1] == 0xFF && d[13 * 3 + 2] == 0x0A);   // R13
    assert(std::string(y.end() - 4, y.end()) == "End!");
    assert(y.size() == p + 16 * 3 + 4);
    std::printf("  ok: YM5 frames, interleaved, R13 = $FF where unwritten, registers masked\n");
}

void testWav()
{
    const fs::path dir = fs::temp_directory_path() / "pom2_wav_test";
    fs::create_directories(dir);
    const fs::path path = dir / "take.wav";
    fs::remove(path);
    pom2::WavFileWriter w;
    std::string err;
    assert(w.open(path, 44100, 2, err));
    assert(!fs::exists(path) && "nothing under the real name until close()");
    const float frames[] = { 0.0f, 1.0f, -1.0f, 0.5f, 2.0f, -3.0f };
    w.write(frames, 3);
    assert(w.close(err));
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    assert(b.size() == 44 + 12);
    assert(std::string(b.begin(), b.begin() + 4) == "RIFF" && le32(b, 4) == 36 + 12);
    assert(std::string(b.begin() + 8, b.begin() + 16) == "WAVEfmt ");
    assert(le32(b, 24) == 44100 && le32(b, 40) == 12);
    auto s16 = [&](std::size_t i) { return static_cast<int16_t>(b[44 + 2 * i] | b[45 + 2 * i] << 8); };
    assert(s16(0) == 0 && s16(1) == 32767 && s16(2) == -32767 && s16(3) == 16384);
    assert(s16(4) == 32767 && s16(5) == -32767);   // clamped

    // A discarded take leaves nothing behind.
    const fs::path gone = dir / "gone.wav";
    fs::remove(gone);
    { pom2::WavFileWriter d; assert(d.open(gone, 44100, 2, err)); d.write(frames, 1); }
    assert(!fs::exists(gone));
    for (const auto& e : fs::directory_iterator(dir))
        assert(e.path().filename() == "take.wav" && "no temporary sibling left behind");
    fs::remove_all(dir);
    std::printf("  ok: WAV header, 16-bit samples clamped, atomic publish, discard\n");
}

void testRing()
{
    pom2::AudioCaptureRing ring(4);
    const float in[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    ring.push(in, 3);
    ring.push(in + 6, 3);              // room for one: two dropped
    assert(ring.droppedFrames() == 2);
    const fs::path path = fs::temp_directory_path() / "pom2_ring_test.wav";
    pom2::WavFileWriter w;
    std::string err;
    assert(w.open(path, 44100, 2, err));
    assert(ring.drainTo(w) == 4);
    ring.push(in, 3);                  // wraps
    assert(ring.drainTo(w) == 3);
    assert(w.framesWritten() == 7);
    w.discard();
    std::printf("  ok: capture ring drops when full, never blocks, wraps\n");
}

} // namespace

int main()
{
    std::printf("ay_music_export:\n");
    testVgmRoundTrip();
    testYm5();
    testWav();
    testRing();
    std::printf("ay_music_export: all passed\n");
    return 0;
}
