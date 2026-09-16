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

// A DOS 3.3 SAVE must land intact whatever the HOST does while it writes,
// and must not land at all on a notched disk. Three defects of the bug
// hunt of 2026-09-16, each of which corrupted or silently dropped a SAVE:
//
//   A. `commitInFlightWrite` reset `writeLineActive`. The burst continues
//      after the commit, so the next edge was taken for a transition that
//      never happened and one bit of the sector came out inverted. A flush
//      mid-SAVE (StorageCoordinator's flushAll, the WASM heartbeat) broke it.
//   B. `installPreparedLocked` / `takeEjectWriteBack` reset controller-wide
//      state — sequencer address, data register, write latch, the burst — on
//      ANY media change. Mounting or ejecting DRIVE 2 while DOS wrote DRIVE 1
//      cleared the WRITE bit with writeMode still on; the rest of the burst
//      ran read states, and write-back then committed the damage.
//   C. With no roms/diskii_p6.rom, the legacy gate answered the write-protect
//      sense's second read ($C0nE with Q6 high) with 0 — "never protected".
//      A SAVE onto a notched disk reported success and wrote nothing.
//
// Each disturbance runs on BOTH read gates, on a machine wired the way
// SlotCardFactory wires one with slots (no //c IWM hooks). The check is the
// guest's own: VERIFY after SAVE, and WRITE PROTECTED on the notched drive.
//
// Skips (77) without apple2.rom / disk2.rom / diskii_p6.rom / the DOS 3.3
// master image.

#include "DiskIICard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string findFirst(std::initializer_list<const char*> candidates)
{
    std::error_code ec;
    for (const char* c : candidates)
        if (fs::is_regular_file(c, ec)) return c;
    return {};
}

std::string screen(const uint8_t* ram)
{
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const uint8_t c = ram[base + col] & 0x7F;
            out += (c < 0x20) ? ' ' : static_cast<char>(c);
        }
        out += '\n';
    }
    return out;
}

void runCycles(M6502& cpu, long cycles)
{
    for (long n = 0; n < cycles;) n += cpu.run(1024);
}

// True once DOS is back at a prompt (or an error) BELOW the echoed command.
bool commandDone(const std::string& s, const char* cmd)
{
    const auto at = s.find(cmd);
    if (at == std::string::npos) return false;
    const auto eol = s.find('\n', at);
    return eol != std::string::npos && s.find(']', eol) != std::string::npos;
}

std::string gRom, gProm, gP6, gMaster;
fs::path gScratch;

enum class Disturb { None, InsertDrive2, EjectDrive2, Flush };

const char* name(Disturb d)
{
    switch (d) {
    case Disturb::None:         return "undisturbed";
    case Disturb::InsertDrive2: return "drive-2 insert mid-SAVE";
    case Disturb::EjectDrive2:  return "drive-2 eject mid-SAVE";
    case Disturb::Flush:        return "flush mid-SAVE";
    }
    return "?";
}

struct Machine {
    Memory                 mem;
    std::unique_ptr<M6502> cpu;
    DiskIICard*            card = nullptr;
};

// Boots the master in drive 1, a second copy in drive 2.
bool boot(Machine& m, bool legacy, bool notchDrive2)
{
    std::error_code ec;
    fs::copy_file(gMaster, gScratch / "d1.dsk", fs::copy_options::overwrite_existing, ec);
    fs::copy_file(gMaster, gScratch / "d2.dsk", fs::copy_options::overwrite_existing, ec);
    m.mem.setIIEMode(false);
    if (!m.mem.loadAppleIIRom(gRom.c_str())) return false;
    auto card = std::make_unique<DiskIICard>();
    card->setIwmHost(false);              // a machine with slots
    card->loadBootRom(gProm);
    if (!legacy) card->loadLssRom(gP6);
    card->setWriteBackEnabled(true);
    if (!card->insertDisk(0, (gScratch / "d1.dsk").string()) ||
        !card->insertDisk(1, (gScratch / "d2.dsk").string()))
        return false;
    if (notchDrive2) card->setDriveHostWriteProtected(1, true);
    m.card = card.get();
    m.mem.slotBus().plug(6, std::move(card));
    m.cpu = std::make_unique<M6502>(&m.mem);
    m.mem.setCpu(m.cpu.get());
    m.cpu->setCpuMode(M6502::CpuMode::NMOS);
    m.mem.clearRam();
    m.mem.resetSoftSwitches();
    m.mem.slotBus().reset();
    m.cpu->hardReset();
    m.cpu->setProgramCounter(0xC600);
    for (int i = 0; i < 400; ++i) {
        runCycles(*m.cpu, 1000000);
        if (screen(m.mem.data()).find("DOS VERSION 3.3") != std::string::npos) return true;
    }
    return false;
}

