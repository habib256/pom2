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

// The 3.5" GCR read path, end to end, with no firmware in the picture.
//
// TODO.md § Storage asks for exactly this, and asks for it FIRST: "find out
// where the GCR read path diverges … either the firmware never gets a clean
// address field, or it does and the fault is further up. That answer decides
// everything below, and building a card first would only produce a card that
// does not boot." Three features are queued behind that one question (//c+
// on-board boot, a Liron card, the plain //c), so the question is worth a
// test rather than a session of poking at a boot ROM.
//
// What this drives, and in which direction:
//
//   Disk35Image blocks → Sony35Drive's zoned GCR encoder → flux transitions
//   → IWMDevice's bit-cell window walker → $C0EC reads → nibble stream →
//   sony35::decodeSectors → blocks again.
//
// Every stage is already pinned individually. The encoder has
// `smartport_35_smoke`, the decoder has the WOZ loader, the IWM has
// `iwm_device_smoke` — and NONE of them crosses the boundary between the
// encoder and the walker, which is where a bit-cell rate disagreement lives.
// A test that reads the drive through `debugCellStream()` (as the encoder's
// own test does) cannot see that class of fault at all: it is the IWM's
// sampling window that has to agree with the drive's cell period, and only
// this path exercises both.
//
// It also answers the question in the form the backlog asked it. The IWM's
// window size comes from mode bits 3-4, and the harness sweeps ALL FOUR
// settings and reports what each one decodes (set `POM2_DUMP_GCR=1` to see
// the table, plus per-track nibble and address-field counts). If a future
// firmware boot fails, this table says whether to look at the firmware's
// mode byte or below it.
//
// Deliberately NOT here: the //c+ firmware, the MIG, SmartPort dispatch. The
// whole point is to remove them from the picture.

#include "Disk35Image.h"
#include "IWMDevice.h"
#include "Sony35Drive.h"
#include "Sony35Gcr.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool gDump = false;

// ── The medium ───────────────────────────────────────────────────────────
// Every block carries a pattern derived from its own index, so a sector that
// decodes cleanly but lands on the wrong track/head/sector is a failure here
// rather than a silent success. That mapping (`blockIndexFor`) is the part
// of the format most likely to be off by a zone.
std::vector<uint8_t> makeImagePayload()
{
    std::vector<uint8_t> img(pom2::Disk35Image::kBytesPerImage, 0);
    for (uint32_t b = 0; b < pom2::Disk35Image::kBlockCount; ++b) {
        uint8_t* p = img.data() + static_cast<size_t>(b) * 512;
        for (int i = 0; i < 512; ++i)
            p[i] = static_cast<uint8_t>((b * 7 + i * 3 + (b >> 8)) & 0xFF);
    }
    // Disk35Image::loadFile sniffs a ProDOS volume key block at block 2.
    uint8_t* vol = img.data() + 2 * 512;
    vol[0] = 0x00; vol[1] = 0x00;
    vol[4] = 0xF5;
    vol[5] = 'P'; vol[6] = 'O'; vol[7] = 'M'; vol[8] = '3'; vol[9] = '5';
    return img;
}

// ── The drive's command bus ──────────────────────────────────────────────
// CA0/CA1/CA2 select a register, LSTRB's RISING edge fires it
// (Sony35Drive::seekPhaseW). Head select is a line, not a register, and it
// forms bit 3 of the same address — so every strobe here happens with side 0
// selected, or "step" (reg 1) would address MFMModeOn (reg 9) instead.
void strobe(pom2::Sony35Drive& drive, uint8_t reg)
{
    drive.seekPhaseW(static_cast<uint8_t>(reg & 0x07));          // LSTRB low
    drive.seekPhaseW(static_cast<uint8_t>((reg & 0x07) | 0x08)); // rising edge
    drive.seekPhaseW(static_cast<uint8_t>(reg & 0x07));          // release
}

