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

// sam_card — the S.A.M. 8-bit DAC card (SamCard.h, Dac8Audio.h), offline.
//
//  1. Level mapping = MAME DAC_8BIT_R2R × the a2sam 0.5 route.
//  2. Cycle-stamped writes land at the right OUTPUT SAMPLE with the right
//     value, including a write mid-sample (time-weighted average). Clock
//     441 kHz / 44.1 kHz = exactly 10 CPU cycles per sample.
//  3. Through a real Memory + SlotBus + 6502: `STA $C0n0` stamps the cycle
//     the store lands on, any $C0nX offset drives the DAC, and a read does
//     nothing to it.
//  4. The DC blocker drains a held level to silence; reset() empties the queue.

#include "Dac8Audio.h"
#include "M6502.h"
#include "Memory.h"
#include "SamCard.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using pom2::Dac8Audio;
using pom2::SamCard;

namespace {
int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
}

int main()
{
    // ── 1. Levels ────────────────────────────────────────────────────────
    expect(near(Dac8Audio::levelFor(0x00), -0.5f), "code $00 = -0.5");
    expect(near(Dac8Audio::levelFor(0x80),  0.0f), "code $80 = 0");
    expect(near(Dac8Audio::levelFor(0xFF), 0.5f * (255.0f / 128.0f - 1.0f)), "code $FF");

    // ── 2. Positions and values, offline ─────────────────────────────────
    {
        Dac8Audio dac;
        dac.setDcBlock(false);
        dac.setSampleRate(44100);
        dac.setCpuClock(441000.0);          // 10 cycles per output sample
        dac.write(1000, 0xFF);              // sample 100 starts here
        dac.write(1015, 0x80);              // half-way through sample 101
        dac.write(1030, 0xC0);              // sample 103
        std::vector<float> out(200);
        dac.fillAudioBuffer(out.data(), 200);
        const float lo = Dac8Audio::levelFor(0x00), ff = Dac8Audio::levelFor(0xFF);
        expect(near(out[0], lo) && near(out[99], lo), "idle samples hold code $00");
        expect(near(out[100], ff), "write at cycle 1000 fills sample 100");
        expect(near(out[101], 0.5f * ff + 0.5f * Dac8Audio::levelFor(0x80)),
               "a write mid-sample averages the two levels");
        expect(near(out[102], Dac8Audio::levelFor(0x80)), "sample 102 holds $80");
        expect(near(out[103], Dac8Audio::levelFor(0xC0)) &&
               near(out[199], Dac8Audio::levelFor(0xC0)), "$C0 holds to the end");
        std::printf("samples 99..103: %.4f %.4f %.4f %.4f %.4f\n",
                    out[99], out[100], out[101], out[102], out[103]);

        // Volume and mute scale the output.
        dac.setVolume(0.5f);
        std::vector<float> out2(10);
        dac.fillAudioBuffer(out2.data(), 10);
        expect(near(out2[0], 0.5f * Dac8Audio::levelFor(0xC0)), "volume scales");
        dac.setMuted(true);
        dac.fillAudioBuffer(out2.data(), 10);
        expect(out2[0] == 0.0f, "mute silences");
    }

    // ── 3. Through the bus, stamped by the CPU ───────────────────────────
    {
        constexpr int kSlot = 4;
        Memory mem;
        M6502  cpu(&mem);
        cpu.hardReset();
        auto card = std::make_unique<SamCard>(kSlot);
        card->setCpu(&cpu);
        SamCard* sam = card.get();
        mem.slotBus().plug(kSlot, std::move(card));
        sam->dac().setDcBlock(false);
        sam->dac().setSampleRate(44100);
        sam->dac().setCpuClock(441000.0);

        // $0300: LDA #$E0 / STA $C0C0 / LDA $C0C5 / LDA #$40 / STA $C0CF / JMP *
        const uint8_t prog[] = { 0xA9, 0xE0, 0x8D, 0xC0, 0xC0, 0xAD, 0xC5, 0xC0,
                                 0xA9, 0x40, 0x8D, 0xCF, 0xC0, 0x4C, 0x0D, 0x03 };
        for (uint16_t i = 0; i < sizeof prog; ++i) mem.memWrite(0x0300 + i, prog[i]);
        cpu.setProgramCounter(0x0300);
        cpu.run(13);   // LDA#(2) STA(4) LDA abs(4) LDA#(2) STA(4) = 16; run past both stores
        cpu.run(20);
        expect(sam->dac().lastCode() == 0x40, "the second store reached the DAC");
        expect(sam->dac().queuedWrites() == 2, "exactly two writes (the read did nothing)");
        {
            // A rewind / snapshot load fans onTimeJump() out to the cards:
            // the queue holds the abandoned future's stamps and must go.
            SamCard jumped(kSlot);
            jumped.dac().write(500000, 0x10);
            jumped.onTimeJump();
            expect(jumped.dac().queuedWrites() == 0, "onTimeJump drops the stamped queue");
        }

        std::vector<float> out(64);
        sam->dac().fillAudioBuffer(out.data(), 64);
        // The writes land ~6 and ~16 cycles in: the first two samples carry
        // $E0 then $40; by sample 2 only $40 is held.
        expect(near(out[2], Dac8Audio::levelFor(0x40)) &&
               near(out[63], Dac8Audio::levelFor(0x40)), "the last write holds");
        expect(out[0] > Dac8Audio::levelFor(0x00) + 0.01f,
               "the first write lands inside the first sample");
        std::printf("bus samples: %.4f %.4f %.4f\n", out[0], out[1], out[2]);
    }

    // ── 4. DC block + reset ──────────────────────────────────────────────
    {
        Dac8Audio dac;
        dac.setSampleRate(44100);
        dac.setCpuClock(441000.0);
        dac.write(10, 0xFF);
        std::vector<float> out(44100);
        dac.fillAudioBuffer(out.data(), 44100);
        expect(std::fabs(out.back()) < 1e-3f, "a held level decays to silence");
        dac.write(500000, 0x10);
        dac.reset();
        expect(dac.queuedWrites() == 0, "reset empties the queue");
    }

    // ── 5. A clock change does not re-time queued writes ─────────────────
    // A Zip at 4x drops to stock for 52 ms after a slot access, then speeds
    // back up. A pulse written while accelerated is 400 CPU cycles long but
    // only 100 cycles of real (stock) time: it must play as 10 samples at
    // 10 stock cycles a sample, whatever the clock is when the audio thread
    // reaches it. Stamping raw CPU cycles played it as 40.
    {
        constexpr int kSlot = 4;
        constexpr double kStock = 441000.0;   // 10 cycles per output sample
        Memory mem;
        M6502  cpu(&mem);
        cpu.hardReset();
        auto card = std::make_unique<SamCard>(kSlot);
        SamCard* sam = card.get();
        mem.slotBus().plug(kSlot, std::move(card));
        sam->dac().setDcBlock(false);
        sam->dac().setSampleRate(44100);
        sam->setStandardClock(kStock);
        sam->setCpuClock(kStock);
        mem.memWrite(0x0300, 0x4C); mem.memWrite(0x0301, 0x00); mem.memWrite(0x0302, 0x03);
        cpu.setProgramCounter(0x0300);   // JMP * — only advances the clock
        cpu.run(300);                    // some stock time first

        sam->setCpuClock(4.0 * kStock);  // accelerator engages
        const uint64_t start = mem.getCycleCounter();
        sam->deviceSelectWrite(0, 0xFF);
        while (mem.getCycleCounter() < start + 400) cpu.run(3);
        const uint64_t span = mem.getCycleCounter() - start;
        sam->deviceSelectWrite(0, 0x80);
        sam->setCpuClock(kStock);        // ...and drops before playback

        std::vector<float> out(8000);
        sam->dac().fillAudioBuffer(out.data(), static_cast<int>(out.size()));
        int high = 0;
        for (float v : out) if (v > Dac8Audio::levelFor(0xC0)) ++high;
        const int expected = static_cast<int>(span / 4 / 10);
        std::printf("accelerated pulse: %llu CPU cycles -> %d samples (expect ~%d)\n",
                    static_cast<unsigned long long>(span), high, expected);
        expect(high >= expected - 1 && high <= expected + 1,
               "an accelerated write plays at the speed it was made");
    }

    std::printf("sam_card: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
