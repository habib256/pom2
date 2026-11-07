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

// porte_parole_probe — boots Ediciel's own Porte Parole disk on a 64 KB ][+
// with the card in slot 4 (its manual's slot) and a Disk II in slot 6, types
// what it is told to, and reports what the MEA8000 did: the screen, the
// seconds the chip spent talking, and a WAV of the card's output under
// probeOutDir(). A manual check: it asserts nothing, and the disk is not in
// the repository (apple2.org.za, "Ediciel Porte Parole v11 Side 1.dsk").
//
//   porte_parole_probe <side1.dsk> [--seconds N] [--type SEC:TEXT]... [--out DIR]
//
// TEXT takes \r for RETURN. A copy of the disk is booted, never the file.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"
#include "PorteParoleCard.h"
#include "ProbeOutDir.h"
#include "SlotBus.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using pom2::PorteParoleCard;

namespace {

constexpr double kCpuHz = 1022727.0;

std::string screenText(const uint8_t* ram)
{
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x400 + (row & 7) * 0x80 + (row >> 3) * 0x28;
        std::string line;
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(ram[base + col] & 0x7F);
            line.push_back((c >= 0x20 && c < 0x7F) ? c : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
    }
    return out;
}

std::string unescape(const std::string& s)
{
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'r') { o.push_back('\r'); ++i; }
        else o.push_back(s[i]);
    }
    return o;
}

void writeWav(const std::string& path, const std::vector<float>& s, uint32_t rate)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(s.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    f.write("data", 4); u32(bytes);
    for (float v : s) {
        const auto x = static_cast<int16_t>(std::lround(std::fmax(-1.0, std::fmin(1.0, v)) * 32767));
        u16(static_cast<uint16_t>(x));
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <side1.dsk> [--seconds N] [--type SEC:TEXT]... [--out DIR]\n", argv[0]);
        return 1;
    }
    std::string disk = argv[1], outArg;
    double seconds = 40;
    std::vector<std::pair<double, std::string>> typed;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--out" && i + 1 < argc) outArg = argv[++i];
        else if (a == "--type" && i + 1 < argc) {
            const std::string v = argv[++i];
            const auto colon = v.find(':');
            if (colon != std::string::npos)
                typed.emplace_back(std::atof(v.substr(0, colon).c_str()), unescape(v.substr(colon + 1)));
        }
    }
    const std::string outDir = pom2test::probeOutDir(outArg);
    const fs::path copy = fs::path(outDir) / "porte_parole_side1.dsk";
    std::error_code ec;
    fs::copy_file(disk, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) { std::fprintf(stderr, "cannot copy %s: %s\n", disk.c_str(), ec.message().c_str()); return 1; }

    Memory mem;
    const char* rom = nullptr;
    for (const char* p : {"roms/apple2p.rom", "../roms/apple2p.rom", "roms/apple2.rom"})
        if (fs::exists(p)) { rom = p; break; }
    if (!rom || !mem.loadAppleIIRom(rom)) { std::fprintf(stderr, "no ][+ ROM\n"); return 1; }

    auto drive = std::make_unique<DiskIICard>();
    for (const char* p : {"roms/disk2.rom", "../roms/disk2.rom"})
        if (fs::exists(p)) { drive->loadBootRom(p); break; }
    if (!drive->insertDisk(copy.string())) { std::fprintf(stderr, "insertDisk failed\n"); return 1; }
    mem.slotBus().plug(6, std::move(drive));

    M6502 cpu(&mem);
    mem.setCpu(&cpu);
    auto owned = std::make_unique<PorteParoleCard>(4);
    PorteParoleCard* card = owned.get();
    card->setCpu(&cpu);
    card->setCpuClock(kCpuHz);
    card->setSampleRate(44100);
    mem.slotBus().plug(4, std::move(owned));

    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    cpu.hardReset();
    cpu.setProgramCounter(0xC600);

    std::vector<float> audio;
    std::size_t typedNext = 0;
    double talking = 0;
    int lastSecond = -1;
    const uint64_t total = uint64_t(seconds * kCpuHz);
    const uint64_t slice = uint64_t(kCpuHz / 100);   // 10 ms
    for (uint64_t done = 0; done < total;) {
        const double now = done / kCpuHz;
        while (typedNext < typed.size() && typed[typedNext].first <= now) {
            const auto& t = typed[typedNext++].second;
            mem.pasteText(t.c_str(), t.size());
            std::printf("[%.1fs] typed \"%s\"\n", now, t.c_str());
        }
        uint64_t ran = 0;
        while (ran < slice) ran += uint64_t(cpu.run(1024));
        done += ran;
        if (card->chipSnapshot().running()) talking += ran / kCpuHz;
        const auto s = card->drainSamples(1u << 20);
        audio.insert(audio.end(), s.begin(), s.end());
        if (int(now) / 10 != lastSecond) {
            lastSecond = int(now) / 10;
            std::printf("[%.0fs] PC=$%04X talking so far %.2f s\n", now, cpu.getProgramCounter(), talking);
        }
    }
    double sum = 0; for (float v : audio) sum += double(v) * v;
    const std::string wav = (fs::path(outDir) / "porte_parole.wav").string();
    writeWav(wav, audio, 44100);
    std::printf("\n%s\nchip talking %.2f s of %.0f s, output rms %.4f\nWAV: %s\n",
                screenText(mem.data()).c_str(), talking, seconds,
                audio.empty() ? 0.0 : std::sqrt(sum / double(audio.size())), wav.c_str());
    return 0;
}
