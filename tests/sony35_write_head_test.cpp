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

// The 3.5" guest-write path: `Sony35Drive::writeFlux` → `decodeAndCommit`.
//
// Nothing in ctest exercised it before this file. It is the only route by
// which a program running on the emulated machine can change an 800K image
// through the IWM, and the commit is driven by what the CELLS say, not by
// what the drive knows.
//
// `decodeAndCommit` filtered the decoded address fields by TRACK
// (`Sony35Gcr.h`: "a stray address field from a neighbouring track would
// commit blocks the guest never wrote") but not by SIDE — while
// `writeStart()` latches `writeHead_` for exactly that reason. A sector whose
// side byte disagreed with the head that wrote it was therefore committed to
// the OTHER side of the platter: at track 20, eleven blocks (483-493) the
// head could not physically reach were overwritten, and the eleven blocks
// under the head (472-482) kept their old contents. A head cannot write the
// far side of a disk.
//
// Pinned here:
//   1. a full-revolution splice on (track, side) commits exactly that
//      (track, side)'s blocks — outer zone side 0 and an inner zone side 1;
//   2. a splice whose address fields name the other side commits NOTHING;
//   3. a write-protected medium commits nothing and stays clean.

#include "CpuClock.h"
#include "Disk35Image.h"
#include "Sony35Drive.h"
#include "Sony35Gcr.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBlocks = 1600;

/// Two distinguishable 800K payloads. Block b of variant v is a function of
/// both, so a sector landing on the wrong block or from the wrong disk shows.
std::vector<uint8_t> payload(int variant)
{
    std::vector<uint8_t> p(std::size_t(kBlocks) * 512);
    for (std::size_t b = 0; b < kBlocks; ++b)
        for (int i = 0; i < 512; ++i)
            p[b * 512 + i] =
                static_cast<uint8_t>((b * 7 + i * 3 + variant * 101) & 0xFF);
    p[2 * 512 + 4] = 0xF5;                      // "looks ProDOS" at block 2
    return p;
}

std::string writePo(const char* name, const std::vector<uint8_t>& p)
{
    const fs::path f = fs::temp_directory_path() / name;
    std::ofstream o(f, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(p.data()),
            static_cast<std::streamsize>(p.size()));
    return f.string();
}

/// CA0/CA1/CA2 pick the register, LSTRB's rising edge fires it. Head select
/// is bit 3 of the same address, so every strobe happens on side 0 — see
/// `sony35_iwm_read_path_test.cpp`.
void strobe(pom2::Sony35Drive& d, uint8_t reg)
{
    d.seekPhaseW(static_cast<uint8_t>(reg & 7));
    d.seekPhaseW(static_cast<uint8_t>((reg & 7) | 8));
    d.seekPhaseW(static_cast<uint8_t>(reg & 7));
}

bool seekTo(pom2::Sony35Drive& d, int track)
{
    d.ssW(false);
    strobe(d, 0x0);                              // DirNext, toward track 79
    int guard = 0;
    while (d.track() < track && ++guard < 300) strobe(d, 0x1);
    strobe(d, 0x4);                              // DirPrev
    while (d.track() > track && ++guard < 600) strobe(d, 0x1);
    return d.track() == track;
}

/// Splice one whole revolution of `cells` onto whatever `dst`'s head is on.
/// `bitPeriodTicks = 0` is the grid-anchored form documented in
/// `Sony35Drive::writeFlux` for a harness handing it stamps that already sit
/// on the medium's own cell grid.
void spliceFullRev(pom2::Sony35Drive& dst, const std::vector<uint8_t>& cells)
{
    const int64_t period = static_cast<int64_t>(dst.cyclesPerRev()) *
                           POM2_IWM_TICKS_PER_CPU_CYCLE;
    const int n = static_cast<int>(cells.size());
    std::vector<int64_t> flux;
    flux.reserve(cells.size() / 2);
    for (int c = 0; c < n; ++c)
        if (cells[c]) flux.push_back((static_cast<int64_t>(c) * period) / n);
    dst.monW(false);                             // motor on: writeStart gates
    strobe(dst, 0x2);
    dst.writeStart();
    dst.writeFlux(0, period, flux.data(), static_cast<int>(flux.size()),
                  /*revStartTick=*/0, /*bitPeriodTicks=*/0);
}

int diffBlocks(pom2::Disk35Image& img, const std::vector<uint8_t>& want,
               std::vector<int>& which)
{
    which.clear();
    for (uint32_t b = 0; b < kBlocks; ++b) {
        uint8_t got[512];
        if (!img.readBlock(b, got)) { which.push_back(int(b)); continue; }
        if (std::memcmp(got, want.data() + std::size_t(b) * 512, 512) != 0)
            which.push_back(static_cast<int>(b));
    }
    return static_cast<int>(which.size());
}

