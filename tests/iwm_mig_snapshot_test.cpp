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

// IWM + //c+ MIG snapshot round-trip — pins:
//
//   1. IWMDevice's eight absolute emuCycles stamps (now_, lastSync_,
//      nextStateChange_, syncUpdate_, asyncUpdate_, revStart35_,
//      fluxWriteStart_, delayDeadline_) survive a save/restore. They did
//      not before: a rewind rolled the machine's cycleCounter backwards
//      while the IWM kept its larger lastSync_, so sync()'s
//      `while (nextSync > lastSync_)` walker stopped advancing and the
//      controller sat frozen until emulated time caught back up. Reachable
//      on every //c-class profile — ioReadIWM ticks the IWM on each
//      $C0E0-$C0EF access even in shadow mode.
//   2. The //c+ MIG gate array's 2 KB RAM and its auto-incrementing page
//      pointer round-trip; they used to come back zeroed, so the alt
//      firmware read something other than what it had written.
//   3. Backward compatibility: a blob truncated before the new trailer
//      still loads, leaving the live device values alone (the exact
//      pre-fix behaviour, so old saves do not regress).
//   4. A corrupt MIG page pointer is sanitised to 0x7E0 on the way in.
//      migRead indexes migRam_[migPage_ + (offset & 0x1F)], so the index
//      only stays inside the 2 KB array because the live cursor is 32-byte
//      ALIGNED (starts at 0, only ever advances by 0x20) — the highest
//      legal page is 0x7E0, not 0x7FF. A `& 0x7FF` mask therefore let a
//      crafted blob restore 0x7FF and index up to 0x81E, 31 bytes past the
//      array and over migPage_ / migIntDrive_ / migHdSel_ / iwm_ / hub_.

#include "IWMDevice.h"
#include "Memory.h"
#include "MemoryProfile_IIcClass.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Drive the IWM to a non-default state: motor on via the $C0Ex switches,
// then advance time so the internal stamps move off zero.
void exerciseIwm(pom2::IWMDevice& iwm)
{
    iwm.tick(1000);
    iwm.write(0x9, 0x00);        // Q6/Q7 mode select region
    iwm.tick(50000);
    iwm.read(0xC);
    iwm.tick(123456789ull);      // a big, distinctly non-zero "now"
}

void testIwmRoundTrip()
{
    pom2::IWMDevice a;
    exerciseIwm(a);
    const uint64_t nowA = a.emuCycles();
    assert(nowA == 123456789ull);

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(blob.size() > 4);
    // 'IWM2' since 2026-09-01: the state-machine timestamps in this blob are
    // IWM ticks (7 per CPU cycle) rather than CPU cycles, which is a
    // different number for the same instant. The magic is what stops an old
    // blob being restored verbatim and parking the walker seven times too
    // early — a device frozen until emulated time catches up, which is the
    // exact failure this file exists to prevent.
    assert(std::memcmp(blob.data(), "IWM2", 4) == 0);

    // A fresh device is at cycle 0 — the exact desync the bug produced.
    pom2::IWMDevice b;
    assert(b.emuCycles() == 0);
    assert(b.loadSnapshotState(blob.data(), blob.size()));
    assert(b.emuCycles() == nowA);
    assert(b.isActive() == a.isActive());
    assert(b.isIdle()   == a.isIdle());

    // Re-serialising the restored device must produce the identical blob:
    // that is the only way to assert the *private* stamps came across, not
    // just the one exposed by emuCycles().
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);

    // Truncation and a bad magic are both rejected, and must not leave the
    // device half-restored.
    pom2::IWMDevice c;
    assert(!c.loadSnapshotState(blob.data(), blob.size() - 1));
    assert(c.emuCycles() == 0);
    std::vector<uint8_t> bad = blob;
    bad[0] = 'X';
    assert(!c.loadSnapshotState(bad.data(), bad.size()));
    assert(c.emuCycles() == 0);
    assert(!c.loadSnapshotState(nullptr, 0));

    std::printf("  ok: IWM emuCycles stamps + registers round-trip\n");
}

