// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone, streaming MEA8000 prototype for the Ediciel Porte-Parole.
// Register/frame layout: Ediciel manual, annexes 2, 5 and 6.
// This is an independently written floating-point model, not a silicon-exact core.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

class Mea8000
{
public:
    static constexpr uint32_t Clock = 4000000;
    static constexpr uint32_t OutputRate = 48000;
    enum Mode { Stopped, WaitFirst, Playing, Slowing };

    Mea8000() { Reset(); }
    void Reset()
    {
        m_mode = Stopped; m_pos = m_length = m_count = m_nativePhase = m_audioPhase = m_busy = 0;
        m_cont = m_roe = m_noise = false;
        m_pitch = m_oldPitch = m_amp = m_oldAmp = m_phase = m_raw = 0;
        m_rng = 0x8000; m_buf.fill(0);
        m_fm.fill(0); m_oldFm.fill(0); m_bw.fill(0); m_oldBw.fill(0);
        m_y1.fill(0); m_y2.fill(0);
        m_z1.fill(0); m_z2.fill(0);
    }
    uint8_t ReadStatus() const
    {
        return !m_busy && (m_mode == Stopped || m_mode == WaitFirst ||
            (m_mode == Playing && m_count < 4)) ? 0x80 : 0;
    }
    bool Active() const { return m_mode == Playing || m_mode == Slowing; }
    Mode GetMode() const { return static_cast<Mode>(m_mode); }
    uint32_t BufferedBytes() const { return m_count; }
    uint32_t FrameSamples() const { return m_length; }
    void WriteCommand(uint8_t data)
    {
        if (data & 8) m_cont = (data & 4) != 0;
        if (data & 2) m_roe = (data & 1) != 0;
        // ROE is retained, but no IRQ wiring is assumed for the Porte-Parole.
        if (data & 0x10) { m_mode = Stopped; m_count = 0; m_raw = 0; }
    }
    bool WriteData(uint8_t data)
    {
        if (!ReadStatus()) return false;
        m_busy = 12; // Approximate <=3 us request turnaround, subject to hardware measurement.
        if (m_mode == Stopped)
        {
            m_pitch = m_oldPitch = 2.0 * data; m_count = 0; m_mode = WaitFirst;
        }
        else
        {
            m_buf[m_count++] = data;
            if (m_mode == WaitFirst && m_count == 4)
            {
                const double initialPitch = m_pitch;
                DecodeFrame();
                m_oldFm = m_fm; m_oldBw = m_bw; m_oldAmp = 0;
                m_oldPitch = initialPitch;
                m_mode = Playing; m_nativePhase = 0;
            }
        }
        return true;
    }
    // All timing is in quartz clocks. No wall clock, file IO or host audio dependency.
    // Output scheduling continues during silence; state survives arbitrary call boundaries.
    void AdvanceClocks(uint64_t clocks, std::vector<int16_t>* output = nullptr)
    {
        while (clocks)
        {
            uint32_t untilAudio = (Clock - m_audioPhase + OutputRate - 1) / OutputRate;
            uint32_t step = static_cast<uint32_t>((std::min)(clocks,
                static_cast<uint64_t>((std::min)(480 - m_nativePhase, untilAudio))));
            clocks -= step;
            m_busy = step >= m_busy ? 0 : m_busy - step;
            m_nativePhase += step; m_audioPhase += step * OutputRate;
            if (m_nativePhase == 480)
            {
                m_nativePhase = 0;
                NativeSample();
            }
            if (m_audioPhase >= Clock)
            {
                m_audioPhase -= Clock;
                double sample = OutputSample();
                if (output) output->push_back(static_cast<int16_t>(
                    (std::max)(-32767.0, (std::min)(32767.0, sample * 32767.0))));
            }
        }
    }
    std::vector<double> SaveState() const
    {
        Mea8000 copy = *this;
        std::vector<double> v;
        copy.Visit([&](auto& x) { v.push_back(static_cast<double>(x)); });
        return v;
    }
    void LoadState(const std::vector<double>& v)
    {
        if (v.size() != SaveState().size()) throw std::runtime_error("MEA8000 state length");
        for (double x : v) if (!std::isfinite(x)) throw std::runtime_error("MEA8000 non-finite state");
        Mea8000 next;
        size_t i = 0;
        next.Visit([&](auto& x) {
            using T = typename std::decay<decltype(x)>::type;
            if (!std::is_floating_point<T>::value)
            {
                double max = std::is_same<T, bool>::value ? 1.0 :
                    (std::is_same<T, uint8_t>::value ? 255.0 : 4294967295.0);
                if (v[i] < 0 || v[i] > max || v[i] != std::floor(v[i]))
                    throw std::runtime_error("MEA8000 integer state");
            }
            x = static_cast<T>(v[i++]);
        });
        if (next.m_mode > Slowing || next.m_count > 4 || next.m_nativePhase >= 480 ||
            next.m_audioPhase >= Clock || next.m_busy > 12 || next.m_length > 512 ||
            (next.Active() && (next.m_length < 64 || next.m_pos >= next.m_length)))
            throw std::runtime_error("MEA8000 invalid state");
        *this = next;
    }

private:
    uint32_t m_mode, m_pos, m_length, m_count, m_nativePhase, m_audioPhase, m_busy, m_rng;
    bool m_cont, m_roe, m_noise;
    std::array<uint8_t, 4> m_buf;
    double m_pitch, m_oldPitch, m_amp, m_oldAmp, m_phase, m_raw;
    std::array<double, 4> m_fm, m_oldFm, m_bw, m_oldBw, m_y1, m_y2;
    std::array<double, 3> m_z1, m_z2;

