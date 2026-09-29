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

// videx_videoterm_snapshot — the "VDXT" v1 blob: VRAM, the $CC00 window's
// bank, the 6845's 18 registers and its address latch; never the ROMs.
//
//   * round trip into a fresh card reproduces the blob and the picture
//   * a foreign blob, a short one (every truncation), a wrong version and a
//     blob carrying a bit its register cannot hold are all refused WHOLE —
//     nothing half-applied
//   * the AN0 soft switch (the Videoterm's video switch) travels in the
//     Memory snapshot, and a restore republishes it for the display

#include "Memory.h"
#include "VidexVideotermCard.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

std::vector<uint8_t> blobOf(const VidexVideotermCard& c)
{
    std::vector<uint8_t> b;
    c.appendSnapshotState(b);
    return b;
}

void seed(VidexVideotermCard& c)
{
    const uint8_t regs[16] = { 0x7B, 0x50, 0x5E, 0x2F, 0x1B, 0x08, 0x18, 0x19,
                               0x00, 0x08, 0xE0, 0x08, 0x01, 0x23, 0x02, 0x45 };
    for (int r = 0; r < 16; ++r) { c.deviceSelectWrite(0, uint8_t(r)); c.deviceSelectWrite(1, regs[r]); }
    for (int bank = 0; bank < 4; ++bank) {
        c.deviceSelectWrite(uint8_t(bank * 4 + 2), 0);   // bank select, no 6845 write
        for (int i = 0; i < 512; ++i)
            c.expansionRomWrite(uint16_t(0x400 + i), uint8_t(i * 3 + bank));
    }
    c.deviceSelectRead(0x9);        // leave the window on bank 2
    c.deviceSelectWrite(0, 0x0E);   // leave the latch on R14
    c.deviceSelectRead(0x9);
}

} // namespace

int main()
{
    VidexVideotermCard a(3);
    seed(a);
    const auto blob = blobOf(a);
    expect(blob.size() == 4 + 1 + 1 + 19 + 2048, "blob size: header + CRTC + 2 KB VRAM");
    expect(blob[0] == 'V' && blob[1] == 'D' && blob[2] == 'X' && blob[3] == 'T' && blob[4] == 1,
           "tagged VDXT v1");

    // Round trip.
    VidexVideotermCard b(3);
    b.loadSnapshotState(blob.data(), blob.size());
    expect(blobOf(b) == blob, "save -> load -> save reproduces the blob");
    expect(b.vramBank() == 1024, "bank restored");
    expect(b.crtc().latch() == 0x0E, "address latch restored");
    expect(b.crtc().startAddress() == 0x0123, "R12/R13 restored");
    std::vector<uint32_t> pa(720 * 216), pb(720 * 216);
    a.paintPicture(pa.data(), 0xFFFFFFFFu, 0xFF000000u, 12345, 1022727.0);
    b.paintPicture(pb.data(), 0xFFFFFFFFu, 0xFF000000u, 12345, 1022727.0);
    expect(pa == pb, "the restored card paints the same picture");
    // Junk appended is ignored (the blob is fixed-size).
    auto longer = blob;
    longer.push_back(0xAA);
    VidexVideotermCard c(3);
    c.loadSnapshotState(longer.data(), longer.size());
    expect(blobOf(c) == blob, "trailing junk: the captured state");

    // Refusals leave the card exactly as it was.
    VidexVideotermCard fresh(3);
    const auto freshBlob = blobOf(fresh);
    const auto untouched = [&](const std::vector<uint8_t>& bad, const char* what) {
        VidexVideotermCard d(3);
        d.loadSnapshotState(bad.data(), bad.size());
        expect(blobOf(d) == freshBlob, what);
    };
    for (std::size_t n = 0; n < blob.size(); ++n) {
        std::vector<uint8_t> shortBlob(blob.begin(), blob.begin() + static_cast<long>(n));
        VidexVideotermCard d(3);
        d.loadSnapshotState(shortBlob.data(), shortBlob.size());
        if (blobOf(d) != freshBlob) {
            std::printf("FAIL: truncation to %zu bytes applied something\n", n);
            ++failures;
            break;
        }
    }
    {
        VidexVideotermCard d(3);
        d.loadSnapshotState(nullptr, 100);
        expect(blobOf(d) == freshBlob, "null data: refused");
    }
    auto foreign = blob; foreign[0] = 'G';
    untouched(foreign, "foreign magic: refused");
    auto future = blob; future[4] = 2;
    untouched(future, "unknown version: refused");
    auto badBank = blob; badBank[5] = 4;
    untouched(badBank, "bank 4 does not exist: refused");
    auto badReg = blob; badReg[6 + 9] = 0x20;    // R9 holds 5 bits
    untouched(badReg, "a register bit its mask forbids: refused");
    auto badLatch = blob; badLatch[6 + 18] = 0x20;
    untouched(badLatch, "a 6-bit address latch: refused");
    auto lightPen = blob; lightPen[6 + 16] = 1;   // R16 read-only
    untouched(lightPen, "a light-pen address: refused");

    // AN0 — the soft video switch — travels in the Memory snapshot.
    {
        Memory m1;
        m1.memWrite(0xC059, 0);
        expect(m1.getDisplayState().an0, "$C059 sets AN0 in DisplayState");
        std::vector<uint8_t> snap;
        m1.appendSnapshotState(snap);
        Memory m2;
        expect(!m2.getDisplayState().an0, "fresh Memory: AN0 off");
        expect(m2.loadSnapshotState(snap.data(), snap.size()), "Memory snapshot loads");
        expect(m2.getDisplayState().an0, "AN0 restored");
        expect(m2.getDisplayStateAtFrameStart().an0,
               "AN0 republished in the frame-start snapshot the display reads");
        m2.memWrite(0xC058, 0);
        expect(!m2.getDisplayState().an0, "$C058 clears AN0");
        m2.memWrite(0xC059, 0);
        m2.resetSoftSwitches();
        expect(!m2.getDisplayState().an0, "a full reset clears AN0 (74LS259 /CLR)");
    }

    if (failures) {
        std::printf("videx_videoterm_snapshot: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("videx_videoterm_snapshot: ok\n");
    return 0;
}
