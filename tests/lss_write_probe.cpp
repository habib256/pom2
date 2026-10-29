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

// lss_write_probe — manual: replay an RWTS-shaped write burst on a WOZ and
// print the flux the controller laid down, once with Apple's P6 (second
// argument: its path) and once with POM2's own sequencer, so the two can be
// compared transition for transition.
//
//   lss_write_probe <image.woz> <diskii_p6.rom>

#include "DiskIICard.h"
#include "DiskImage.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct Script {
    long now = 0;
    DiskIICard& c;
    void at(long t) { if (t > now) { c.advanceCycles(int(t - now)); now = t; } }
    void rd(long t, uint8_t sw) { at(t); c.deviceSelectRead(sw); }
    void wr(long t, uint8_t sw, uint8_t v) { at(t); c.deviceSelectWrite(sw, v); }
};

// The burst RWTS writes: Q6H, Q7L (sense), Q7H with the first byte, Q6L,
// then a load (STA $C0nD) and a shift (ORA $C0nC) per byte — 40 cycles
// apart for the self-sync $FFs, 32 for the rest — and Q7L to end it.
std::vector<int> writeBurst(const std::string& src, const std::string& p6, int lead,
                            int shiftDelay = 4, int entry = 4)
{
    const auto tmp = std::filesystem::temp_directory_path() / "pom2_lss_write_probe.woz";
    std::filesystem::copy_file(src, tmp, std::filesystem::copy_options::overwrite_existing);
    DiskIICard c(6);
    c.setWriteBackEnabled(true);
    if (!p6.empty()) c.loadLssRom(p6);
    if (!c.insertDisk(0, tmp.string())) { std::puts("insert failed"); return {}; }
    c.driveImage(0).setWriteBackEnabled(true);
    Script s{0, c};
    s.rd(0, 0x9);                                   // motor on
    long t = 100000 + lead;
    s.rd(t, 0xD); s.rd(t + 4, 0xE);                 // sense
    s.wr(t + 8, 0xF, 0xFF);                         // Q7H, first byte
    s.rd(t + 8 + entry, 0xC);                       // Q6L: shift
    t += 12 + 28;
    const uint8_t data[] = { 0xD5, 0xAA, 0xAD, 0x96, 0xFF, 0x9A, 0xB7, 0xEA,
                             0xCB, 0xDF, 0xF2, 0xA6, 0xDE, 0xAA, 0xEB, 0xFF };
    for (int i = 0; i < 6; ++i) { s.wr(t, 0xD, 0xFF); s.rd(t + shiftDelay, 0xC); t += 40; }
    for (uint8_t b : data)      { s.wr(t, 0xD, b);    s.rd(t + shiftDelay, 0xC); t += 32; }
    s.rd(t, 0xE);                                   // Q7L: splice
    s.rd(t + 4, 0xC);
    s.at(t + 1000);
    std::vector<int> ev = c.driveImage(0).fluxEvents(c.getHalfTrack() * 2);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return ev;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <image.woz> <diskii_p6.rom>\n", argv[0]); return 2; }
    if (argc > 3 && std::string(argv[3]) == "pair") {   // Apple | POM2, one case
        const int lead = std::atoi(argv[4]), sd = std::atoi(argv[5]);
        const auto a = writeBurst(argv[1], argv[2], lead, sd, 4);
        const auto b = writeBurst(argv[1], "", lead, sd, 4);
        std::vector<int> wa, wb;
        for (int e : a) if (e >= 200000 && e < 200800) wa.push_back(e);
        for (int e : b) if (e >= 200000 && e < 200800) wb.push_back(e);
        for (size_t i = 0; i < wa.size() || i < wb.size(); ++i)
            std::printf("%6d %6d%s\n", i < wa.size() ? wa[i] : -1, i < wb.size() ? wb[i] : -1,
                        (i < wa.size() && i < wb.size() && wa[i] == wb[i]) ? "" : "   <<");
        return 0;
    }
    if (argc > 3 && std::string(argv[3]) == "dump") {   // dump Apple's burst
        const int sd = argc > 4 ? std::atoi(argv[4]) : 4, en = argc > 5 ? std::atoi(argv[5]) : 4;
        const auto a = writeBurst(argv[1], argv[2], 0, sd, en);
        int prev = -1;
        for (int e : a) if (e >= 200000 && e < 201400) {
            std::printf("%d (+%d)\n", e, prev < 0 ? 0 : e - prev);
            prev = e;
        }
        return 0;
    }
    int worst = 0, cases = 0, same = 0;
    for (int lead = 0; lead < 8; ++lead)
        for (int sd = 4; sd < 8; ++sd)
            for (int en = 4; en < 8; ++en) {
                const auto a = writeBurst(argv[1], argv[2], lead, sd, en);
                const auto b = writeBurst(argv[1], "", lead, sd, en);
                ++cases;
                if (a == b) { ++same; continue; }
                worst = 1;
                std::printf("lead %d shift +%d entry +%d: Apple %zu events, POM2 %zu\n",
                            lead, sd, en, a.size(), b.size());
            }
    std::printf("%d of %d write bursts lay down the same flux as Apple's P6\n", same, cases);
    return worst;
}
