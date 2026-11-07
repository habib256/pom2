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

// Mea8000 — see the header for the sources and the departures from MAME.
// Function-level comments name the MAME function each one ports
// ([D] mea8000.cpp @ 5346a8ce).

#include "Mea8000.h"

#include "ByteIO.h"

#include <algorithm>
#include <cmath>

namespace pom2 {

namespace {

// [B] Porte Parole manual Annexe 6 = [D] MAME fm1_table..bw_table; the data
// sheet fixes FM4 at 3500 Hz (Fig. 2 table). Frequencies in Hz AT THE
// NOMINAL 3.84 MHz CLOCK (header, departure 1).
constexpr int kFm1[32] = {
    150,  162,  174,  188,  202,  217,  233,  250,
    267,  286,  305,  325,  346,  368,  391,  415,
    440,  466,  494,  523,  554,  587,  622,  659,
    698,  740,  784,  830,  880,  932,  988, 1047 };
constexpr int kFm2[32] = {
    440,  466,  494,  523,  554,  587,  622,  659,
    698,  740,  784,  830,  880,  932,  988, 1047,
    1100, 1179, 1254, 1337, 1428, 1528, 1639, 1761,
    1897, 2047, 2214, 2400, 2609, 2842, 3105, 3400 };
constexpr int kFm3[8] = { 1179, 1337, 1528, 1761, 2047, 2400, 2842, 3400 };
constexpr int kFm4 = 3500;
constexpr int kBw[4] = { 726, 309, 125, 50 };
// Amplitude x 1000.
constexpr int kAmpl[16] = {
    0,   8,  11,  16,  22,  31,  44,   62,
    88, 125, 177, 250, 354, 500, 707, 1000 };
// Pitch increment, Hz per 8 ms; code 16 selects noise.
constexpr int kPi[32] = {
    0, 1,  2,  3,  4,  5,  6,  7,
    8, 9, 10, 11, 12, 13, 14, 15,
    0, -15, -14, -13, -12, -11, -10, -9,
    -8, -7, -6, -5, -4, -3, -2, -1 };
constexpr int kMaxPitchHz = 510;   // header, departure 4

constexpr int kQuant = 512;
constexpr unsigned kTableLen = 3600;
constexpr unsigned kNoiseLen = 8192;

// MAME init_tables, with f = i / 8000 (the design rate, departure 1) instead
// of i / (clock / 480). Shared by every instance: pure functions of i.
struct Tables {
    int cosT[kTableLen], expT[kTableLen], exp2T[kTableLen];
    int noise[kNoiseLen];
    Tables() {
        constexpr double pi = 3.14159265358979323846;
        for (unsigned i = 0; i < kTableLen; ++i) {
            const double f = static_cast<double>(i) / Mea8000::kDesignRate;
            cosT[i]  = static_cast<int>(2.0 * std::cos(2.0 * pi * f) * kQuant);
            expT[i]  = static_cast<int>(std::exp(-pi * f) * kQuant);
            exp2T[i] = static_cast<int>(std::exp(-2.0 * pi * f) * kQuant);
        }
        // [F] departure 5: a fixed xorshift32 instead of machine().rand(),
        // same range as MAME's `rand() % (2 * QUANT) - QUANT`.
        uint32_t x = 0x4D454138u;   // "MEA8"
        for (auto& n : noise) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            n = static_cast<int>(x % (2 * kQuant)) - kQuant;
        }
    }
};
const Tables& tables() { static const Tables t; return t; }

// Floor division by a power of two, for a signed numerator: the meaning of
// MAME's `>>` on a negative `int` on every compiler it targets, spelled out
// because C++17 leaves a negative right shift implementation-defined.
int shiftFloor(int64_t v, unsigned bits)
{
    const int64_t d = int64_t(1) << bits;
    return static_cast<int>(v >= 0 ? v / d : -((-v + d - 1) / d));
}

} // namespace

Mea8000::Mea8000() { (void)tables(); reset(); }

void Mea8000::reset()
{
    gridPos_ = 0;
    pitchWait_ = 0;
    state_ = State::Stopped;
    buf_ = {};
    bufPos_ = 0;
    cont_ = roe_ = 0;
    frameLength_ = framePos_ = frameLog_ = 0;
    lastSample_ = sample_ = 0;
    phi_ = 0;
    f_ = {};
    lastAmpl_ = ampl_ = 0;
    lastPitch_ = pitch_ = 0;
    noise_ = 0;
    running_ = false;
    output_ = 0;
}

