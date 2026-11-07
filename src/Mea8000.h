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

// Mea8000 — the Philips / Signetics (RTC in France) MEA8000 formant speech
// synthesiser: a periodic or noise source, an amplitude stage and four
// second-order resonators, every parameter interpolated across a frame of
// 8, 16, 32 or 64 ms.
//
// ─── Sources, graded ────────────────────────────────────────────────────
//
// [B] Signetics "MEA8000 Speech Synthesizer" data sheet, Linear Products,
//     September 1985, pp. 5-270..5-282 (CPCWiki mirror, MEA8000_Signetics.pdf):
//     pinout, the 4-byte frame and its field widths (Fig. 2), the command
//     register (STOP D4, CONT enable/CONT D3/D2, ROE enable/ROE D1/D0), the
//     three modes, the status register (REQ on D7 only), the timing.
// [B] Ediciel "Porte Parole" manual, Annexe 6 (1983): the parameter coding
//     tables (amplitude, frame duration, pitch, PI, FM1-FM3, bandwidth) —
//     identical to the tables below.
// [B] Philips Technical Publication 101, "MEA8000 voice synthesizer:
//     principles and interfacing" (1983; zx81.ordi5.free.fr mirror,
//     MEA8000-2.pdf): Table 2 "Speech code parameters when using a clock
//     frequency of 3,84 MHz" and its pitch note, the first-frame rule
//     (p. 10), REQ timing (pp. 10-11, Fig. 20), the 11-bit / 8-bit output
//     path (p. 5, p. 12).
// [D] MAME src/devices/sound/mea8000.cpp @ 5346a8ce (last change to the file
//     bdf56c6ae8, 2026-05-29), Antoine Miné, BSD-3-Clause: the synthesis
//     itself (sawtooth, noise table, resonator recurrence, linear parameter
//     interpolation, the x8 output interpolation, the fade-out frame). The
//     integer path is ported; table and function names follow it.
//
// ─── Where POM2 departs from MAME, and why ──────────────────────────────
//
// 1. The clock scales EVERY frequency, not just the frame length. [B] The
//    data sheet's block diagram feeds a code-to-parameter ROM into digital
//    resonators clocked from CLK, with "8 kHz" at the sample interpolator and
//    "64 kHz" at the DAC for the nominal 3.84 MHz crystal (pin 16/17 text,
//    p. 5-271) — so a coded frequency is a fixed fraction of the sample rate.
//    MAME builds its coefficient tables from clock()/480 in Hz, which keeps
//    pitch and formants in Hz whatever the crystal while the frames shorten.
//    POM2 builds them for the nominal 8 kHz design rate and runs that design
//    at clock/480: on the Porte Parole's 4.000 MHz crystal (data sheet max)
//    everything is 4/3.84 = +4.2 % higher and every frame 4 % shorter, as
//    the silicon would be. [F] The chip's internal coefficient quantisation
//    is unknown; the tables are MAME's QUANT = 512 integer rounding.
// 2. A status read drives D7 only. [B] Fig. 5 note: "Address input A0 is a
//    don't care. Data bits D0 to D6 remain floating." MAME returns 0 in
//    D0-D6; `status()` returns only bit 7 and the CARD fills the rest from
//    the floating bus.
// 3. REQ drops after each data write. [B] data sheet Fig. 7 "3 µs max";
//    Philips TP101 p. 10: "REQ remains high [inactive] for about 2 µs
//    between the first and second, second and third, and third and fourth
//    bytes". MAME's REQ never drops between bytes. The CARD holds the status
//    bit at 0 for 2 µs of CPU cycles after a data write (PorteParoleCard).
// 4. Pitch is the table value x 1.024. [B] TP101 Table 2 note: "For exact
//    values of pitch and pitch increment, multiply the values given above by
//    1,024." MAME uses the table value. Code 50 = 102.4 Hz at 3.84 MHz.
//    Pitch also stays in table range 0..510. [B] the Porte Parole table codes the
//    starting pitch as 2 Hz x 0..255; [F] what the chip does when a run of
//    PI increments leaves that range is unknown, so POM2 clamps where MAME's
//    uint16 wraps to ~65 kHz.
// 5. The noise table is seeded, not machine().rand(). [F] The chip's noise
//    source is undocumented ("random noise", p. 5-274). A fixed seed makes
//    the output reproducible, which a snapshot and a test both need.
// 6. The first frame fades IN. [B] TP101 p. 10: "except for the amplitude
//    which starts from zero and reaches its correct value at the end of the
//    frame, the value of each parameter is set before the first frame is
//    spoken." MAME's code (its comment says "fade-in") interpolates from the
//    frame's amplitude DOWN to 0, so an utterance's first frame decays and a
//    one-frame CONT loop is silent.
// 7. After the starting pitch, REQ waits for the 8 ms grid. [B] TP101 p. 10:
//    "After the starting pitch byte has been written, REQ remains high for
//    up to 8 ms, then goes low"; Fig. 20: its trailing edges occur "at
//    multiples of 8 ms". POM2 keeps a free-running 8 ms grid (512 output
//    ticks) and holds the status bit at 0 from the pitch byte to the next
//    grid point. [F] The grid's phase at power-up is unknown (0 here), and
//    the first frame still starts at its fourth byte as in MAME — a few
//    microseconds after the grid point for a polling host.
// 8. The output is 11 bits into the interpolator and 8 bits out of the DAC.
//    [B] TP101 p. 5: "The 16-bit samples from the formant filter bank are
//    truncated to 11-bits before entering the interpolation and
//    digital-to-analogue circuitry", an "8-bit DAC and linear interpolator".
//    MAME interpolates and outputs the full 16 bits. [F] that MAME's
//    16-bit scale is the chip's: the truncation keeps the top bits of it.
//
// Not modelled: the REQ pin itself and ROE (the Porte Parole polls the
// status bit; no line reaches the slot), the 64 kHz PWM DAC's analogue
// shape, power-up mis-behaviour (the data sheet warns of spurious sounds).