bool seekTo(pom2::Sony35Drive& drive, int track)
{
    // Head select FIRST, and to side 0: `regSelect()` is
    // { HDSEL, CA2, CA1, CA0 }, so stepping while side 1 is selected
    // addresses registers 8-F — "step" (1) becomes "MFM mode on" (9), the
    // head never moves, and a `while (track() < n)` loop never ends. Cost
    // an afternoon the first time; hence the bound below as well.
    drive.ssW(false);
    strobe(drive, 0x0);                       // DirNext — toward track 79
    int guard = 0;
    while (drive.track() < track && ++guard < 200) strobe(drive, 0x1);
    strobe(drive, 0x4);                       // DirPrev — toward track 0
    while (drive.track() > track && ++guard < 400) strobe(drive, 0x1);
    if (drive.track() != track) {
        std::printf("FAIL: seek to track %d stalled at %d — the drive is not "
                    "stepping (check regSelect / LSTRB edge)\n",
                    track, drive.track());
        return false;
    }
    return true;
}

/// Read the data register the way firmware does: poll until bit 7 is set,
/// four CPU cycles per `LDA $C0EC / BPL`. Returns the disk bytes collected
/// over `cycles` of emulated time.
std::vector<uint8_t> readNibbles(pom2::IWMDevice& iwm, uint64_t& now,
                                 uint64_t cycles)
{
    // Every mode this harness uses sets bit 1 (async handshake), so the data
    // register goes stale two cycles after an access (IWMDevice::controlAccess
    // → asyncUpdate_). That is what stops a byte being collected twice, and
    // it is the real hardware's answer too — de-duplicating by value here
    // would silently drop the repeated bytes a GCR data field legitimately
    // contains, and cost a checksum instead.
    std::vector<uint8_t> out;
    const uint64_t until = now + cycles;
    while (now < until) {
        iwm.tick(now);
        const uint8_t v = iwm.read(0x0C);     // Q6 low → data register
        if (v & 0x80) out.push_back(v);
        now += 4;
    }
    return out;
}

/// Bring the IWM up on `drive` in read mode with the given mode byte, the
/// same order a driver uses: mode first (it is only writable while the drive
/// is off), then motor on, then Q6/Q7 low.
bool powerUp(pom2::IWMDevice& iwm, pom2::Sony35Drive& drive,
             uint8_t modeByte, uint64_t& now)
{
    // $C0E0-$C0EF: offsets 8..15 clear (even) or set (odd) control bits
    // 4..7 — motor, drive select, Q6, Q7. The mode register is written by a
    // write to an ODD offset while Q6 AND Q7 are both set and the drive is
    // off (IWMDevice::controlAccess → modeW).
    iwm.reset();
    iwm.setSony35(&drive);
    iwm.tick(now);
    iwm.write(0xD, 0);            // Q6 = 1 ┐ mode-register window
    iwm.write(0xF, modeByte);     // Q7 = 1 ┘ + the write itself
    iwm.write(0xC, 0);            // Q6 = 0
    iwm.write(0xE, 0);            // Q7 = 0 → read mode
    iwm.write(0x9, 0);            // motor enable (control bit 4)
    iwm.write(0xA, 0);            // drive select 1
    now += 64;                    // let MODE_ACTIVE settle
    iwm.tick(now);
    strobe(drive, 2);          // MotorOn is a command, not /ENBL
    strobe(drive, 0x2);           // MotorOn register, belt and braces
    now += 64;
    iwm.tick(now);
    // Did the mode actually take? Getting this wrong is silent: the sweep
    // below would run four identical passes at mode $00 and report the
    // answer for a mode nobody asked about.
    if (iwm.mode() != modeByte) {
        std::printf("FAIL: mode register write did not take — wrote $%02X, "
                    "reads back $%02X\n", modeByte, iwm.mode());
        return false;
    }
    return true;
}