// MAME accept_byte. [B] p. 5-274 / 5-275: REQ is 1 in STOP mode and while a
// frame's four bytes are being collected; after the fourth it stays 0 until
// the running frame ends. The fade-out frame accepts nothing.
bool Mea8000::acceptsByte() const
{
    if (pitchWait_) return false;   // departure 7
    return state_ == State::Stopped || state_ == State::WaitFirst ||
           (state_ == State::Started && bufPos_ < 4);
}

// MAME interp: org + ((dst - org) * framepos) >> framelog.
int Mea8000::interp(int from, int to) const
{
    return from + shiftFloor(int64_t(to - from) * framePos_, frameLog_);
}

// MAME filter_step: a second-order resonator, y[n] = x + b*y[n-1] - c*y[n-2]
// with b = 2 cos(2 pi fm/F0) e^(-pi bw/F0), c = e^(-2 pi bw/F0). The products
// are taken in 64 bits: a resonant filter's state is only clamped at the
// end of the chain, and MAME's int products can overflow there.
int Mea8000::filterStep(int i, int input)
{
    const Tables& t = tables();
    const int fm = std::clamp(interp(f_[i].lastFm, f_[i].fm), 0, int(kTableLen) - 1);
    const int bw = std::clamp(interp(f_[i].lastBw, f_[i].bw), 0, int(kTableLen) - 1);
    const int64_t b = (int64_t(t.cosT[fm]) * t.expT[bw]) / kQuant;
    const int64_t c = t.exp2T[bw];
    const int64_t next = input + (b * f_[i].output - c * f_[i].lastOutput) / kQuant;
    f_[i].lastOutput = f_[i].output;
    f_[i].output = static_cast<int32_t>(std::clamp<int64_t>(next, INT32_MIN / 2, INT32_MAX / 2));
    return f_[i].output;
}

// MAME noise_gen: the next entry of the noise table, in [-QUANT, QUANT).
int Mea8000::noiseGen()
{
    phi_ = (phi_ + 1) % kNoiseLen;
    return tables().noise[phi_];
}

// MAME freq_gen: a sawtooth whose phase advances `pitch` per sample modulo
// the design rate — `pitch` Hz at 8 kHz, scaled with the clock — times the
// TP101 factor 1.024 = 128/125 (departure 4): the phase runs modulo
// 8000 x 125 and steps by pitch x 128.
int Mea8000::freqGen()
{
    constexpr uint32_t kPhaseMod = uint32_t(kDesignRate) * 125u;
    const int pitch = std::clamp(interp(lastPitch_, pitch_), 0, kMaxPitchHz);
    phi_ = (phi_ + static_cast<uint32_t>(pitch) * 128u) % kPhaseMod;
    return static_cast<int>((uint64_t(phi_) * kQuant * 2) / kPhaseMod) - kQuant;
}

// MAME compute_sample: source x amplitude, then the four resonators in
// cascade, clipped to 16 bits.
int Mea8000::computeSample()
{
    const int ampl = interp(lastAmpl_, ampl_);
    int out = noise_ ? noiseGen() : freqGen();
    out *= ampl / 32;
    for (int i = 0; i < 4; ++i) out = filterStep(i, out);
    return std::clamp(out, -32767, 32767);
}

// MAME shift_frame: the frame just played becomes the interpolation origin.
void Mea8000::shiftFrame()
{
    lastPitch_ = pitch_;
    for (auto& f : f_) { f.lastBw = f.bw; f.lastFm = f.fm; }
    lastAmpl_ = ampl_;
}

