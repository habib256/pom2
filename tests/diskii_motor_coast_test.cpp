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

// Disk II motor coast — the controller's 556 one-shot, on the LEGACY path.
//
// A motor-off ($C0E8) does not stop the spindle: the Disk II analog card
// keeps it driven for about one second, and DOS 3.3's RWTS RELIES on it —
// its "is the disk spinning?" check reads $C08C repeatedly BEFORE
// re-asserting $C0E9, and skips the one-second motor-on wait only if the
// latch moves. The bit-LSS path has modelled this since the MAME port
// (MODE_DELAY); the legacy nibble gate (plain .dsk with no P6 PROM — every
// headless test) stopped the drive instantly, so every RWTS call paid the
// full wait and a DOS 3.3 boot took ~115 s of machine time. What this pins:
//
//   1. While the motor is on, $C08C delivers a moving GCR stream.
//   2. After $C0E8 the drive still reports motor on and the latch keeps
//      moving through the ~1 s window (RWTS's check passes).
//   3. A $C0E9 inside the window cancels the countdown — still spinning
//      well past where the coast would have ended.
//   4. With no cancel, the drive stops after ~1 s: motor off, latch frozen
//      at $FF.

#include "DiskIICard.h"
#include "Memory.h"
#include "ResourcePaths.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

namespace {

// Advance the card and sample the latch: how many distinct values, and did
// any carry bit 7 (a completed nibble)?
struct Sample { int distinct; bool sawByte; };
Sample sampleLatch(Memory& mem, DiskIICard& card, int reads, int gapCycles)
{
    std::set<uint8_t> seen;
    bool byte7 = false;
    for (int i = 0; i < reads; ++i) {
        card.advanceCycles(gapCycles);
        const uint8_t v = mem.memRead(0xC0EC);
        seen.insert(v);
        if (v & 0x80) byte7 = true;
    }
    return { static_cast<int>(seen.size()), byte7 };
}

} // namespace

