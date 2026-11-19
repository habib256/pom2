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

// AyMusicExport — see the header.

#include "AyMusicExport.h"

#include <algorithm>
#include <cmath>

namespace pom2::aymusic {

namespace {

void putLe32(std::vector<uint8_t>& o, std::size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i) o[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
}

void appendBe32(std::vector<uint8_t>& o, uint32_t v)
{
    for (int i = 3; i >= 0; --i) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void appendBe16(std::vector<uint8_t>& o, uint16_t v)
{
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

// [B] VGM 1.71: $61 nnnn waits up to 65535 samples, $62 = 735 (1/60 s),
// $63 = 882 (1/50 s), $7n = n+1 samples.
void appendWait(std::vector<uint8_t>& o, uint64_t samples)
{
    while (samples > 0) {
        if (samples == 735) { o.push_back(0x62); return; }
        if (samples == 882) { o.push_back(0x63); return; }
        if (samples <= 16) { o.push_back(static_cast<uint8_t>(0x70 + samples - 1)); return; }
        const uint64_t n = std::min<uint64_t>(samples, 0xFFFF);
        o.push_back(0x61);
        o.push_back(static_cast<uint8_t>(n & 0xFF));
        o.push_back(static_cast<uint8_t>(n >> 8));
        samples -= n;
    }
}

void appendAyWrite(std::vector<uint8_t>& o, int chip, uint8_t reg, uint8_t val)
{
    o.push_back(0xA0);
    o.push_back(static_cast<uint8_t>((reg & 0x0F) | (chip ? 0x80 : 0x00)));
    o.push_back(val);
}

// The bits each AY register holds ([B] GI AY-3-8910 data sheet, register
// array table). YM5 keeps its effect flags in the bits the chip ignores, so
// an unmasked store would switch on a phantom SID or DigiDrum effect there.
constexpr uint8_t kRegMask[16] = { 0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                                   0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0x00, 0x00 };

} // namespace

uint64_t sampleAt(const Capture& c, uint64_t cycle)
{
    if (cycle <= c.startCycle || c.busHz <= 0.0) return 0;
    const double s = static_cast<double>(cycle - c.startCycle) * kVgmRate / c.busHz;
    return static_cast<uint64_t>(std::floor(s));
}

std::vector<uint8_t> encodeVgm(const Capture& c)
{
    constexpr std::size_t kHeader = 0x100;
    std::vector<uint8_t> o(kHeader, 0);
    o[0] = 'V'; o[1] = 'g'; o[2] = 'm'; o[3] = ' ';
    putLe32(o, 0x08, 0x00000171);                       // version 1.71
    putLe32(o, 0x34, static_cast<uint32_t>(kHeader - 0x34));   // data offset, relative
    const int chips = std::clamp(c.chips, 1, 2);
    uint32_t clock = static_cast<uint32_t>(std::lround(c.busHz));
    if (chips == 2) clock |= 0x40000000u;               // bit 30: dual chip
    putLe32(o, 0x74, clock);
    o[0x78] = 0x00;                                     // AY8910
    o[0x79] = 0x01;                                     // legacy output (the default)

    // The chips as the capture found them, at sample 0.
    for (int chip = 0; chip < chips; ++chip)
        for (uint8_t r = 0; r < 14; ++r) appendAyWrite(o, chip, r, c.initial[chip][r]);

    uint64_t at = 0;
    for (const Write& w : c.writes) {
        if (w.chip >= chips) continue;
        if (w.reg != kResetReg && w.reg > 13) continue;   // the I/O ports make no sound
        const uint64_t s = sampleAt(c, w.cycle);
        if (s > at) { appendWait(o, s - at); at = s; }
        if (w.reg == kResetReg)
            for (uint8_t r = 0; r < 14; ++r) appendAyWrite(o, w.chip, r, 0);
        else
            appendAyWrite(o, w.chip, w.reg, w.val);
    }
    const uint64_t total = std::max(at, sampleAt(c, c.endCycle));
    if (total > at) appendWait(o, total - at);
    o.push_back(0x66);                                  // end of sound data

    putLe32(o, 0x04, static_cast<uint32_t>(o.size() - 4));   // EOF offset
    putLe32(o, 0x18, static_cast<uint32_t>(std::min<uint64_t>(total, 0xFFFFFFFFu)));
    return o;
}

std::vector<uint8_t> encodeYm5(const Capture& c, int chip, const std::string& title)
{
    chip = std::clamp(chip, 0, 1);
    const uint64_t cpf = static_cast<uint64_t>(std::max(1, c.cyclesPerFrame));
    const uint64_t span = c.endCycle > c.startCycle ? c.endCycle - c.startCycle : 0;
    const uint64_t frames = std::max<uint64_t>(1, (span + cpf - 1) / cpf);

    // Replay the writes frame by frame: the registers at each frame's end.
    std::vector<uint8_t> regs(static_cast<std::size_t>(frames) * 16, 0);
    uint8_t live[16];
    for (int r = 0; r < 16; ++r) live[r] = c.initial[chip][r];
    std::size_t next = 0;
    for (uint64_t f = 0; f < frames; ++f) {
        const uint64_t frameEnd = c.startCycle + (f + 1) * cpf;
        bool envWritten = (f == 0);   // the capture's first frame sets the envelope
        while (next < c.writes.size() && c.writes[next].cycle < frameEnd) {
            const Write& w = c.writes[next++];
            if (w.chip != chip) continue;
            if (w.reg == kResetReg) {
                for (int r = 0; r < 14; ++r) live[r] = 0;
                envWritten = true;
            } else if (w.reg < 16) {
                live[w.reg] = w.val;
                if (w.reg == 13) envWritten = true;
            }
        }
        for (int r = 0; r < 16; ++r)
            regs[static_cast<std::size_t>(f) * 16 + static_cast<std::size_t>(r)] =
                static_cast<uint8_t>(live[r] & kRegMask[r]);
        regs[static_cast<std::size_t>(f) * 16 + 13] =
            envWritten ? static_cast<uint8_t>(live[13] & 0x0F) : 0xFF;
    }

    std::vector<uint8_t> o;
    for (char ch : std::string("YM5!LeOnArD!")) o.push_back(static_cast<uint8_t>(ch));
    appendBe32(o, static_cast<uint32_t>(frames));
    appendBe32(o, 1);                                   // attributes: interleaved
    appendBe16(o, 0);                                   // no digidrums
    appendBe32(o, static_cast<uint32_t>(std::lround(c.busHz)));   // AY master clock
    appendBe16(o, static_cast<uint16_t>(std::lround(c.busHz / static_cast<double>(cpf))));
    appendBe32(o, 0);                                   // loop frame
    appendBe16(o, 0);                                   // no extra data
    for (const std::string& s : { title, std::string("POM2"),
                                  std::string("Mockingboard AY ") + (chip ? "2" : "1") }) {
        for (char ch : s) o.push_back(static_cast<uint8_t>(ch));
        o.push_back(0);
    }
    // Interleaved: every frame's R0, then every frame's R1, …
    for (int r = 0; r < 16; ++r)
        for (uint64_t f = 0; f < frames; ++f)
            o.push_back(regs[static_cast<std::size_t>(f) * 16 + static_cast<std::size_t>(r)]);
    for (char ch : std::string("End!")) o.push_back(static_cast<uint8_t>(ch));
    return o;
}

} // namespace pom2::aymusic