// MAME decode_frame, [B] data sheet Fig. 2: byte 1 = BW1 BW2 BW3 BW4 (2 bits
// each), byte 2 = FM3 (3) FM2 (5), byte 3 = FM1 (5) AMPL high 3, byte 4 =
// AMPL low 1, FD (2), PI (5). The PI increment is per 8 ms, so a longer
// frame multiplies it (MAME `<< fd`).
void Mea8000::decodeFrame()
{
    const int fd = (buf_[3] >> 5) & 3;
    const int pi = kPi[buf_[3] & 0x1F] * (1 << fd);
    noise_ = (buf_[3] & 0x1F) == 16;
    pitch_ = static_cast<uint16_t>(std::clamp(int(lastPitch_) + pi, 0, kMaxPitchHz));
    f_[0].bw = static_cast<uint16_t>(kBw[buf_[0] >> 6]);
    f_[1].bw = static_cast<uint16_t>(kBw[(buf_[0] >> 4) & 3]);
    f_[2].bw = static_cast<uint16_t>(kBw[(buf_[0] >> 2) & 3]);
    f_[3].bw = static_cast<uint16_t>(kBw[buf_[0] & 3]);
    f_[3].fm = kFm4;
    f_[2].fm = static_cast<uint16_t>(kFm3[buf_[1] >> 5]);
    f_[1].fm = static_cast<uint16_t>(kFm2[buf_[1] & 0x1F]);
    f_[0].fm = static_cast<uint16_t>(kFm1[buf_[2] >> 3]);
    ampl_ = static_cast<uint16_t>(kAmpl[((buf_[2] & 7) << 1) | (buf_[3] >> 7)]);
    // 8 ms at the design rate = 64 samples = 512 output ticks; x2 per FD step.
    frameLog_ = static_cast<uint16_t>(fd + 6 + 3);
    frameLength_ = static_cast<uint16_t>(1u << frameLog_);
    bufPos_ = 0;
}

void Mea8000::startFrame()
{
    running_ = true;
    framePos_ = 0;
}

// MAME stop_frame. [B] p. 5-276: STOP "results in an immediate reset of the
// synthesizer to the STOP mode", silent output.
void Mea8000::stopFrame()
{
    running_ = false;
    state_ = State::Stopped;
    output_ = 0;
}

// MAME timer_expire: one output tick. A real sample is computed every 8th
// tick and the ticks between are linearly interpolated ([B] the "8 kHz"
// sample interpolator feeding the "64 kHz" DAC). At the end of a frame: the
// next frame if its four bytes are in, else the repeat (CONT = 1), else one
// fade-out frame "with decaying amplitude and the same pitch" (p. 5-274),
// else STOP.
void Mea8000::idle(uint64_t ticks)
{
    if (running_ || ticks == 0) return;
    const uint64_t pos = gridPos_ + ticks;
    if (pitchWait_ && pos >= uint64_t(kGridTicks)) pitchWait_ = 0;
    gridPos_ = static_cast<uint16_t>(pos % kGridTicks);
}

int16_t Mea8000::tick()
{
    if (++gridPos_ >= kGridTicks) { gridPos_ = 0; pitchWait_ = 0; }
    if (!running_) return 0;
    const int pos = framePos_ % kSuperSampling;
    if (pos == 0) {
        lastSample_ = sample_;
        // Departure 8: the filter bank's 16 bits truncated to 11.
        sample_ = static_cast<int16_t>(computeSample() & ~0x1F);
    }
    // The interpolator's seven in-between samples, then the 8-bit DAC.
    const int level = lastSample_ + (pos * (sample_ - lastSample_)) / kSuperSampling;
    output_ = shiftFloor(level, 8) * 256;
    const int16_t result = static_cast<int16_t>(output_);

    if (++framePos_ >= frameLength_) {
        shiftFrame();
        if (bufPos_ == 4) {
            decodeFrame();
            startFrame();
        } else if (cont_) {
            startFrame();
        } else if (state_ == State::Started) {
            ampl_ = 0;
            startFrame();
            state_ = State::Slowing;
        } else if (state_ == State::Slowing) {
            stopFrame();
        }
    }
    return result;
}

// MAME write offset 0. In STOP the byte is the starting pitch (2 Hz units,
// [B] manual Annexe 6); then bytes collect into the 4-byte buffer, and the
// first full frame starts by fading in from silence at that pitch.
void Mea8000::writeData(uint8_t value)
{
    if (state_ == State::Stopped) {
        pitch_ = static_cast<uint16_t>(2 * value);
        state_ = State::WaitFirst;
        bufPos_ = 0;
        pitchWait_ = 1;   // departure 7: REQ at the next 8 ms grid point
        return;
    }
    if (bufPos_ == 4) return;   // overflow: MAME drops it, as does the buffer
    buf_[bufPos_++] = value;
    if (bufPos_ == 4 && state_ == State::WaitFirst) {
        // Departure 6: every parameter already at the first frame's value,
        // the amplitude rising from 0 to it. MAME zeroes the TARGET instead.
        const uint16_t oldPitch = pitch_;
        lastPitch_ = oldPitch;
        decodeFrame();
        shiftFrame();
        lastPitch_ = oldPitch;
        lastAmpl_ = 0;
        startFrame();
        state_ = State::Started;
    }
}