struct TrackResult {
    size_t nibbles      = 0;
    int    addressMarks = 0;   // D5 AA 96 seen in the raw stream
    int    dataMarks    = 0;   // D5 AA AD
    int    sectors      = 0;   // decoded AND both checksums good
    int    mismatched   = 0;   // decoded but payload != image
};

int countMarks(const std::vector<uint8_t>& nib, uint8_t third)
{
    int n = 0;
    for (size_t i = 0; i + 2 < nib.size(); ++i)
        if (nib[i] == 0xD5 && nib[i + 1] == 0xAA && nib[i + 2] == third) ++n;
    return n;
}

/// `now` is threaded through every call and NEVER rewound. The IWM keeps
/// absolute cycle stamps (`lastSync_`, `nextStateChange_`, `revStart35_`) and
/// its `sync()` walker only moves forward: hand it a timestamp older than the
/// one it already reached and it sits frozen until emulated time catches up,
/// reading nothing. That is a documented hazard on the rewind path
/// (IWMDevice.h, appendSnapshotState) and it bites a test harness the same
/// way — the first track read works, every later one returns an empty stream.
TrackResult readTrack(pom2::IWMDevice& iwm, pom2::Sony35Drive& drive,
                      const std::vector<uint8_t>& image,
                      int track, bool side1, uint8_t modeByte, uint64_t& now)
{
    if (!powerUp(iwm, drive, modeByte, now)) return {};
    if (!seekTo(drive, track)) return {};
    drive.ssW(side1);
    iwm.setSony35(&drive);        // re-arm the revolution anchor after the seek
    now += 128;
    iwm.tick(now);

    // Two revolutions: one is enough in principle, but the first byte lands
    // mid-cell wherever the anchor fell, and a sector that straddles the
    // wrap would otherwise be lost for reasons that have nothing to do with
    // the encoder.
    const uint64_t rev = static_cast<uint64_t>(drive.cyclesPerRev());
    TrackResult r;
    const std::vector<uint8_t> nib = readNibbles(iwm, now, rev * 2);
    r.nibbles      = nib.size();
    r.addressMarks = countMarks(nib, 0x96);
    r.dataMarks    = countMarks(nib, 0xAD);

    std::map<int, std::vector<uint8_t>> got;
    pom2::sony35::decodeSectors(nib, track,
        [&](int t, int h, int s, const uint8_t* data) {
            if (t != track || h != (side1 ? 1 : 0)) return;
            got[s].assign(data, data + 512);
        });
    r.sectors = static_cast<int>(got.size());
    for (const auto& [sector, payload] : got) {
        const int blk = pom2::sony35::blockIndexFor(track, side1 ? 1 : 0, sector);
        const uint8_t* want = image.data() + static_cast<size_t>(blk) * 512;
        if (std::memcmp(want, payload.data(), 512) != 0) ++r.mismatched;
    }
    return r;
}

}  // namespace

/// The stream the ENCODER produced, read straight off the cells with no IWM
/// in the way (`Sony35Drive::debugCellStream` + the same self-sync alignment
/// the shifter uses). This is the control: if the ideal stream decodes and
/// the IWM's does not, the encoder and the decoder are exonerated and the
/// fault is in the flux/window path between them.
TrackResult idealTrack(pom2::Sony35Drive& drive,
                       const std::vector<uint8_t>& image,
                       int track, bool side1)
{
    drive.ssW(false);
    if (!seekTo(drive, track)) return {};
    drive.ssW(side1);
    const std::vector<uint8_t> nib =
        pom2::sony35::nibblesFromCells(drive.debugCellStream());
    TrackResult r;
    r.nibbles      = nib.size();
    r.addressMarks = countMarks(nib, 0x96);
    r.dataMarks    = countMarks(nib, 0xAD);
    std::map<int, std::vector<uint8_t>> got;
    pom2::sony35::decodeSectors(nib, track,
        [&](int t, int h, int sct, const uint8_t* data) {
            if (t != track || h != (side1 ? 1 : 0)) return;
            got[sct].assign(data, data + 512);
        });
    r.sectors = static_cast<int>(got.size());
    for (const auto& [sector, payload] : got) {
        const int blk = pom2::sony35::blockIndexFor(track, side1 ? 1 : 0, sector);
        if (std::memcmp(image.data() + static_cast<size_t>(blk) * 512,
                        payload.data(), 512) != 0) ++r.mismatched;
    }
    return r;
}

