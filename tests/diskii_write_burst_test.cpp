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

// Disk II write-burst boundary test (bug hunt 12).
//
// The controller streams a write as one continuous burst and flushes it to
// the image in pieces: every ~30 transitions, on Q7 falling edge, at
// motor-off, on a drive swap, and through commitInFlightWrite() when
// something outside the sequencer abandons the burst. Two of those
// boundaries were wrong.
//
//   A. commitInFlightWrite() spliced [writeStartTime, now) and then left
//      writeStartTime alone — unlike every other flush site, which re-bases
//      it to `now`. A burst that CONTINUED after the commit was therefore
//      flushed again as [old writeStartTime, now): the already-committed
//      window re-spliced with only the new transitions in it. writeFlux saw
//      a start that no longer matched writeFraming.nextCycle, treated it as
//      a NEW burst, re-anchored on the angular position of the OLD start and
//      threw away the half-assembled nibble — two nibbles lost and the rest
//      of the burst shifted one slot early. Reachable from
//      StorageCoordinator::flushAll (the WASM session heartbeat runs it
//      mid-session) and from an insert/eject on the other drive while this
//      one writes.
//
//   B. A stepper pulse moved headQuarterTrack[] while transitions were
//      still sitting in writeBuffer, laid down on the track the head was
//      leaving. Every flush site reads the head position AT FLUSH TIME, so
//      the buffered bits were spliced onto the track the head had just
//      arrived at — a write to track 0 silently mutating track 1.
//
// Method: a blank ($FF-filled) 232 960-byte .nib, so the framed nibbles land
// in the file byte-for-byte with no GCR decode in the way. Write 24 nibbles
// at DOS 3.3's 32-cycle pacing through the real LSS (roms/diskii_p6.rom),
// perturb the burst, and compare against the unperturbed control.
//
// Skips (77) without roms/disk2.rom + roms/diskii_p6.rom.

#include "DiskIICard.h"
#include "DiskImage.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) return c;
    }
    return {};
}

fs::path makeBlankNib(const char* tag)
{
    fs::path p = fs::temp_directory_path() /
                 (std::string("pom2_write_burst_") + tag + ".nib");
    std::vector<uint8_t> b(
        static_cast<size_t>(DiskImage::kTracks) * DiskImage::kNibblesPerTrack,
        0xFF);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
    return p;
}

int countWritten(const std::vector<uint8_t>& v, int track)
{
    int n = 0;
    const size_t base = static_cast<size_t>(track) * DiskImage::kNibblesPerTrack;
    for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i)
        if (v[base + i] != 0xFF) ++n;
    return n;
}

struct Rig {
    std::string bootRom, p6Rom;
};

// `midFlush`  — call flushPendingWrites() half-way through the burst.
// `midStep`   — pulse phase 2 after the last byte but before Q7L
//               (quarter-track 2 → 4, i.e. whole track 0 → 1). Phase 1 is
//               deliberately not used here: it is the write-inhibit wire.
std::vector<uint8_t> runBurst(const Rig& rig, bool midFlush, bool midStep,
                              const char* tag, int& headQtOut)
{
    fs::path p = makeBlankNib(tag);
    DiskIICard card;
    if (!card.loadBootRom(rig.bootRom) || !card.loadLssRom(rig.p6Rom)) {
        std::printf("FAIL: ROM load\n");
        std::exit(1);
    }
    card.setWriteBackEnabled(true);
    if (!card.insertDisk(0, p.string())) {
        std::printf("FAIL: insert: %s\n", card.getLastError().c_str());
        std::exit(1);
    }
    card.seekTrack0();
    card.deviceSelectWrite(0x9, 0);            // motor on
    card.advanceCycles(64);
    // Park the head on quarter-track 2 (still whole track 0) so a later
    // phase-2 pulse can step it, and drop every magnet again — phase 1 is
    // the write gate and must be off before the burst starts.
    card.deviceSelectWrite(0x3, 0); card.advanceCycles(200);   // phase 1 ON
    card.deviceSelectWrite(0x2, 0); card.advanceCycles(200);   // phase 1 OFF

    card.deviceSelectWrite(0xF, 0);            // Q7H — write mode
    // DOS 3.3's write loop: STA $C0nD (load the latch) / ORA $C0nC (shift).
    const auto nib = [&](uint8_t v) {
        card.deviceSelectWrite(0xD, v);
        card.advanceCycles(4);
        (void)card.deviceSelectRead(0xC);
        card.advanceCycles(28);
    };
    for (int i = 0; i < 12; ++i) nib(static_cast<uint8_t>(0xA0 + i));
    if (midFlush) (void)card.flushPendingWrites();
    for (int i = 0; i < 12; ++i) nib(static_cast<uint8_t>(0xC0 + i));
    if (midStep) {
        // The step lands AFTER the last byte and BEFORE Q7L: nothing more
        // is written, so every nibble of the burst belongs to track 0 and
        // track 1 must come back untouched. (A step in the MIDDLE of a
        // burst legitimately splits it — the head really is over the new
        // track for the rest — which is why the boundary is tested here.)
        card.deviceSelectWrite(0x5, 0);        // phase 2 ON  → qt 2 steps to 4
        card.advanceCycles(4);
        card.deviceSelectWrite(0x4, 0);        // phase 2 OFF
        card.advanceCycles(4);
    }
    card.deviceSelectWrite(0xE, 0);            // Q7L — flush
    card.deviceSelectWrite(0x8, 0);            // motor off
    card.advanceCycles(2'000'000);             // let the 556 one-shot expire
    if (!card.flushPendingWrites()) {
        std::printf("FAIL: final flush: %s\n", card.getLastError().c_str());
        std::exit(1);
    }
    headQtOut = card.getQuarterTrack(0);
    std::vector<uint8_t> out(
        static_cast<size_t>(DiskImage::kTracks) * DiskImage::kNibblesPerTrack);
    {
        std::ifstream f(p, std::ios::binary);
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    }
    std::error_code ec;
    fs::remove(p, ec);
    return out;
}

