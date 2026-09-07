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

// Smoke test for IWMDevice — the MAME-faithful port at IWMDevice.{h,cpp}.
// Verifies the surface that's directly exercised by the Apple //c+ alt
// firmware cold-reset path:
//   1. Reset state matches MAME `iwm.cpp:48-69`.
//   2. Mode register handshake via $C0nF write + $C0nE read returns
//      the mode low-5-bits in the status register (MAME `iwm.cpp:259`).
//   3. Write-handshake register reads $0xBF (bit 7 ready, bit 6 clear)
//      from a fresh cold reset (MAME `iwm.cpp:57`).
//   4. control_ tracks Q6/Q7/motor/drive-select bits via even/odd
//      offset decode (MAME `iwm.cpp:148-158`).
//   5. Sync state machine is callable as a no-op when inactive (MAME
//      `iwm.cpp:337-338 sync` returns immediately when !m_active).
//
// The bit-shift transitions (SR_/SW_ states) are not exercised here —
// they need a real flux source from DiskImage and would belong in a
// dedicated flux-replay smoke test.

#include "IWMDevice.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using pom2::IWMDevice;

void testResetState()
{
    IWMDevice iwm;
    assert(iwm.mode()    == 0x00);
    assert(iwm.status()  == 0x00);
    assert(iwm.whd()     == 0xBF);
    assert(iwm.control() == 0x00);
    assert(iwm.data()    == 0x00);
    std::printf("  ok: cold reset state matches MAME iwm.cpp:48-69\n");
}

void testControlBits()
{
    IWMDevice iwm;
    // Offsets 8..15 toggle control bits 0..7 (even=clear, odd=set).
    iwm.write(0x9, 0);              // set bit 4 (motor enable)
    assert((iwm.control() & 0x10) != 0);
    iwm.write(0xB, 0);              // set bit 5 (drive 2 select)
    assert((iwm.control() & 0x20) != 0);
    iwm.write(0xD, 0);              // set bit 6 (Q6)
    assert((iwm.control() & 0x40) != 0);
    iwm.write(0xF, 0);              // set bit 7 (Q7)
    assert((iwm.control() & 0x80) != 0);
    iwm.write(0x8, 0);              // clear bit 4
    assert((iwm.control() & 0x10) == 0);
    std::printf("  ok: control register bit decode (MAME iwm.cpp:148-158)\n");
}

void testModeStatusEcho()
{
    IWMDevice iwm;
    // Step 1: motor on so write_mode_w fires when offset & 1.
    // Without active state the write goes through mode_w (inactive
    // path) — that's exactly what the //c+ alt firmware does at
    // $C0EF when probing the IWM (before any drive is engaged).
    iwm.write(0xD, 0);              // Q6 set
    iwm.write(0xF, 0x1F);           // mode_w(0x1F) — control 0xC0 + odd
    // Low 5 of status reflects low 5 of mode (MAME line 259).
    assert((iwm.status() & 0x1F) == 0x1F);
    // Read $C0nE with Q6 high → control 0x40 → returns status.
    // The IWM read also pre-runs control() which clears Q7 (offset 0xE
    // is even, low 4 bit = bit 7 of control → 0). Status low 5 must
    // still be the mode echo.
    const uint8_t v = iwm.read(0xE);
    assert((v & 0x1F) == 0x1F);
    // WPT bit 7 high (no disk loaded → write-protected by default in
    // POM2 IWM read).
    assert((v & 0x80) != 0);
    std::printf("  ok: mode_w → status echo via $C0nF / $C0nE (MAME iwm.cpp:248-269)\n");
}

void testWhdReadIdle()
{
    IWMDevice iwm;
    // Read $C0nC with Q7 high + Q6 low → control 0x80 → returns whd.
    // Cold value is 0xBF; the //c+ alt firmware leans on bit 7 = 1
    // ("ready") to make its boot-time IWM init proceed.
    iwm.write(0xF, 0);              // Q7 set (write mode)
    iwm.write(0xC, 0);              // Q6 clear (this read clears Q6)
    const uint8_t v = iwm.read(0xC);
    assert(v == 0xBF);
    std::printf("  ok: whd cold read = 0xBF (MAME iwm.cpp:57 / read case 0x80)\n");
}

void testSyncNoCrashWhenInactive()
{
    IWMDevice iwm;
    // sync() must early-return when m_active is IDLE (MAME line 337-338).
    // No disk attached → still safe.
    iwm.tick(1'000'000);
    iwm.sync(1'000'000);
    iwm.tick(2'000'000);
    iwm.sync(2'000'000);
    // No assertion needed — we're checking that the call doesn't
    // segfault, divide by zero, or otherwise mis-step.
    std::printf("  ok: sync() inactive path is safe\n");
}