int main()
{
    // A 140 K .dsk with VARIED sector data — an all-zero disk nibblizes
    // into long runs of one GCR nibble and a short latch sample can land
    // entirely inside one, which is movement the sampler cannot see.
    const std::filesystem::path dsk =
        std::filesystem::temp_directory_path() / "pom2_motor_coast.dsk";
    {
        std::ofstream f(dsk, std::ios::binary | std::ios::trunc);
        std::vector<char> data(143'360);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<char>((i * 131) & 0xFF);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    Memory mem;
    auto card = std::make_unique<DiskIICard>();
    if (!card->insertDisk(dsk.string())) {
        std::fprintf(stderr, "insertDisk failed: %s\n", card->getLastError().c_str());
        return 1;
    }
    DiskIICard* raw = card.get();
    mem.slotBus().plug(6, std::move(card));

    (void)mem.memRead(0xC0E9);            // motor on
    raw->advanceCycles(50'000);
    assert(raw->isMotorOn());

    // 1. Spinning: the latch moves and completes nibbles. 256 reads × 128
    //    cycles ≈ a thousand nibbles — several sectors of varied data.
    {
        const Sample s = sampleLatch(mem, *raw, 256, 128);
        assert(s.distinct >= 3 && s.sawByte && "motor on must deliver a moving stream");
    }

    // 2. Motor off: the 556 keeps the spindle driven — RWTS's pre-$C0E9
    //    spin check must still see movement through the window.
    (void)mem.memRead(0xC0E8);
    assert(raw->isMotorOn() && "the spindle coasts; $C0E8 is not an instant stop");
    for (int slice = 0; slice < 8; ++slice) {           // ~0.9 s in ~110 ms steps
        raw->advanceCycles(80'000);
        const Sample s = sampleLatch(mem, *raw, 256, 128);   // + ~33 k cycles
        assert(raw->isMotorOn());
        assert(s.distinct >= 3 && "the latch must keep moving while coasting");
    }

    // 3. $C0E9 inside the window cancels the countdown.
    (void)mem.memRead(0xC0E9);
    raw->advanceCycles(2'200'000);                      // well past any coast
    assert(raw->isMotorOn() && "a motor-on during the coast cancels the stop");
    {
        const Sample s = sampleLatch(mem, *raw, 256, 128);
        assert(s.distinct >= 3);
    }

    // 4. No cancel: the drive stops ~1 s after $C0E8 and the latch freezes.
    (void)mem.memRead(0xC0E8);
    raw->advanceCycles(1'100'000);
    assert(!raw->isMotorOn() && "the coast must end");
    {
        const Sample s = sampleLatch(mem, *raw, 16, 64);
        assert(s.distinct == 1 && "a stopped drive's latch is frozen");
        assert(mem.memRead(0xC0EC) == 0xFF);   // the frozen value is $FF
    }

    // ── The coast across a READ-GATE switch ─────────────────────────────
    // With no roms/diskii_p6.rom the card runs the legacy gate and switches
    // to the bit-level LSS only while a WOZ is mounted. Both gates share ONE
    // motor-off countdown, and two switches lost it (bug hunt 2026-09-16):
    //   A. ejecting the only WOZ mid-coast zeroed the countdown with the motor
    //      flagged on — the drive never stopped;
    //   B. inserting a WOZ mid-coast promoted a coasting motor to "running":
    //      the countdown then cleared the flag under a live LSS and the next
    //      $C0E9 did nothing.
    {
        const std::string woz = pom2::findResource("disks_5.4/demo/fastloader/fastloader.woz");
        if (woz.empty()) {
            std::printf("  (gate-switch coast skipped: fastloader.woz not found)\n");
        } else {
            const auto scratchWoz = std::filesystem::temp_directory_path() / "pom2_motor_coast.woz";
            std::error_code cec;
            std::filesystem::copy_file(woz, scratchWoz,
                                       std::filesystem::copy_options::overwrite_existing, cec);
            auto adv = [](DiskIICard& c, long n) {
                while (n > 0) { const int k = n > 4096 ? 4096 : static_cast<int>(n); c.advanceCycles(k); n -= k; }
            };
            constexpr long kSecond = 1'022'727;
            {   // A
                DiskIICard c;
                c.setIwmHost(false);
                c.setWriteBackEnabled(false);
                assert(c.insertDisk(0, scratchWoz.string()) && c.usingBitLss());
                c.deviceSelectRead(0x9); adv(c, 100000);
                c.deviceSelectRead(0x8); adv(c, 100000);         // coasting
                c.ejectDisk(0);
                assert(!c.usingBitLss() && "the eject should demote to the legacy gate");
                adv(c, 5 * kSecond);
                assert(!c.isMotorOn() && "a motor coasting across the demotion never stopped");
            }
            {   // B
                DiskIICard c;
                c.setIwmHost(false);
                c.setWriteBackEnabled(false);
                assert(c.insertDisk(0, dsk.string()) && !c.usingBitLss());
                c.deviceSelectRead(0x9); adv(c, 100000);
                c.deviceSelectRead(0x8); adv(c, 100000);         // legacy coast
                assert(c.insertDisk(1, scratchWoz.string()) && c.usingBitLss());
                adv(c, 5 * kSecond);
                assert(!c.isMotorOn() && "the promoted coast never stopped");
                c.deviceSelectRead(0x9); adv(c, 1000);
                assert(c.isMotorOn() && "$C0E9 after a promoted coast did not start the motor");
            }
            std::filesystem::remove(scratchWoz, cec);
        }
    }

    // ── A step the opposing magnet cancels at once never happened ───────
    // The //c's SmartPort firmware addresses the bus with PH1 then, four CPU
    // cycles later, PH3 — opposing magnets. With the internal drive still
    // coasting, POM2 (like MAME) moved that head the instant PH1 came on, and
    // ProDOS's next seek landed a track short (bug hunt 2026-09-16). A real
    // stepper needs milliseconds to travel and the opposing magnet holds the
    // rotor where it was, so the step is undone; a step that has STOOD for
    // longer stays, exactly as before.
    {
        const std::string p6 = pom2::findResource("roms/diskii_p6.rom");
        if (p6.empty()) {
            std::printf("  (stepper-response case skipped: diskii_p6.rom not found)\n");
        } else {
            auto adv = [](DiskIICard& c, long n) {
                while (n > 0) { const int k = n > 4096 ? 4096 : static_cast<int>(n); c.advanceCycles(k); n -= k; }
            };
            auto run = [&](long pause) {
                DiskIICard c;
                c.setIwmHost(false);
                c.setWriteBackEnabled(false);
                assert(c.loadLssRom(p6));
                assert(c.insertDisk(0, dsk.string()) && c.usingBitLss());
                c.deviceSelectRead(0x9); adv(c, 1000);       // motor on
                // A real seek to track 1: PH1, PH2, PH1 off, PH2 off, with
                // the milliseconds a driver holds each phase.
                c.deviceSelectRead(0x3); adv(c, 5000);
                c.deviceSelectRead(0x5); adv(c, 5000);
                c.deviceSelectRead(0x2); adv(c, 5000);
                c.deviceSelectRead(0x4); adv(c, 5000);
                assert(c.getCurrentTrack(0) == 1 && "the ordinary seek no longer steps");
                c.deviceSelectRead(0x8); adv(c, 1000);       // motor off: coasting
                // The addressing pattern: PH1 on, then PH3 after `pause`.
                c.deviceSelectRead(0x3); adv(c, pause);
                c.deviceSelectRead(0x7); adv(c, 100);
                c.deviceSelectRead(0x2);                     // phases off again
                c.deviceSelectRead(0x6);
                return c.getCurrentTrack(0);
            };
            assert(run(4) == 1 &&
                   "PH1 cancelled by PH3 four cycles later still moved the head");
            assert(run(2000) == 0 &&
                   "a step that stood for 2 ms was undone — only a sub-response one may be");
        }
    }

    std::error_code ec;
    std::filesystem::remove(dsk, ec);
    std::printf("diskii motor coast: OK (spin, 1 s coast after $C0E8, $C0E9 cancel, stop, "
                "across a read-gate switch, and a step cancelled by the opposing magnet)\n");
    return 0;
}
