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

// porte_parole_card — the Ediciel Porte Parole (PorteParoleCard.h) and its
// MEA8000 (Mea8000.h), offline and on a real 6502 bus.
//
//  1. Bus: status = REQ in D7 only, D0-D6 the floating bus ([B] MEA8000
//     data sheet Fig. 5 — MAME returns zeros there), at either A0; writes
//     decode A0 only, so $C0n2/$C0n3 are data/command again.
//  2. After the starting pitch REQ waits for the 8 ms grid (Philips TP101
//     p. 10); after a frame byte it drops for "about 2 µs".
//  3. REQ after the 4th byte of a frame waits for the running frame's end:
//     512 output ticks at 4 MHz / 60 = 7.68 ms, not the nominal 8 ms.
//  4. Pitch is the table value x 1.024 (TP101 Table 2) and scales with the
//     4 MHz crystal: code 50 sounds at 100 x 1.024 x 4 / 3.84 = 106.7 Hz
//     (MAME: 100 Hz). The first frame fades IN (TP101 p. 10; MAME fades it
//     out, which silenced a one-frame CONT loop). The DAC is 8 bits.
//  5. With CONT = 0 a stream that runs dry plays one fade-out frame and
//     stops (silence, REQ back); STOP is immediate; noise frames sound.
//  6. Ctrl-Reset leaves the chip talking (no reset pin); power stops it.
//  7. A snapshot taken mid-word resumes to the same samples.
//  8. The manual's own polling loop (Annexe 2: LDA $C0C0 / ROL / BCC,
//     STA $C0C0), run by the 6502 through Memory and the SlotBus, delivers
//     every byte and makes sound.

#include "M6502.h"
#include "Memory.h"
#include "Mea8000.h"
#include "PorteParoleCard.h"
#include "SlotBus.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using pom2::Mea8000;
using pom2::PorteParoleCard;

namespace {

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

constexpr double kCpuHz = 1022727.0;

// A vowel close to French "a": BW 125 Hz x4; FM3 2400, FM2 1179; FM1 740;
// amplitude code 15 (1.0); FD and PI in the 4th byte.
constexpr uint8_t kB1 = 0xAA, kB2 = (5 << 5) | 17, kB3 = (25 << 3) | 7;
uint8_t b4(int fd, int pi) { return static_cast<uint8_t>(0x80 | (fd << 5) | (pi & 0x1F)); }

bool req(PorteParoleCard& c, uint8_t low4 = 0) { return (c.deviceSelectRead(low4) & 0x80) != 0; }

// Poll REQ the way the driver does; returns the cycles waited.
int waitReq(PorteParoleCard& c, int limit = 200000)
{
    int waited = 0;
    while (!req(c) && waited < limit) { c.advanceCycles(2); waited += 2; }
    return waited;
}

void frame(PorteParoleCard& c, int fd, int pi)
{
    for (uint8_t v : {kB1, kB2, kB3, b4(fd, pi)}) {
        waitReq(c);
        c.deviceSelectWrite(0, v);
    }
}

double rms(const std::vector<float>& v)
{
    double s = 0; for (float x : v) s += double(x) * x;
    return v.empty() ? 0 : std::sqrt(s / double(v.size()));
}

} // namespace