    template<class F> void Visit(F f)
    {
        f(m_mode); f(m_pos); f(m_length); f(m_count); f(m_nativePhase); f(m_audioPhase);
        f(m_busy); f(m_rng); f(m_cont); f(m_roe); f(m_noise);
        for (auto& x : m_buf) f(x);
        f(m_pitch); f(m_oldPitch); f(m_amp); f(m_oldAmp); f(m_phase); f(m_raw);
        for (auto* a : {&m_fm, &m_oldFm, &m_bw, &m_oldBw, &m_y1, &m_y2})
            for (auto& x : *a) f(x);
        for (auto* a : {&m_z1, &m_z2}) for (auto& x : *a) f(x);
    }
    void DecodeFrame()
    {
        static constexpr int fm1[] = {150,162,174,188,202,217,233,250,267,286,305,325,346,368,391,415,
            440,466,494,523,554,587,622,659,698,740,784,830,880,932,988,1047};
        // Annex 6 gives 1110 at index 16; MAME gives 1100. Hardware arbitration pending.
        static constexpr int fm2[] = {440,466,494,523,554,587,622,659,698,740,784,830,880,932,988,1047,
            1110,1179,1254,1337,1428,1528,1639,1761,1897,2047,2214,2400,2609,2842,3105,3400};
        static constexpr int fm3[] = {1179,1337,1528,1761,2047,2400,2842,3400};
        static constexpr int bw[] = {726,309,125,50};
        static constexpr double amp[] = {0,.008,.011,.016,.022,.031,.044,.062,.088,.125,.177,.250,.354,.500,.707,1};
        m_fm = {double(fm1[m_buf[2] >> 3]), double(fm2[m_buf[1] & 31]), double(fm3[m_buf[1] >> 5]), 3500};
        for (int i = 0; i < 4; ++i) m_bw[i] = bw[(m_buf[0] >> (6 - 2*i)) & 3];
        uint32_t fd = (m_buf[3] >> 5) & 3;
        int pi = m_buf[3] & 31;
        m_noise = pi == 16;
        if (!m_noise)
        {
            if (pi > 16) pi -= 32;
            m_pitch = (std::max)(0.0, (std::min)(510.0, m_oldPitch + pi * (1 << fd)));
        }
        m_amp = amp[((m_buf[2] & 7) << 1) | (m_buf[3] >> 7)];
        m_length = 64U << fd; m_pos = 0; m_count = 0;
    }
    void ShiftFrame()
    {
        m_oldFm = m_fm; m_oldBw = m_bw; m_oldAmp = m_amp; m_oldPitch = m_pitch;
    }
    void NativeSample()
    {
        if (!Active())
        {
            // STOP gates the DAC, not time. Previously the digital resonators were
            // frozen throughout silence, then their old energy reappeared under
            // different coefficients at the next message (even a silent frame).
            // Let the stable zero-input system settle while the DAC is gated.
            bool charged = false;
            for (int i = 0; i < 4; ++i)
                charged = charged || std::abs(m_y1[i]) > 1e-12 || std::abs(m_y2[i]) > 1e-12;
            if (charged)
            {
                constexpr double pi = 3.14159265358979323846;
                double x = 0;
                for (int i = 0; i < 4; ++i)
                {
                    double r = std::exp(-pi*m_bw[i]/8000.0);
                    double b = 2*r*std::cos(2*pi*m_fm[i]/8000.0);
                    double y = x + b*m_y1[i] - r*r*m_y2[i];
                    m_y2[i] = m_y1[i]; m_y1[i] = y; x = y;
                }
            }
            else { m_y1.fill(0); m_y2.fill(0); }
            m_raw = 0;
            return;
        }
        constexpr double pi = 3.14159265358979323846;
        double t = static_cast<double>(m_pos) / m_length;
        auto lerp = [t](double a, double b) { return a + (b-a)*t; };
        double x;
        if (m_noise)
        {
            // Deterministic xorshift excitation; actual silicon noise sequence is unknown.
            m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
            x = double(m_rng & 65535) / 32768.0 - 1;
        }
        else
        {
            m_phase += lerp(m_oldPitch, m_pitch) / 8000.0;
            m_phase -= std::floor(m_phase); x = 2*m_phase - 1;
        }
        x *= lerp(m_oldAmp, m_amp);
        for (int i = 0; i < 4; ++i)
        {
            double r = std::exp(-pi * lerp(m_oldBw[i], m_bw[i]) / 8000.0);
            double b = 2*r*std::cos(2*pi*lerp(m_oldFm[i], m_fm[i])/8000.0);
            double y = x + b*m_y1[i] - r*r*m_y2[i];
            m_y2[i] = m_y1[i]; m_y1[i] = y; x = y;
        }
        // Fixed output gain; never normalize per utterance or per host buffer.
        m_raw = x * 0.2;
        if (++m_pos == m_length)
        {
            ShiftFrame();
            if (m_count == 4 && m_mode == Playing) DecodeFrame();
            else if (m_cont && m_mode == Playing) m_pos = 0;
            else if (m_mode == Playing) { m_amp = 0; m_pos = 0; m_mode = Slowing; }
            else { m_mode = Stopped; m_count = 0; m_raw = 0; }
        }
    }
    double OutputSample()
    {
        // Provisional analogue profile used for calibration: LP4 3kHz, HP2 120Hz.
        // Butterworth biquads, maintaining filter history across all IO calls and frames.
        struct Biquad { double b0,b1,b2,a1,a2; };
        static const std::array<Biquad,3> coeff = [] {
            std::array<Biquad,3> c{};
            constexpr double pi = 3.14159265358979323846;
            for (int i = 0; i < 3; ++i)
            {
                double w = 2*pi*(i == 2 ? 120 : 3000)/OutputRate;
                double q = i == 0 ? 0.541196100146197 : (i == 1 ? 1.306562964876377 : 0.707106781186548);
                double co = std::cos(w), alpha = std::sin(w)/(2*q), a0 = 1+alpha;
                double b0 = i == 2 ? (1+co)/2 : (1-co)/2;
                c[i] = {b0/a0, (i == 2 ? -2 : 2)*b0/a0, b0/a0, -2*co/a0, (1-alpha)/a0};
            }
            return c;
        }();
        double x = m_raw;
        for (int i = 0; i < 3; ++i)
        {
            const auto& c = coeff[i]; double y = c.b0*x + m_z1[i];
            m_z1[i] = c.b1*x - c.a1*y + m_z2[i]; m_z2[i] = c.b2*x - c.a2*y; x = y;
        }
        return x;
    }
};