int saveAndVerify(bool legacy, Disturb d)
{
    const char* gate = legacy ? "legacy" : "LSS";
    Machine m;
    if (!boot(m, legacy, false)) {
        std::fprintf(stderr, "FAIL [%s, %s]: no boot\n", gate, name(d));
        return 1;
    }
    // Long enough that SAVE writes several sectors, so the disturbances
    // land inside bursts.
    const char* prog =
        "10 REM AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r"
        "20 REM BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\r";
    m.mem.pasteText(prog, std::strlen(prog));
    runCycles(*m.cpu, 20'000'000);
    m.mem.pasteRawKeys("SAVE PROBE\r", 11);

    uint64_t last = m.card->getWriteFlushCount();
    int disturbances = 0;
    bool done = false;
    for (long i = 0; i < 300000 && !done; ++i) {
        runCycles(*m.cpu, 200);
        const uint64_t f = m.card->getWriteFlushCount();
        // A flush count that just moved means a burst is in flight; spread
        // the disturbances across several sectors.
        if (d != Disturb::None && f != last && (f % 7) == 3) {
            switch (d) {
            case Disturb::InsertDrive2:
                m.card->insertDisk(1, (gScratch / "d2.dsk").string()); break;
            case Disturb::EjectDrive2:
                m.card->ejectDisk(1); break;
            case Disturb::Flush:
                (void)m.card->flushPendingWrites(); break;
            case Disturb::None: break;
            }
            ++disturbances;
        }
        last = f;
        if ((i % 5000) == 0) done = commandDone(screen(m.mem.data()), "SAVE PROBE");
    }
    if (!done) {
        std::fprintf(stderr, "FAIL [%s, %s]: SAVE never returned\n%s", gate, name(d),
                     screen(m.mem.data()).c_str());
        return 1;
    }
    if (d != Disturb::None && disturbances == 0) {
        std::fprintf(stderr, "FAIL [%s, %s]: the disturbance never fired — this "
                             "run proves nothing\n", gate, name(d));
        return 1;
    }
    if (!m.card->flushPendingWrites()) {
        std::fprintf(stderr, "FAIL [%s, %s]: write-back failed\n", gate, name(d));
        return 1;
    }
    m.mem.pasteRawKeys("VERIFY PROBE\r", 13);
    for (int i = 0; i < 150; ++i) {
        runCycles(*m.cpu, 1000000);
        if (commandDone(screen(m.mem.data()), "VERIFY PROBE")) break;
    }
    const std::string s = screen(m.mem.data());
    const auto at = s.find("VERIFY PROBE");
    if (at == std::string::npos || !commandDone(s, "VERIFY PROBE") ||
        s.find("ERROR", at) != std::string::npos) {
        std::fprintf(stderr, "FAIL [%s, %s]: the saved program does not VERIFY "
                             "(%d disturbances)\n%s", gate, name(d), disturbances, s.c_str());
        return 1;
    }
    std::printf("  %s gate, %s: SAVE verifies (%d disturbances)\n", gate, name(d), disturbances);
    return 0;
}

int saveOntoNotch(bool legacy)
{
    const char* gate = legacy ? "legacy" : "LSS";
    Machine m;
    if (!boot(m, legacy, true)) {
        std::fprintf(stderr, "FAIL [%s, notch]: no boot\n", gate);
        return 1;
    }
    const uint64_t before = m.card->getWriteFlushCount();
    m.mem.pasteRawKeys("SAVE PROBE,D2\r", 14);
    for (int i = 0; i < 300; ++i) {
        runCycles(*m.cpu, 1000000);
        if (commandDone(screen(m.mem.data()), "SAVE PROBE,D2")) break;
    }
    const std::string s = screen(m.mem.data());
    if (s.find("WRITE PROTECTED") == std::string::npos) {
        std::fprintf(stderr, "FAIL [%s, notch]: SAVE onto a notched disk did not "
                             "answer WRITE PROTECTED (%llu write flushes attempted)\n%s",
                     gate, static_cast<unsigned long long>(m.card->getWriteFlushCount() - before),
                     s.c_str());
        return 1;
    }
    if (m.card->hasUnsavedChanges(1)) {
        std::fprintf(stderr, "FAIL [%s, notch]: the notched disk was modified\n", gate);
        return 1;
    }
    std::printf("  %s gate: SAVE onto a notched disk answers WRITE PROTECTED\n", gate);
    return 0;
}

}  // namespace

int main()
{
    gRom    = findFirst({ "../roms/apple2.rom", "roms/apple2.rom", "../../roms/apple2.rom" });
    gProm   = findFirst({ "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    gP6     = findFirst({ "../roms/diskii_p6.rom", "roms/diskii_p6.rom", "../../roms/diskii_p6.rom" });
    gMaster = findFirst({ "../disks_5.4/dsk/dos33_master.dsk", "disks_5.4/dsk/dos33_master.dsk",
                          "../../disks_5.4/dsk/dos33_master.dsk" });
    if (gRom.empty() || gProm.empty() || gP6.empty() || gMaster.empty()) {
        std::printf("diskii_write_integrity SKIP: missing ROM or disk\n");
        return 77;
    }
    std::error_code ec;
    gScratch = fs::temp_directory_path() / "pom2_diskii_write_integrity";
    fs::create_directories(gScratch, ec);

    int failures = 0;
    for (bool legacy : { false, true }) {
        for (Disturb d : { Disturb::None, Disturb::Flush,
                           Disturb::InsertDrive2, Disturb::EjectDrive2 })
            failures += saveAndVerify(legacy, d);
        failures += saveOntoNotch(legacy);
    }
    if (failures) {
        std::printf("diskii_write_integrity: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("diskii_write_integrity OK\n");
    return 0;
}