int main()
{
    // ── 1. Bus decode ────────────────────────────────────────────────────
    {
        SlotBus bus;
        bus.setFloatingBusSource([] { return uint8_t{0x15}; });
        auto owned = std::make_unique<PorteParoleCard>(4);
        PorteParoleCard* card = owned.get();
        bus.plug(4, std::move(owned));
        const uint8_t s0 = bus.deviceSelectRead(0xC0C0);
        const uint8_t s1 = bus.deviceSelectRead(0xC0C1);
        std::printf("idle status: $C0C0=%02X $C0C1=%02X\n", s0, s1);
        expect(s0 == 0x95, "idle: REQ=1 in D7, D0-D6 from the floating bus");
        expect(s1 == 0x95, "A0 is a don't care on a read");
        bus.deviceSelectWrite(0xC0C2, 50);          // even = data: pitch
        expect(card->chipSnapshot().state() == Mea8000::State::WaitFirst,
               "$C0n2 is the data register (A0 only)");
        bus.deviceSelectWrite(0xC0C3, 0x10);        // odd = command: STOP
        expect(card->chipSnapshot().state() == Mea8000::State::Stopped,
               "$C0n3 is the command register");
    }

    // ── 2 + 3. REQ timing ────────────────────────────────────────────────
    {
        PorteParoleCard c(4);
        c.setCpuClock(kCpuHz);
        c.deviceSelectWrite(0, 50);
        const int gridWait = waitReq(c);
        std::printf("REQ after the starting pitch: %.3f ms\n", gridWait / kCpuHz * 1000.0);
        expect(gridWait > int(0.0075 * kCpuHz) && gridWait <= int(0.00769 * kCpuHz),
               "after a starting pitch REQ waits for the 8 ms grid (7.68 ms from power-up)");
        c.deviceSelectWrite(0, kB1);
        expect(!req(c), "REQ is low right after a frame byte");
        c.advanceCycles(3);   // ceil(2 us x 1.0227 MHz)
        expect(req(c), "REQ is back about 2 us later");
        for (uint8_t v : {kB2, kB3, b4(0, 0)}) { c.advanceCycles(3); c.deviceSelectWrite(0, v); }
        expect(c.chipSnapshot().state() == Mea8000::State::Started, "4 bytes start");
        int waited = 0;                         // frame 1 began at that write
        for (uint8_t v : {kB1, kB2, kB3, b4(0, 0)}) {
            waited += waitReq(c);
            c.deviceSelectWrite(0, v);
        }
        c.advanceCycles(4); waited += 4;
        expect(!req(c), "after the 4th byte REQ waits for the frame end");
        while (!req(c) && waited < 20000) { c.advanceCycles(1); ++waited; }
        const double ms = waited / kCpuHz * 1000.0;
        std::printf("frame 1 (FD=0) ended after %d cycles = %.3f ms\n", waited, ms);
        expect(std::fabs(ms - 7.68) < 0.05, "an 8 ms frame lasts 7.68 ms on the 4 MHz crystal");
    }

    // ── 4. Pitch follows the crystal ─────────────────────────────────────
    {
        PorteParoleCard c(4);
        c.setCpuClock(kCpuHz);
        c.setSampleRate(44100);
        c.deviceSelectWrite(1, 0x0C);           // CONT enable + CONT: repeat
        c.advanceCycles(8);
        c.deviceSelectWrite(0, 50);             // 100 Hz nominal
        frame(c, 3, 0);                         // 64 ms frame, steady pitch
        (void)c.drainSamples(1u << 20);
        c.advanceCycles(int(0.0154 * kCpuHz));  // first quarter of the fade-in
        const double early = rms(c.drainSamples(1u << 20));
        c.advanceCycles(int(0.0307 * kCpuHz));
        (void)c.drainSamples(1u << 20);
        c.advanceCycles(int(0.0154 * kCpuHz));  // last quarter
        const double late = rms(c.drainSamples(1u << 20));
        std::printf("first frame: rms %.4f in its first quarter, %.4f in its last\n", early, late);
        expect(late > 2.0 * early, "the first frame fades in, not out");
        expect((c.chipSnapshot().output() & 0xFF) == 0, "the DAC is 8 bits");
        c.advanceCycles(int(0.02 * kCpuHz));    // into the repeats
        (void)c.drainSamples(1u << 20);
        for (int i = 0; i < 40; ++i) c.advanceCycles(int(0.01 * kCpuHz));
        const auto s = c.drainSamples(1u << 20);
        double best = -1e30; int bestLag = 0;
        for (int lag = 380; lag <= 480; ++lag) {
            double acc = 0;
            for (std::size_t i = 0; i + lag < s.size(); ++i) acc += double(s[i]) * s[i + lag];
            if (acc > best) { best = acc; bestLag = lag; }
        }
        const double f0 = 44100.0 / bestLag;
        std::printf("pitch code 50: %zu samples, rms %.4f, period %d samples = %.1f Hz\n",
                    s.size(), rms(s), bestLag, f0);
        expect(rms(s) > 0.01, "a repeated (CONT) frame keeps sounding");
        expect(std::fabs(f0 - 106.67) < 1.5, "code 50 sounds at 106.7 Hz on 4 MHz");
    }

    // ── 5. Fade-out, STOP, noise ─────────────────────────────────────────
    {
        PorteParoleCard c(4);
        c.setCpuClock(kCpuHz);
        c.deviceSelectWrite(0, 60);
        frame(c, 0, 0);
        c.advanceCycles(int(0.0077 * kCpuHz));
        expect(c.chipSnapshot().state() == Mea8000::State::Slowing,
               "a dry stream with CONT=0 enters the fade-out frame");
        expect(!req(c), "no byte is accepted during the fade-out frame");
        c.advanceCycles(int(0.0078 * kCpuHz));
        expect(c.chipSnapshot().state() == Mea8000::State::Stopped, "then STOP");
        c.advanceCycles(8);
        expect(req(c), "REQ is back in STOP mode");
        (void)c.drainSamples(1u << 20);
        c.advanceCycles(int(0.01 * kCpuHz));
        expect(rms(c.drainSamples(1u << 20)) == 0.0, "STOP mode is silent");

        c.deviceSelectWrite(0, 60);
        frame(c, 2, 16);                        // PI = 16: noise, 32 ms
        c.advanceCycles(int(0.04 * kCpuHz));    // fade-in
        (void)c.drainSamples(1u << 20);
        frame(c, 2, 16);
        c.advanceCycles(int(0.02 * kCpuHz));
        const double noise = rms(c.drainSamples(1u << 20));
        std::printf("noise frame rms %.4f\n", noise);
        expect(noise > 0.005, "a noise frame sounds");
        c.deviceSelectWrite(1, 0x10);
        expect(c.chipSnapshot().state() == Mea8000::State::Stopped, "STOP is immediate");
        expect(c.chipSnapshot().output() == 0, "STOP silences the output");
    }

    // ── 6. Reset vs power ────────────────────────────────────────────────
    {
        PorteParoleCard c(4);
        c.setCpuClock(kCpuHz);
        c.deviceSelectWrite(1, 0x0C);
        c.deviceSelectWrite(0, 60);
        frame(c, 3, 0);
        c.onReset();
        expect(c.chipSnapshot().running(), "Ctrl-Reset does not reach the MEA8000");
        c.onPowerCycle();
        expect(!c.chipSnapshot().running() &&
               c.chipSnapshot().state() == Mea8000::State::Stopped, "power-up is STOP");
    }

    // ── 7. Snapshot mid-word ─────────────────────────────────────────────
    {
        PorteParoleCard a(4), b(4);
        for (auto* c : {&a, &b}) { c->setCpuClock(kCpuHz); c->setSampleRate(44100); }
        a.deviceSelectWrite(0, 55);
        frame(a, 1, 3);
        frame(a, 1, -4);
        a.advanceCycles(5003);
        std::vector<uint8_t> blob;
        a.appendSnapshotState(blob);
        b.loadSnapshotState(blob.data(), blob.size());
        std::vector<uint8_t> again;
        b.appendSnapshotState(again);
        expect(blob == again, "snapshot round-trips");
        (void)a.drainSamples(1u << 20);
        (void)b.drainSamples(1u << 20);
        for (auto* c : {&a, &b}) { frame(*c, 2, 16); c->advanceCycles(30011); }
        // The chip, its tick phase and the REQ window are the machine; the
        // host-rate queue and phase are not saved, so compare the machine.
        std::vector<uint8_t> ea, eb;
        a.appendSnapshotState(ea); b.appendSnapshotState(eb);
        expect(ea == eb, "a restored card goes on exactly as the original");
        const double ra = rms(a.drainSamples(1u << 20)), rb = rms(b.drainSamples(1u << 20));
        expect(ra > 0.01 && std::fabs(ra - rb) < 0.01 * ra, "and sounds the same");
        std::vector<uint8_t> junk = blob;
        junk[4 + 4] = 9;                        // the chip's state byte
        PorteParoleCard c(4);
        std::vector<uint8_t> before; c.appendSnapshotState(before);
        c.loadSnapshotState(junk.data(), junk.size());
        std::vector<uint8_t> after; c.appendSnapshotState(after);
        expect(before == after, "an invalid blob leaves the card alone");
    }

    // ── 8. The manual's driver loop on a real 6502 ───────────────────────
    {
        constexpr int kSlot = 4;
        Memory mem;
        M6502 cpu(&mem);
        cpu.hardReset();
        auto owned = std::make_unique<PorteParoleCard>(kSlot);
        PorteParoleCard* card = owned.get();
        card->setCpu(&cpu);
        card->setCpuClock(kCpuHz);
        card->setSampleRate(44100);
        mem.slotBus().plug(kSlot, std::move(owned));

        // Bytes: pitch, then 6 frames of 16 ms.
        std::vector<uint8_t> table = {55};
        for (int f = 0; f < 6; ++f)
            for (uint8_t v : {kB1, kB2, kB3, b4(1, f & 1 ? -2 : 2)}) table.push_back(v);
        for (std::size_t i = 0; i < table.size(); ++i) mem.memWrite(uint16_t(0x0380 + i), table[i]);
        const uint8_t prog[] = {
            0xA2, 0x00,             // 0300 LDX #0
            0xAD, 0xC0, 0xC0,       // 0302 LDA $C0C0   PRET (Annexe 2)
            0x2A,                   // 0305 ROL A
            0x90, 0xFA,             // 0306 BCC $0302
            0xBD, 0x80, 0x03,       // 0308 LDA $0380,X
            0x8D, 0xC0, 0xC0,       // 030B STA $C0C0   ECH1
            0xE8,                   // 030E INX
            0xE0, uint8_t(table.size()), // 030F CPX #n
            0xD0, 0xEF,             // 0311 BNE $0302
            0x4C, 0x13, 0x03 };     // 0313 JMP *
        for (std::size_t i = 0; i < sizeof prog; ++i) mem.memWrite(uint16_t(0x0300 + i), prog[i]);
        cpu.setProgramCounter(0x0300);
        const uint64_t start = mem.getCycleCounter();
        while (cpu.getProgramCounter() != 0x0313 &&
               mem.getCycleCounter() - start < uint64_t(kCpuHz)) cpu.run(50);
        const double ms = double(mem.getCycleCounter() - start) / kCpuHz * 1000.0;
        std::printf("driver loop: %zu bytes delivered in %.2f ms\n", table.size(), ms);
        expect(cpu.getProgramCounter() == 0x0313, "the polling loop delivers every byte");
        // Frames 1..4 must play out before the 6th can be queued: >= 4 x 15.36 ms.
        // ...plus up to 7.68 ms of grid wait after the starting pitch.
        expect(ms > 4 * 15.36 - 1 && ms < 6 * 15.36 + 8.7, "the loop is paced by REQ");
        while (mem.getCycleCounter() - start < uint64_t(0.2 * kCpuHz)) cpu.run(200);
        const auto s = card->drainSamples(1u << 20);
        std::printf("driver loop audio: %zu samples, rms %.4f\n", s.size(), rms(s));
        expect(rms(s) > 0.01, "and the card speaks");
    }

    std::printf("porte_parole_card: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