void dumpHead(const char* tag, const std::vector<uint8_t>& v)
{
    std::printf("      %-9s track0[0..31]:", tag);
    for (int i = 0; i < 32; ++i) std::printf(" %02X", v[i]);
    std::printf("\n");
}

}  // namespace

int main()
{
    Rig rig;
    rig.bootRom = findFirst({"roms/disk2.rom", "../roms/disk2.rom",
                             "../../roms/disk2.rom"});
    rig.p6Rom   = findFirst({"roms/diskii_p6.rom", "../roms/diskii_p6.rom",
                             "../../roms/diskii_p6.rom"});
    if (rig.bootRom.empty() || rig.p6Rom.empty()) {
        std::printf("SKIP: roms/disk2.rom + roms/diskii_p6.rom required\n");
        return 77;
    }

    int qtCtl = 0, qtFlush = 0, qtStep = 0;
    const auto control = runBurst(rig, false, false, "ctrl",  qtCtl);
    const auto flushed = runBurst(rig, true,  false, "flush", qtFlush);
    const auto stepped = runBurst(rig, false, true,  "step",  qtStep);

    bool ok = true;

    // ── A. a mid-burst commitInFlightWrite must be invisible on the medium
    const int ctlWritten = countWritten(control, 0);
    if (ctlWritten < 20) {
        std::printf("FAIL: control burst wrote only %d nibbles — the rig is "
                    "not driving the LSS write path\n", ctlWritten);
        return 1;
    }
    int diff = 0;
    for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i)
        if (control[i] != flushed[i]) ++diff;
    if (diff != 0) {
        std::printf("FAIL: flushPendingWrites() mid-burst changed the track: "
                    "%d of %d nibbles differ from the control "
                    "(control wrote %d, mid-flush wrote %d)\n",
                    diff, DiskImage::kNibblesPerTrack,
                    ctlWritten, countWritten(flushed, 0));
        dumpHead("control", control);
        dumpHead("midflush", flushed);
        ok = false;
    } else {
        std::printf("[ OK ] commitInFlightWrite mid-burst is transparent "
                    "(%d nibbles, byte-identical)\n", ctlWritten);
    }

    // ── B. a step mid-burst must not write to the destination track
    if (qtStep != 4) {
        std::printf("FAIL: the mid-burst phase pulse did not step the head "
                    "(quarter-track %d, expected 4)\n", qtStep);
        return 1;
    }
    const int t1 = countWritten(stepped, 1);
    const int t0 = countWritten(stepped, 0);
    if (t1 != 0) {
        std::printf("FAIL: a head step mid-write put %d nibble(s) on track 1 "
                    "— the guest only ever wrote over track 0 "
                    "(track 0 kept %d of %d)\n", t1, t0, ctlWritten);
        ok = false;
    } else if (t0 != ctlWritten) {
        std::printf("FAIL: a head step mid-write lost %d nibble(s) from "
                    "track 0 (%d vs %d in the control)\n",
                    ctlWritten - t0, t0, ctlWritten);
        ok = false;
    } else {
        std::printf("[ OK ] a head step mid-burst leaves the destination "
                    "track untouched (track 0 %d, track 1 %d)\n", t0, t1);
    }

    if (!ok) return 1;
    std::printf("PASS: Disk II write-burst boundaries\n");
    return 0;
}