// MAME write offset 1, [B] p. 5-276: D4 STOP; D3 enables a write of CONT
// (D2); D1 enables a write of ROE (D0). STOP leaves CONT and ROE alone.
void Mea8000::writeCommand(uint8_t value)
{
    if (value & 0x08) cont_ = (value >> 2) & 1;
    if (value & 0x02) roe_ = value & 1;
    if (value & 0x10) stopFrame();
}

// ─── Snapshot ────────────────────────────────────────────────────────────

void Mea8000::appendSnapshot(std::vector<uint8_t>& out) const
{
    using namespace byteio;
    out.insert(out.end(), {'M', 'E', 'A', 1});
    putU8(out, static_cast<uint8_t>(state_));
    for (uint8_t b : buf_) putU8(out, b);
    putU8(out, bufPos_); putU8(out, cont_); putU8(out, roe_);
    putU16(out, frameLength_); putU16(out, framePos_); putU16(out, frameLog_);
    putU16(out, static_cast<uint16_t>(lastSample_));
    putU16(out, static_cast<uint16_t>(sample_));
    putU32(out, phi_);
    for (const auto& f : f_) {
        putU16(out, f.fm); putU16(out, f.lastFm);
        putU16(out, f.bw); putU16(out, f.lastBw);
        putU32(out, static_cast<uint32_t>(f.output));
        putU32(out, static_cast<uint32_t>(f.lastOutput));
    }
    putU16(out, lastAmpl_); putU16(out, ampl_);
    putU16(out, lastPitch_); putU16(out, pitch_);
    putU8(out, noise_); putU8(out, running_ ? 1 : 0);
    putU32(out, static_cast<uint32_t>(output_));
    putU16(out, gridPos_); putU8(out, pitchWait_);
}

bool Mea8000::loadSnapshot(const uint8_t* data, std::size_t size)
{
    if (!data || size != kSnapshotBytes || data[0] != 'M' || data[1] != 'E' ||
        data[2] != 'A' || data[3] != 1)
        return false;
    byteio::Reader r(data, size);
    r.pos = 4;
    Mea8000 c;
    const uint8_t st = r.u8();
    if (st > static_cast<uint8_t>(State::Slowing)) return false;
    c.state_ = static_cast<State>(st);
    for (auto& b : c.buf_) b = r.u8();
    c.bufPos_ = r.u8(); c.cont_ = r.u8(); c.roe_ = r.u8();
    c.frameLength_ = r.u16(); c.framePos_ = r.u16(); c.frameLog_ = r.u16();
    c.lastSample_ = static_cast<int16_t>(r.u16());
    c.sample_ = static_cast<int16_t>(r.u16());
    c.phi_ = r.u32();
    for (auto& f : c.f_) {
        f.fm = r.u16(); f.lastFm = r.u16();
        f.bw = r.u16(); f.lastBw = r.u16();
        f.output = static_cast<int32_t>(r.u32());
        f.lastOutput = static_cast<int32_t>(r.u32());
        if (f.fm >= kTableLen || f.lastFm >= kTableLen ||
            f.bw >= kTableLen || f.lastBw >= kTableLen) return false;
    }
    c.lastAmpl_ = r.u16(); c.ampl_ = r.u16();
    c.lastPitch_ = r.u16(); c.pitch_ = r.u16();
    c.noise_ = r.u8();
    const uint8_t run = r.u8();
    c.output_ = static_cast<int32_t>(r.u32());
    c.gridPos_ = r.u16(); c.pitchWait_ = r.u8();
    if (c.gridPos_ >= kGridTicks || c.pitchWait_ > 1) return false;
    // Every field inside the range the chip itself can reach.
    if (c.bufPos_ > 4 || c.cont_ > 1 || c.roe_ > 1 || c.noise_ > 1 || run > 1 ||
        c.frameLog_ > 12 || (run && c.frameLog_ < 9) ||
        c.frameLength_ != (c.frameLog_ ? (1u << c.frameLog_) : 0u) ||
        (run && c.framePos_ >= c.frameLength_) ||
        c.phi_ >= uint32_t(kDesignRate) * 125u ||
        c.lastAmpl_ > 1000 || c.ampl_ > 1000 ||
        c.lastPitch_ > kMaxPitchHz || c.pitch_ > kMaxPitchHz ||
        c.output_ < -32768 || c.output_ > 32767 || (c.output_ & 0xFF))
        return false;
    c.running_ = run != 0;
    *this = c;
    return true;
}

} // namespace pom2
