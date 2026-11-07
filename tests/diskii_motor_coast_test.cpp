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
#include "DiskIIRoms.h"
#include "Memory.h"
#include "ResourcePaths.h"

#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <vector>

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
    pom2test::loadDiskIIDefaults(*card);
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

    // 4. No cancel: the drive stops after $C0E8 and the latch freezes. This
    //    hand-built card is an IWM host (the default), so the delay is the
    //    IWM's: 2^23 ticks of its 7.16 MHz clock, 1.172 s = 1'198'350 cycles
    //    (MAME iwm.cpp, IWMDevice.cpp). It was a round 1.0 s.
    (void)mem.memRead(0xC0E8);
    raw->advanceCycles(1'150'000);
    assert(raw->isMotorOn() && "the IWM's coast is 2^23 FCLK, not a round second");
    raw->advanceCycles(60'000);                          // 1.21 M
    assert(!raw->isMotorOn() && "the coast must end");
    {
        const Sample s = sampleLatch(mem, *raw, 16, 64);
        assert(s.distinct == 1 && "a stopped drive's latch is frozen");
        assert(mem.memRead(0xC0EC) == 0xFF);   // the frozen value is $FF
    }

    // 5. Under a 4x accelerator the coast is still ~1 s of REAL time — the
    //    556 on the controller does not see the CPU's clock. Counted in
    //    accelerated cycles it ended after ~0.25 s (bug hunt 2026-10-27).
    {
        raw->setStandardClock(1022727.0);
        raw->setCpuClock(4.0 * 1022727.0);
        (void)mem.memRead(0xC0E9);
        raw->advanceCycles(50'000);
        (void)mem.memRead(0xC0E8);
        raw->advanceCycles(2'000'000);                  // ~0.5 s at 4x
        assert(raw->isMotorOn() && "under 4x the coast ended in CPU time");
        raw->advanceCycles(2'900'000);                  // past the IWM's 1.172 s of real time
        assert(!raw->isMotorOn() && "…and it must still end");
        raw->setCpuClock(1022727.0);
    }

    // 6. The disk itself turns in real time too: over the same span of CPU
    //    cycles a 4x machine sees a quarter of the stream. The LSS used to
    //    run on CPU cycles, so an accelerated read saw the disk spin 4x fast.
    {
        auto changesOver = [&](double cpuHz) {
            raw->setCpuClock(cpuHz);
            (void)mem.memRead(0xC0E9);
            raw->advanceCycles(50'000);
            int changes = 0;
            uint8_t last = mem.memRead(0xC0EC);
            for (int i = 0; i < 8000; ++i) {                // 64 k CPU cycles
                raw->advanceCycles(8);
                const uint8_t v = mem.memRead(0xC0EC);
                if (v != last) ++changes;
                last = v;
            }
            return changes;
        };
        const int at1x = changesOver(1022727.0);
        const int at4x = changesOver(4.0 * 1022727.0);
        std::printf("  latch changes over 64k CPU cycles: 1x %d, 4x %d\n", at1x, at4x);
        assert(at1x > 100);
        assert(at4x * 3 < at1x && "an accelerator spun the disk faster");
        raw->setCpuClock(1022727.0);
    }

    // 7. The delay per controller, and the IWM's timer-off mode (bug hunt
    //    2026-10-31). A Disk II's 556 coasts R5 47k x C2 22uF x ln 3 =
    //    1.136 s = 1'161'820 cycles; an IWM with mode bit 2 set stops at once
    //    (MAME iwm.cpp: `if(m_mode & 0x04)` -> MODE_IDLE, no timer).
    {
        auto fresh = [&](bool iwm) {
            auto c = std::make_unique<DiskIICard>();
            pom2test::loadDiskIIDefaults(*c);
            c->setIwmHost(iwm);
            c->setWriteBackEnabled(false);
            assert(c->insertDisk(dsk.string()));
            return c;
        };
        {
            auto c = fresh(false);
            c->deviceSelectRead(0x9); c->advanceCycles(50'000);
            c->deviceSelectRead(0x8);
            c->advanceCycles(1'150'000);
            assert(c->isMotorOn() && "the 556 coasts 1.136 s");
            c->advanceCycles(25'000);                     // 1.175 M
            assert(!c->isMotorOn() && "the 556's coast must end by 1.175 M cycles");
        }
        {
            auto c = fresh(true);
            c->deviceSelectRead(0xD);                     // Q6 high
            c->deviceSelectWrite(0xF, 0x0F);              // mode: timer OFF (bit 2)
            c->deviceSelectRead(0xE);                     // back to Q7 low
            c->deviceSelectRead(0xC);                     // Q6 low
            c->deviceSelectRead(0x9); c->advanceCycles(50'000);
            assert(c->isMotorOn());
            c->deviceSelectRead(0x8);
            assert(!c->isMotorOn() && "IWM mode bit 2: motor-off is immediate");
            c->advanceCycles(10'000);
            assert(!c->isMotorOn());
            c->deviceSelectRead(0x9); c->advanceCycles(1'000);
            assert(c->isMotorOn() && "a motor-on after an immediate stop starts again");
        }
        {   // The SAME write with the motor running is write DATA, not a
            // mode: the IWM takes a mode only while idle (MAME iwm.cpp:283
            // dispatch, `if(m_active) data_w(data); else mode_w(data);`,
            // tag mame0287). A 5.25" write opens with an $FF sync byte sent
            // through $C0nF with Q6 already high; latching it as a mode
            // armed bit 2 and killed the coast for the rest of the session,
            // because nothing ever clears iwmMode
            // (docs/audits/parite-iwm-2026-11-01.md, constat 1).
            // Both decode paths own a copy of the hook, so both are pinned:
            // the legacy 32-cycle gate and the bit-level LSS.
            for (const bool bitLss : { false, true }) {
                auto c = std::make_unique<DiskIICard>();
                assert(c->loadBootRom(pom2test::diskIIRomPath("disk2.rom")));
                assert(c->loadLssRom(pom2test::diskIIRomPath("diskii_p6.rom"), bitLss));
                c->setIwmHost(true);
                c->setWriteBackEnabled(false);
                assert(c->insertDisk(dsk.string()));
                assert(c->usingBitLss() == bitLss);

                c->deviceSelectRead(0x9); c->advanceCycles(50'000);   // motor ON
                assert(c->isMotorOn());
                c->deviceSelectRead(0xD);                             // Q6 high
                const uint8_t stBefore = c->deviceSelectRead(0xE);
                c->deviceSelectWrite(0xF, 0xFF);                      // DATA, not a mode
                c->deviceSelectRead(0xC);
                c->deviceSelectRead(0xD);
                const uint8_t stAfter = c->deviceSelectRead(0xE);
                assert(stBefore == stAfter &&
                       "a $C0nF write with the motor on must not reach the mode register");
                c->deviceSelectRead(0xC);
                c->deviceSelectRead(0x8);                             // motor-off command
                assert(c->isMotorOn() &&
                       "mode bit 2 must NOT be armed: the IWM timer still coasts");
                c->advanceCycles(1'190'000);                          // IWM coast = 1'198'372
                assert(c->isMotorOn() && "the IWM coasts 1.172 s");
                c->advanceCycles(20'000);
                assert(!c->isMotorOn() && "and ends there");
            }
        }
        {   // A Disk II ignores that register: no mode, the 556 still coasts.
            auto c = fresh(false);
            c->deviceSelectRead(0xD);
            c->deviceSelectWrite(0xF, 0x0F);
            c->deviceSelectRead(0xE);
            c->deviceSelectRead(0xC);
            c->deviceSelectRead(0x9); c->advanceCycles(50'000);
            c->deviceSelectRead(0x8);
            assert(c->isMotorOn() && "a Disk II has no IWM mode register");
        }
    }

    // 7b. The platter resumes where it stopped. [D] MAME `bc9aec5a`
    //     (2026-10-03, "Resume the disk at its stopped angular position when
    //     the motor restarts"); until then upstream — and POM2 — re-anchored
    //     the revolution on the motor-on instant, so every spin-up read the
    //     disk at angular ZERO: a platter teleporting to index. Observable as
    //     a deterministic first sector after any motor restart.
    //     `docs/audits/parite-floppy-2026-11-01.md`, constat 1.
    {
        // Bit-level LSS: that is the path where motor-on runs `lssStart()`
        // and anchors the revolution, so `getFluxPosition` reports an angle.
        // (The legacy gate never anchors and reads 0 throughout.)
        auto c = std::make_unique<DiskIICard>();
        assert(c->loadBootRom(pom2test::diskIIRomPath("disk2.rom")));
        assert(c->loadLssRom(pom2test::diskIIRomPath("diskii_p6.rom"), /*selectBitLss=*/true));
        c->setIwmHost(false);
        c->setWriteBackEnabled(false);
        assert(c->insertDisk(dsk.string()));
        assert(c->usingBitLss());
        c->deviceSelectRead(0x9); c->advanceCycles(300'000);  // spinning, mid-track
        c->deviceSelectRead(0x8);                             // motor-off: the 556 coasts
        // Walk the coast one cycle at a time and keep the last angle read
        // while the spindle still turned: that is where the platter stopped.
        int stoppedAt = -1;
        for (int i = 0; i < 1'300'000 && c->isMotorOn(); ++i) {
            stoppedAt = c->getFluxPosition(0);
            c->advanceCycles(1);
        }
        assert(!c->isMotorOn() && "the 556 coast must have ended");
        assert(stoppedAt > 0 && "the angle was readable while it turned");
        assert(c->getFluxPosition(0) == 0 && "stopped: no anchor, position reads 0");

        c->deviceSelectRead(0x9); c->advanceCycles(100);      // restart, 100 cycles = 200 LSS
        const int resumed = c->getFluxPosition(0);
        // Old model: ~200 (index + 100 cycles). New: the stopped angle + 200.
        // The stop is captured within one cycle of `stoppedAt`, hence ±4.
        const int expected = stoppedAt + 200;
        assert(std::abs(resumed - expected) <= 4 &&
               "the platter resumes at the angle where it stopped, not at index");
        assert(resumed > 1000 && "and certainly not back at index");
    }

    // 7c. A drive swap with the motor on stops the old drive's platter where
    //     it is, and selecting it again resumes there ([D] MAME floppy.cpp:
    //     871-876 @ b2a182f0b7 — mon_w(true) saves the angle on every on→off,
    //     and wozfdc.cpp:269-290 calls it on the old drive of a $C0nA/$C0nB
    //     swap). selectDrive skipped the save: drive 1 came back at index
    //     (bug hunt 2026-11-07).
    {
        auto c = std::make_unique<DiskIICard>();
        assert(c->loadBootRom(pom2test::diskIIRomPath("disk2.rom")));
        assert(c->loadLssRom(pom2test::diskIIRomPath("diskii_p6.rom"), /*selectBitLss=*/true));
        c->setIwmHost(false);
        c->setWriteBackEnabled(false);
        assert(c->insertDisk(0, dsk.string()) && c->insertDisk(1, dsk.string()));
        assert(c->usingBitLss());
        c->deviceSelectRead(0x9); c->advanceCycles(300'000);   // drive 1 spinning
        const int before = c->getFluxPosition(0);
        assert(before > 1000);
        c->deviceSelectRead(0xB); c->advanceCycles(1'000);     // drive 2 selected
        c->deviceSelectRead(0xA);                               // back to drive 1
        const int after = c->getFluxPosition(0);
        assert(std::abs(after - before) <= 4 &&
               "a swapped-out drive resumes at its angle, not at index");
    }

    // 8. A CPU SLOWER than the bus (a FASTChip at 0.2 MHz: ratio 5 bus ticks
    //    per CPU cycle). The motor stop splits a batch at the deadline, so
    //    one 300 k-cycle batch and 300 k one-cycle steps across the stop
    //    leave the disk in the same place. The split assumed ratio <= 1 and
    //    put the deadline 5x late (bug hunt 2026-10-31).
    {
        auto run = [&](bool oneBatch) {
            auto c = std::make_unique<DiskIICard>();
            pom2test::loadDiskIIDefaults(*c);
            c->setIwmHost(false);
            c->setWriteBackEnabled(false);
            assert(c->insertDisk(dsk.string()));
            c->setStandardClock(1022727.0);
            c->setCpuClock(0.2 * 1022727.0);
            c->deviceSelectRead(0x9); c->advanceCycles(5'000);
            c->deviceSelectRead(0x8);                     // ~232 k CPU cycles of coast
            if (oneBatch) c->advanceCycles(300'000);
            else for (int i = 0; i < 300'000; ++i) c->advanceCycles(1);
            assert(!c->isMotorOn());
            c->deviceSelectRead(0x9);
            std::vector<uint8_t> stream;
            for (int i = 0; i < 512; ++i) {
                c->advanceCycles(3);
                stream.push_back(c->deviceSelectRead(0xC));
            }
            return stream;
        };
        assert(run(true) == run(false) &&
               "a slow CPU's batch ran the disk past the motor stop");
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
            {   // The bit-level LSS turns in real time too (see 6).
                auto changesOver = [&](double cpuHz) {
                    DiskIICard c;
                    pom2test::loadDiskIIDefaults(c);
                    c.setIwmHost(false);
                    c.setWriteBackEnabled(false);
                    assert(c.insertDisk(0, scratchWoz.string()) && c.usingBitLss());
                    c.setStandardClock(1022727.0);
                    c.setCpuClock(cpuHz);
                    c.deviceSelectRead(0x9); adv(c, 100000);
                    // Completed nibbles: rising edges of the latch's bit 7.
                    int nibbles = 0;
                    uint8_t last = c.deviceSelectRead(0xC);
                    for (int i = 0; i < 16000; ++i) {
                        c.advanceCycles(4);
                        const uint8_t v = c.deviceSelectRead(0xC);
                        if ((v & 0x80) && !(last & 0x80)) ++nibbles;
                        last = v;
                    }
                    return nibbles;
                };
                const int at1x = changesOver(1022727.0);
                const int at4x = changesOver(4.0 * 1022727.0);
                std::printf("  LSS nibbles over 64k CPU cycles: 1x %d, 4x %d\n", at1x, at4x);
                assert(at1x > 100);
                assert(at4x * 3 < at1x && "an accelerator spun the WOZ faster");
            }
            {   // A
                DiskIICard c;
                pom2test::loadDiskIIDefaults(c);
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
                pom2test::loadDiskIIDefaults(c);
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
                pom2test::loadDiskIIDefaults(c);
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