void testDataWLatchedMode()
{
    // MAME `iwm.cpp:311-318 data_w`. The IWM clears WHD bit 7 ("no data
    // loaded") on a CPU data write ONLY when mode bit 0 is set (the
    // latched-handshake mode). Without mode bit 0, the bit stays set
    // and the FSM eventually fires an underrun if it tries to write.
    //
    // Pre-port POM2 cleared bit 7 on every sync+write data_w regardless
    // of mode bit 0, which silently bypassed the handshake gate. Two
    // fresh IWMs so the whd cold value (0xBF, bit 7 = 1) is the same
    // starting point on each branch.

    // --- Non-latched path (mode bit 0 = 0): whd bit 7 must survive. ---
    {
        IWMDevice iwm;
        // Mode_w fires when control 0xC0 + odd offset write while !active.
        iwm.write(0xD, 0);      // Q6 set
        iwm.write(0xF, 0x00);   // mode_w(0x00) — non-latched + sync
        // Engage write mode: motor on (active) then Q7 (write).
        iwm.write(0x9, 0);      // motor on → MODE_ACTIVE
        // whd cold value 0xBF has bit 7 set. Entering MODE_WRITE sets
        // bit 6 on top → whd = 0xFF. Verify bit 7 is still up before
        // the data write so the assertion below is meaningful.
        iwm.write(0xF, 0);      // Q7 set → MODE_WRITE (this is also a
                                // mode_w call site if !active — but we
                                // are active, so it falls to data_w
                                // path. Even offset → no dispatch.)
        // Actually offset 0xF is odd. With (control & 0xC0) == 0xC0
        // (Q7+Q6 both set) and we're active, this fires data_w(0).
        // That's fine — non-latched, so whd bit 7 survives.
        assert((iwm.whd() & 0x80) != 0);
        // Explicit data_w(0xBB) via $C0EF write.
        iwm.write(0xF, 0xBB);
        assert((iwm.whd() & 0x80) != 0);   // non-latched: bit 7 sticks
        assert(iwm.data() == 0xBB);
    }

    // --- Latched path (mode bit 0 = 1): whd bit 7 must clear. ---
    {
        IWMDevice iwm;
        iwm.write(0xD, 0);      // Q6 set
        iwm.write(0xF, 0x01);   // mode_w(0x01) — latched + sync
        iwm.write(0x9, 0);      // motor on → MODE_ACTIVE
        iwm.write(0xF, 0);      // Q7 set → MODE_WRITE (also fires
                                // data_w(0) because we're active +
                                // control 0xC0 + odd offset). In
                                // latched mode this clears whd bit 7
                                // immediately.
        assert((iwm.whd() & 0x80) == 0);
        iwm.write(0xF, 0xAA);   // data_w(0xAA) — sanity
        assert((iwm.whd() & 0x80) == 0);
        assert(iwm.data() == 0xAA);
    }

    std::printf("  ok: data_w handshake gated on mode bit 0 (MAME iwm.cpp:311-318)\n");
}

void testDevselFiresOnReset()
{
    // MAME `iwm.cpp:79` fires `m_devsel_cb(0)` once during device_reset.
    // POM2 mirrors via `fireDevsel(0)` — but only if devsel_ was
    // non-zero (else idempotent). Test that the callback fires when
    // we install it before reset and the IWM had a non-zero devsel
    // from a prior session.
    IWMDevice iwm;
    int fires = 0;
    uint8_t lastValue = 0xFF;
    iwm.setDevselCallback([&](uint8_t v) { ++fires; lastValue = v; });
    // Set up an active drive selection: motor on with SEL=1 → devsel=2.
    iwm.write(0xB, 0);      // SEL set
    iwm.write(0x9, 0);      // motor on → devsel = 2
    assert(fires >= 1);
    assert(lastValue == 2);
    const int firesBeforeReset = fires;
    iwm.reset();
    // The reset must have notified the host that devsel went back to 0.
    assert(fires > firesBeforeReset);
    assert(lastValue == 0);
    std::printf("  ok: devsel callback fires on reset (MAME iwm.cpp:79)\n");
}

#include "Memory.h"