#ifndef POM2_MEA8000_H
#define POM2_MEA8000_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

class Mea8000
{
public:
    /// [B] data sheet p. 5-271: "Nominal crystal frequency is 3.84 MHz",
    /// p. 5-272: f_XTAL 3.84 typ, 4.00 max.
    static constexpr double kNominalClockHz = 3840000.0;
    /// [B] clock / 480 = the 8 kHz sample interpolator; x8 = the 64 kHz DAC.
    static constexpr int kClockDivider = 480;
    static constexpr int kSuperSampling = 8;
    static constexpr int kDesignRate = 8000;     ///< kNominalClockHz / 480

    enum class State : uint8_t { Stopped, WaitFirst, Started, Slowing };

    Mea8000();

    /// Power-up. [B] p. 5-276: STOP mode, CONT and ROE cleared ("equals the
    /// command 00011010").
    void reset();

    /// A0 = 0 write: the starting pitch, then frame bytes.
    void writeData(uint8_t value);
    /// A0 = 1 write: the command register.
    void writeCommand(uint8_t value);
    /// The status register's only bit, REQ, as bit 7; bits 0-6 are 0 here
    /// (the chip leaves them floating — the caller supplies the bus).
    uint8_t status() const { return acceptsByte() ? 0x80 : 0x00; }
    bool acceptsByte() const;

    /// Advance `ticks` output ticks while the chip is idle (not running):
    /// only the 8 ms grid moves. The card's fast path for silence.
    void idle(uint64_t ticks);

    /// One output tick: clock / (480 / 8), i.e. 64 kHz at 3.84 MHz. Returns
    /// the DAC level in [-32767, 32767]; 0 while the chip is idle.
    int16_t tick();
    /// True while the frame timer runs (MAME's timer armed).
    bool running() const { return running_; }
    State state() const { return state_; }
    int16_t output() const { return static_cast<int16_t>(output_); }

    /// Versioned, self-validating. `loadSnapshot` returns false and leaves
    /// the chip untouched on a blob that is not exactly one of ours.
    static constexpr std::size_t kSnapshotBytes = 107;
    /// Output ticks in the free-running 8 ms grid (departure 7).
    static constexpr int kGridTicks = 512;
    void appendSnapshot(std::vector<uint8_t>& out) const;
    bool loadSnapshot(const uint8_t* data, std::size_t size);

private:
    struct Filter {
        uint16_t fm = 0, lastFm = 0;
        uint16_t bw = 0, lastBw = 0;
        int32_t output = 0, lastOutput = 0;
    };

    int interp(int from, int to) const;
    int filterStep(int i, int input);
    int noiseGen();
    int freqGen();
    int computeSample();
    void shiftFrame();
    void decodeFrame();
    void startFrame();
    void stopFrame();

    // Machine state — everything below is in the snapshot.
    State state_ = State::Stopped;
    std::array<uint8_t, 4> buf_{};
    uint8_t bufPos_ = 0;
    uint8_t cont_ = 0, roe_ = 0;
    uint16_t frameLength_ = 0, framePos_ = 0, frameLog_ = 0;
    int16_t lastSample_ = 0, sample_ = 0;
    uint32_t phi_ = 0;
    std::array<Filter, 4> f_{};
    uint16_t lastAmpl_ = 0, ampl_ = 0;
    uint16_t lastPitch_ = 0, pitch_ = 0;
    uint8_t noise_ = 0;
    bool running_ = false;
    int32_t output_ = 0;
    uint16_t gridPos_ = 0;     ///< output ticks into the 8 ms grid
    uint8_t pitchWait_ = 0;    ///< 1 from a starting pitch to the next grid point
};

} // namespace pom2

#endif // POM2_MEA8000_H