void report(const char* what, const std::vector<int>& which)
{
    std::printf("FAIL: %s — %zu unexpected block(s):", what, which.size());
    for (std::size_t i = 0; i < which.size() && i < 16; ++i)
        std::printf(" %d", which[i]);
    std::printf("%s\n", which.size() > 16 ? " …" : "");
}

}  // namespace

int main()
{
    const std::vector<uint8_t> pa = payload(0), pb = payload(1);
    const std::string fa = writePo("pom2_sony35_wh_a.po", pa);
    const std::string fb = writePo("pom2_sony35_wh_b.po", pb);
    const std::string fc = writePo("pom2_sony35_wh_c.po", pa);
    const std::string fd = writePo("pom2_sony35_wh_d.po", pa);

    pom2::Disk35Image ia, ib;
    if (!ia.loadFile(fa) || !ib.loadFile(fb)) {
        std::printf("FAIL: could not mount the synthetic 800K images\n");
        return 1;
    }
    // ctest runs with POM2_MEDIA_WRITE_DEFAULT=protected; these are temp files
    // this test wrote itself.
    ia.setWriteBackEnabled(true);

    pom2::Sony35Drive da, db;
    da.setImage(&ia); da.notifyMediaChange();
    db.setImage(&ib); db.notifyMediaChange();

    int failures = 0;
    std::vector<uint8_t> want = pa;
    std::vector<int> which;

    // ── 1. Outer zone (12 sectors), side 0 ──────────────────────────────
    if (!seekTo(da, 0) || !seekTo(db, 0)) {
        std::printf("FAIL: seek to track 0 stalled\n");
        return 1;
    }
    da.ssW(false); db.ssW(false);
    spliceFullRev(da, db.debugCellStream());
    std::memcpy(want.data(), pb.data(), 12u * 512u);       // blocks 0..11
    if (diffBlocks(ia, want, which)) {
        report("full-revolution write on track 0 / side 0", which);
        ++failures;
    }

    // ── 2. Zone 2 (10 sectors), side 1 ──────────────────────────────────
    if (!seekTo(da, 40) || !seekTo(db, 40)) {
        std::printf("FAIL: seek to track 40 stalled\n");
        return 1;
    }
    da.ssW(true); db.ssW(true);
    spliceFullRev(da, db.debugCellStream());
    {
        const int n    = pom2::sony35::sectorsForTrack(40);
        const int base = pom2::sony35::blockIndexFor(40, 1, 0);
        std::memcpy(want.data() + std::size_t(base) * 512,
                    pb.data()   + std::size_t(base) * 512,
                    std::size_t(n) * 512);
    }
    if (diffBlocks(ia, want, which)) {
        report("full-revolution write on track 40 / side 1", which);
        ++failures;
    }

    // ── 3. Address fields naming the OTHER side commit nothing ──────────
    {
        pom2::Disk35Image ic;
        if (!ic.loadFile(fc)) { std::printf("FAIL: mount C\n"); return 1; }
        ic.setWriteBackEnabled(true);
        pom2::Sony35Drive dc;
        dc.setImage(&ic); dc.notifyMediaChange();
        if (!seekTo(dc, 20) || !seekTo(db, 20)) {
            std::printf("FAIL: seek to track 20 stalled\n");
            return 1;
        }
        dc.ssW(false);                 // the head is on SIDE 0 …
        db.ssW(true);                  // … the cells describe SIDE 1
        spliceFullRev(dc, db.debugCellStream());
        if (diffBlocks(ic, pa, which)) {
            const int n  = pom2::sony35::sectorsForTrack(20);
            const int b1 = pom2::sony35::blockIndexFor(20, 1, 0);
            report("side-1 cells written by a side-0 head", which);
            std::printf("      side 1 of track 20 is blocks %d..%d — a head "
                        "cannot write the far side of the platter; "
                        "decodeAndCommit must filter on the LATCHED head, "
                        "not only on the track\n", b1, b1 + n - 1);
            ++failures;
        }
    }

    // ── 4. A write-protected medium commits nothing ─────────────────────
    {
        pom2::Disk35Image id;
        if (!id.loadFile(fd)) { std::printf("FAIL: mount D\n"); return 1; }
        id.setWriteBackEnabled(false);                 // protected
        pom2::Sony35Drive dd;
        dd.setImage(&id); dd.notifyMediaChange();
        if (!seekTo(dd, 0) || !seekTo(db, 0)) {
            std::printf("FAIL: seek to track 0 stalled (WP case)\n");
            return 1;
        }
        dd.ssW(false); db.ssW(false);
        spliceFullRev(dd, db.debugCellStream());
        if (diffBlocks(id, pa, which) || id.hasUnsavedChanges()) {
            report("write-protected medium took a flux splice", which);
            ++failures;
        }
    }

    for (const std::string& p : { fa, fb, fc, fd }) {
        std::error_code ec;
        fs::remove(p, ec);
    }
    if (failures == 0)
        std::printf("sony35_write_head: OK — a 3.5\" flux splice commits "
                    "exactly the blocks under the latched (track, head)\n");
    return failures == 0 ? 0 : 1;
}