int main()
{
    gDump = std::getenv("POM2_DUMP_GCR") != nullptr;

    const std::vector<uint8_t> payload = makeImagePayload();
    const fs::path path = fs::temp_directory_path() / "pom2_sony35_readpath.po";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
    }

    pom2::Disk35Image img;
    if (!img.loadFile(path.string())) {
        std::printf("FAIL: could not load the synthetic 800K image\n");
        return 1;
    }
    pom2::Sony35Drive drive;
    drive.setImage(&img);
    drive.notifyMediaChange();
    pom2::IWMDevice iwm;

    int failures = 0;
    // Both halves run by default since 2026-09-01, the day the IWM half
    // started passing. POM2_GCR_CONTROL_ONLY=1 keeps just the encoder→decoder
    // control, which is the thing to reach for when the IWM half fails and
    // you need to know whether the format or the controller broke.
    const bool iwmPass = std::getenv("POM2_GCR_CONTROL_ONLY") == nullptr;

    // ── Part 1, the regression: encoder → decoder, every zone, both heads ─
    // This passes today, so it is what ctest runs. It is not a formality:
    // it is the only test that walks all five speed zones AND both heads
    // through `blockIndexFor`, which is the mapping most likely to be off by
    // a zone, and it is the control the IWM pass below is measured against.
    for (int track : { 0, 15, 16, 31, 32, 47, 48, 63, 64, 79 }) {
        for (bool side1 : { false, true }) {
            const int want = pom2::sony35::sectorsForTrack(track);
            const TrackResult r = idealTrack(drive, payload, track, side1);
            if (gDump)
                std::printf("  encoder→decoder track %2d head %d: %5zu "
                            "nibbles, %2d addr, %2d data, %2d/%2d sectors, "
                            "%d bad payloads\n",
                            track, side1 ? 1 : 0, r.nibbles, r.addressMarks,
                            r.dataMarks, r.sectors, want, r.mismatched);
            if (r.sectors != want) {
                std::printf("FAIL: encoder→decoder, track %d head %d: %d/%d "
                            "sectors (%d address fields in %zu nibbles)\n",
                            track, side1 ? 1 : 0, r.sectors, want,
                            r.addressMarks, r.nibbles);
                ++failures;
            }
            if (r.mismatched != 0) {
                std::printf("FAIL: track %d head %d: %d sectors decoded to the "
                            "wrong payload — sony35::blockIndexFor disagrees "
                            "with the encoder's zone layout\n",
                            track, side1 ? 1 : 0, r.mismatched);
                ++failures;
            }
        }
    }

    // ── Part 2: the same read THROUGH the IWM ───────────────────────────
    // This is what the harness was built to answer, and for one afternoon it
    // read 4 address fields per revolution and zero sectors while part 1 read
    // the same track perfectly. Two causes, both fixed the same day:
    //
    //  1. The IWM state machine was clocked in whole CPU cycles — two samples
    //     per 2.02-cycle Sony cell, so a window edge could not be placed
    //     inside a cell. It now runs on IWM ticks (7 per CPU cycle,
    //     `POM2_IWM_TICKS_PER_CPU_CYCLE`), where a cell is 14.17 units wide
    //     and MAME's window constants (28/14/36/18) are usable verbatim.
    //     That alone took address-field recovery from 4 to 17 of 24 — and
    //     still zero sectors, because of:
    //  2. `nextTransition(lastSync_ + 1)` where MAME asks from `lastSync_`.
    //     The extra tick dropped any flux landing exactly one tick after the
    //     last sync point, which is where a poll boundary parks it routinely:
    //     a 1 bit read as 0, about once every 35 bytes. Enough to pass a
    //     short address field and never a 700-byte data field, which is
    //     exactly the shape the numbers had.
    //
    // Only mode $0A works, and that is correct rather than a limitation: it
    // is the 14-tick window, and the Sony cell is 14.17 ticks. $02 and $12
    // (28 and 36 ticks) are the 4 µs 5.25" settings.
    if (!iwmPass) {
        std::printf("sony35_gcr_zones: OK — encoder→decoder round-trips all "
                    "five zones on both heads (control only)\n");
        fs::remove(path);
        return failures == 0 ? 0 : 1;
    }

    // Mode bits 3-4 pick the window size (IWMDevice::windowSize: 4/2/5/2 CPU
    // cycles); bit 1 is the async handshake, without which the data register
    // never self-clears and a polling loop reports one byte many times.
    const uint8_t kModes[] = { 0x02, 0x0A, 0x12, 0x1A };
    int workingModes = 0;
    uint8_t bestMode  = 0x0A;
    int bestSectors   = -1;
    int bestScore     = -1;
    if (gDump) std::printf("  mode | nibbles | D5AA96 | D5AAAD | sectors\n");
    uint64_t now = 1000;
    for (uint8_t m : kModes) {
        const TrackResult r = readTrack(iwm, drive, payload, 0, false, m, now);
        if (gDump)
            std::printf("   $%02X | %7zu | %6d | %6d | %d/12\n",
                        m, r.nibbles, r.addressMarks, r.dataMarks, r.sectors);
        // Rank by sectors, then by address fields: when nothing decodes, the
        // mode that at least finds prologues is the informative one.
        const int score = r.sectors * 1000 + r.addressMarks;
        if (score > bestScore) {
            bestScore = score; bestSectors = r.sectors; bestMode = m;
        }
        if (r.sectors == 12 && r.mismatched == 0) ++workingModes;
    }
    if (workingModes == 0) {
        std::printf("FAIL: no IWM window setting reads a full track. Best was "
                    "mode $%02X at %d/12 sectors, while the encoder→decoder "
                    "control above reads 12/12 — so the fault is in the "
                    "flux/window path, not in the format. Suspect the tick "
                    "clock (CpuClock.h) or the flux query in "
                    "IWMDevice::sync.\n",
                    bestMode, bestSectors);
        ++failures;
    }

    for (int track : { 0, 16, 32, 48, 64 }) {
        for (bool side1 : { false, true }) {
            const int want = pom2::sony35::sectorsForTrack(track);
            const TrackResult r =
                readTrack(iwm, drive, payload, track, side1, bestMode, now);
            if (gDump)
                std::printf("  IWM track %2d head %d: %6zu nibbles, %2d addr, "
                            "%2d data, %2d/%2d sectors\n",
                            track, side1 ? 1 : 0, r.nibbles, r.addressMarks,
                            r.dataMarks, r.sectors, want);
            if (r.sectors != want) {
                std::printf("FAIL: through the IWM, track %d head %d read "
                            "%d/%d sectors\n",
                            track, side1 ? 1 : 0, r.sectors, want);
                ++failures;
            }
            if (r.mismatched != 0) {
                std::printf("FAIL: through the IWM, track %d head %d: %d "
                            "sectors with the wrong payload\n",
                            track, side1 ? 1 : 0, r.mismatched);
                ++failures;
            }
        }
    }

    fs::remove(path);
    if (failures == 0)
        std::printf("sony35_iwm_read_path: OK — blocks → GCR → IWM windows → "
                    "nibbles → blocks, all five zones, both heads\n");

    return failures == 0 ? 0 : 1;
}