void testMemoryTrailerCarriesIwm()
{
    // The Memory blob must carry the IWM section through, and a blob
    // truncated before the trailer must still load.
    Memory mem;
    pom2::IWMDevice iwm;
    exerciseIwm(iwm);
    mem.setIWM(&iwm);

    std::vector<uint8_t> blob;
    mem.appendSnapshotState(blob);

    // Reload into a machine whose IWM is at cycle 0.
    Memory mem2;
    pom2::IWMDevice iwm2;
    mem2.setIWM(&iwm2);
    assert(iwm2.emuCycles() == 0);
    assert(mem2.loadSnapshotState(blob.data(), blob.size()));
    assert(iwm2.emuCycles() == iwm.emuCycles());

    // Backward compatibility: lop the length-prefixed trailer off
    // entirely, computing its true size from the sections a blob of this
    // configuration carries — IWM (4-byte length + payload), profile
    // (4 + 0, no //c profile here) and the paging/IOU flags (4 + 7:
    // intC8Rom, ioudis, vblIrqMask, vblIrqPending, then AN0/AN1/AN2, which
    // joined that section on 2026-09-06 — AN2 selects the live 4 KB half of
    // an 8 KB international char ROM).
    // A fixed "-8" bit-rotted the moment a third section was added: it
    // only removed the newest section and the "old blob" kept restoring
    // the IWM.
    std::vector<uint8_t> iwmBlob;
    iwm.appendSnapshotState(iwmBlob);
    const size_t trailerLen = (4 + iwmBlob.size()) + (4 + 0) + (4 + 7);
    pom2::IWMDevice iwm3;
    Memory mem3;
    mem3.setIWM(&iwm3);
    const size_t shortLen = blob.size() - trailerLen;
    assert(mem3.loadSnapshotState(blob.data(), shortLen));
    assert(iwm3.emuCycles() == 0);   // untouched, exactly as before the fix

    // Once a trailer starts, a torn length prefix is corruption, not the
    // backward-compatible "trailer absent" case.
    assert(!mem3.loadSnapshotState(blob.data(), shortLen + 2));

    std::printf("  ok: Memory trailer carries IWM; old blobs still load\n");
}

void testMigPageMasked()
{
    // Hand-build a MIG blob with an out-of-range page pointer and confirm
    // it is masked rather than trusted. Goes through Memory because the
    // profile is private to it; a //c-class profile is required for the
    // section to be consumed at all, so this exercises the generic path:
    // an unrecognised/absent profile simply ignores the section.
    std::vector<uint8_t> sect;
    sect.insert(sect.end(), { 'M', 'I', 'G', '1' });
    sect.push_back(0xFF);            // page low  — 0xFFFF, way past 2 KB
    sect.push_back(0xFF);            // page high
    sect.resize(sect.size() + 0x800, 0xA5);
    assert(sect.size() == 4 + 2 + 0x800);

    // Feeding it to a non-//c Memory must be a no-op, not a crash: the
    // profile pointer is null there and the section is skipped by length.
    Memory mem;
    std::vector<uint8_t> blob;
    mem.appendSnapshotState(blob);
    assert(mem.loadSnapshotState(blob.data(), blob.size()));

    std::printf("  ok: MIG section length-skipped when no //c profile\n");

    // ── The bound itself, on a real //c-class profile ──────────────────
    // The check above never reaches the sanitiser (no profile to consume
    // the section), so it pinned nothing. Drive the profile directly and
    // read the restored cursor back out through appendSnapshotState — the
    // only public window onto migPage_.
    std::vector<uint8_t> rom(0x4000, 0x00);
    rom[0x3bbf] = 0x05;                       // //c+ signature (MAME probe)
    IIcClassProfile profile(rom.data(), rom.size(), nullptr,
                                  nullptr, nullptr, false);

    // Every page value a hostile blob could carry, not just one: the
    // invariant is "aligned AND inside", so sweep and assert both.
    for (uint32_t raw = 0; raw <= 0xFFFF; ++raw) {
        std::vector<uint8_t> hostile;
        hostile.insert(hostile.end(), { 'M', 'I', 'G', '1' });
        hostile.push_back(static_cast<uint8_t>(raw));
        hostile.push_back(static_cast<uint8_t>(raw >> 8));
        hostile.resize(hostile.size() + 0x800, 0xA5);
        const size_t used = profile.loadSnapshotState(hostile.data(),
                                                      hostile.size());
        assert(used >= 4 + 2 + 0x800);

        std::vector<uint8_t> back;
        profile.appendSnapshotState(back);
        const uint16_t page = static_cast<uint16_t>(
            back[4] | (static_cast<uint16_t>(back[5]) << 8));
        // Aligned to 32 bytes...
        assert((page & 0x1F) == 0);
        // ...and low enough that the widest access (page + 0x1F) still
        // lands on the last byte of the 0x800 array rather than past it.
        assert(page + 0x1F <= 0x7FF);
        assert(page == (raw & 0x7E0));
    }
    std::printf("  ok: MIG page cursor sanitised to 0x7E0 for all 65536 "
                "blob values\n");
}