void testMemoryMirror()
{
    // Memory's $C0E0-$C0EF mirror only fires on `iicHasAltBank` (//c
    // 32K or //c+ profile). Build a synthetic 32K ROM with a valid
    // reset vector so `loadAppleIIRom(pickLower=true)` flips
    // iicHasAltBank = true, then verify that writes to $C0EF reach
    // the IWM mode register.
    Memory mem;
    IWMDevice iwm;
    mem.setIWM(&iwm);
    mem.setIIEMode(true);

    // Minimal 32K //c+-style image: reset vector at $FFFC = $FA62, plus
    // the MAME `apple2e.cpp:1275-1299` probe bytes — byte at offset
    // 0x3bc0 must be 0x00 (= //c-class) AND byte at 0x3bbf must be 0x05
    // (= //c+). Without the //c+ probe Memory keeps `isIIcPlus=false`
    // and never dispatches $C0E0-$C0EF to the IWM (Theme 6 D-2-1 fix).
    namespace fs = std::filesystem;
    std::vector<uint8_t> rom(32 * 1024, 0);
    rom[0x3BBF] = 0x05;  // //c+ marker (//c-class probe at 0x3BC0 is implicit 0)
    rom[0x3FFC] = 0x62; rom[0x3FFD] = 0xFA;
    rom[0x7FFC] = 0x88; rom[0x7FFD] = 0xC7;
    const fs::path tmp = fs::temp_directory_path() /
                         "pom2_iwm_mirror_smoke.rom";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rom.data()),
                static_cast<std::streamsize>(rom.size()));
    }
    assert(mem.loadAppleIIRom(tmp.string().c_str(), /*pickLower=*/true));
    fs::remove(tmp);

    // Simulate the //c+ alt firmware's IWM mode probe:
    //   1. set Q6 (read $C0ED)
    //   2. write mode register: STA $C0EF, value = $1F
    //   3. read status via $C0EE — low 5 bits should echo $1F
    (void)mem.memRead(0xC0ED);              // Q6 on, loadMode tracking in IWM
    mem.memWrite(0xC0EF, 0x1F);             // mode_w(0x1F)
    const uint8_t status = mem.memRead(0xC0EE);
    // The Memory mirror only runs the IWM in shadow — the value
    // returned to the CPU is still from the slot bus, which has no
    // DiskII in this minimal harness, so `status` here is the slot
    // bus's "no card" floating-bus value (0x00). But the IWM should
    // have seen the mode write — inspect via the getter.
    (void)status;
    assert((iwm.mode() & 0x1F) == 0x1F);
    assert((iwm.status() & 0x1F) == 0x1F);
    std::printf("  ok: Memory $C0E0-$C0EF mirror reaches IWMDevice on //c+ profile\n");
}

// ─── The bit-cell window table (MAME iwm.cpp:303-329) ────────────────────
// All four mode-bit-4-3 rows, both halves, diffed against upstream. Apple
// firmware only ever selects 0x00 and 0x08, which is how `case 0x18` sat at
// 18 (upstream: 16) with every test and every disk in the corpus green.
void testWindowSizeTable()
{
    struct Row { uint8_t mode; uint64_t window; uint64_t half; };
    static const Row rows[] = {
        { 0x00, 28, 14 },
        { 0x08, 14,  7 },
        { 0x10, 36, 16 },
        { 0x18, 16,  8 },
    };
    for (const Row& row : rows) {
        IWMDevice iwm;
        // mode_w: write $C0nF with Q6 already high (MAME iwm.cpp:248-253).
        iwm.write(0xD, 0);                 // Q6 set
        iwm.write(0xF, row.mode);
        assert((iwm.mode() & 0x18) == row.mode);
        if (iwm.windowSize() != row.window ||
            iwm.halfWindowSize() != row.half) {
            std::fprintf(stderr,
                "FAIL: mode $%02X window %llu/%llu, MAME says %llu/%llu\n",
                row.mode,
                static_cast<unsigned long long>(iwm.windowSize()),
                static_cast<unsigned long long>(iwm.halfWindowSize()),
                static_cast<unsigned long long>(row.window),
                static_cast<unsigned long long>(row.half));
            std::abort();
        }
    }
    std::printf("  ok: window/half-window table matches MAME iwm.cpp:303-329\n");
}