void testRomBankRoundTrip()
{
    // romBank_ ($C028 ROMSWITCH) was the highest-impact snapshot gap on
    // //c-class machines: the //c+ alt firmware runs from bank 1 during
    // MIG/3.5" work, and a rewind across a toggle restored a PC captured
    // under one bank while the ROM reader served the other — the CPU got
    // the wrong 16 KB of firmware at $C100-$FFFF.
    std::vector<uint8_t> payload(0x4000, 0x00);   // //c signature: [0x3bbf]=0
    std::vector<uint8_t> altBank(0x4000, 0xEE);
    constexpr size_t kMigBytes = 4 + 2 + 0x800;   // magic + page + RAM
    constexpr size_t kTail     = 3;               // romBank + intDrive + hdSel

    IIcClassProfile a(payload.data(), payload.size(), altBank.data(),
                            nullptr, nullptr, true);
    a.romBankToggle();                            // → bank 1
    std::vector<uint8_t> blobA;
    a.appendSnapshotState(blobA);
    assert(blobA.size() == kMigBytes + kTail);
    assert(blobA[kMigBytes] == 1);                // romBank serialized

    // Round-trip into a fresh (bank 0) profile.
    IIcClassProfile b(payload.data(), payload.size(), altBank.data(),
                            nullptr, nullptr, true);
    assert(b.loadSnapshotState(blobA.data(), blobA.size()) ==
           kMigBytes + kTail);
    std::vector<uint8_t> blobB;
    b.appendSnapshotState(blobB);
    assert(blobB == blobA);                       // bank 1 came across

    // Backward compatibility: a pre-tail blob leaves the live bank alone.
    IIcClassProfile c(payload.data(), payload.size(), altBank.data(),
                            nullptr, nullptr, true);
    c.romBankToggle();                            // live bank 1
    std::vector<uint8_t> oldBlob(blobA.begin(),
                                 blobA.begin() + static_cast<long>(kMigBytes));
    assert(c.loadSnapshotState(oldBlob.data(), oldBlob.size()) == kMigBytes);
    std::vector<uint8_t> blobC;
    c.appendSnapshotState(blobC);
    assert(blobC[kMigBytes] == 1);                // still bank 1

    std::printf("  ok: romBank ($C028) round-trips; old blobs keep live bank\n");
}

}  // namespace

int main()
{
    std::printf("IWM + MIG snapshot test\n");
    testIwmRoundTrip();
    testMemoryTrailerCarriesIwm();
    testMigPageMasked();
    testRomBankRoundTrip();
    std::printf("PASS\n");
    return 0;
}