// ─── A snapshot blob may not hand sync() an unwalkable timeline ───────────
//
// Fuzz finding F1 (2026-09-07). `lastSync_` and `now_` are absolute stamps on
// the same clock, restored from the file independently. With `now_` far ahead
// and `rw_` in a walking mode, `sync()`'s bit-cell walker closes the gap 14
// ticks at a time: the 104-byte blob below is ~5·10¹¹ iterations and ~1 GB of
// RSS, run from `phasesCb_` INSIDE loadSnapshotState, on the CPU worker,
// under `stateMutex` — machine and window frozen with no cancel. Reachable
// from a .pom2snap through Memory's //c+ IWM section, the Liron card blob and
// the //c external port.
//
// Two things are pinned: the blob is REJECTED (state untouched), and the
// device stays responsive afterwards. The wall-clock bound is deliberately
// enormous — the regression takes minutes to hours, so anything that finishes
// at all is the fixed behaviour.
void testSnapshotTimelineRejected()
{
    auto put64 = [](std::vector<uint8_t>& v, uint64_t x) {
        for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
    };
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
    };

    std::vector<uint8_t> blob;
    for (char c : std::string("IWM2")) blob.push_back(uint8_t(c));
    put64(blob, 0x000000FFFFFFFF00ull);   // now_      — ~1.1e12 CPU cycles
    put64(blob, 0);                       // revStart35_
    put64(blob, 0);                       // lastSync_ — the whole gap
    put64(blob, 0);                       // nextStateChange_
    put64(blob, 0);                       // syncUpdate_
    put64(blob, 0);                       // asyncUpdate_
    put64(blob, 0);                       // fluxWriteStart_
    put64(blob, 0);                       // delayDeadline_
    put32(blob, 0);                       // fluxWriteCount_
    put32(blob, 0);                       // q3Clock_
    blob.push_back(0);                    // q3ClockActive_
    put32(blob, 0);                       // qt_
    put32(blob, 1);                       // active_ = MODE_ACTIVE
    put32(blob, 4);                       // rw_     = MODE_WRITE  ← walks
    put32(blob, 0);                       // rwState_ = S_IDLE
    blob.push_back(0x00);                 // data_
    blob.push_back(0xFF);                 // whd_ — bit 6 set: the write
                                          //        walker keeps going
    blob.push_back(0x00);                 // mode_
    blob.push_back(0x20);                 // status_
    blob.push_back(0xF0);                 // control_
    blob.push_back(0x00);                 // rwBitCount_
    blob.push_back(0x00);                 // rsh_
    blob.push_back(0x00);                 // wsh_
    blob.push_back(0x02);                 // devsel_
    blob.push_back(0x0F);                 // phases_
    blob.push_back(0x00);                 // writeDataLoaded_
    assert(blob.size() == 104);

    IWMDevice iwm;
    const uint8_t modeBefore   = iwm.mode();
    const uint8_t statusBefore = iwm.status();
    const uint8_t whdBefore    = iwm.whd();

    const auto t0 = std::chrono::steady_clock::now();
    const bool loaded = iwm.loadSnapshotState(blob.data(), blob.size());
    assert(!loaded && "an implausible IWM timeline must be rejected");
    assert(iwm.mode()   == modeBefore);
    assert(iwm.status() == statusBefore);
    assert(iwm.whd()    == whdBefore);
    // Reads mutate control_/status_ by design (they ARE the soft switches),
    // so the untouched-state check above comes first; these only prove the
    // device is still responsive rather than walking the FSM for hours.
    for (uint8_t r = 0; r < 16; ++r) (void)iwm.read(r);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    assert(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 10
           && "loadSnapshotState + 16 reads must not walk the FSM for hours");
    std::printf("  ok: implausible snapshot timeline rejected, device untouched\n");
}

// The same hazard reached the honest way: a drive left MODE_IDLE for a long
// stretch (where sync() returns early and lastSync_ stops tracking) and then
// re-enabled. MAME has the same walker shape; POM2 bounds the catch-up so the
// worst case is a few milliseconds instead of unbounded.
void testSyncCatchUpIsBounded()
{
    IWMDevice iwm;
    iwm.write(0x9, 0);                 // motor on  → MODE_ACTIVE, rw = READ
    iwm.tick(1000);
    iwm.write(0x8, 0);                 // motor off → MODE_DELAY → drains idle
    iwm.tick(4'000'000);               // past the ~1.2 M-cycle disable delay
    iwm.write(0x9, 0);                 // motor back on, 500 emulated seconds on
    const auto t0 = std::chrono::steady_clock::now();
    iwm.tick(512'000'000ull);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    assert(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 10
           && "sync() must bound how much emulated time one call walks");
    std::printf("  ok: sync() catch-up is bounded after a long idle\n");
}

int main()
{
    testResetState();
    testWindowSizeTable();
    testSnapshotTimelineRejected();
    testSyncCatchUpIsBounded();
    testControlBits();
    testModeStatusEcho();
    testWhdReadIdle();
    testSyncNoCrashWhenInactive();
    testDataWLatchedMode();
    testDevselFiresOnReset();
    testMemoryMirror();
    std::printf("OK iwm_device_smoke\n");
    return 0;
}
