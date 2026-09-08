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

// CLI kiosk / positional-disk smoke test. Pins the two pure pieces the
// `--kiosk` + bare `POM2 <disk>` launcher relies on:
//
//   1. parseCli()          — positional disk capture + --kiosk flag, and
//                            that they compose with existing flags.
//   2. classifyDiskForSlot — extension/size → slot class (5.25 / 3.5 / HDV),
//                            the routing the insert+boot path keys off.
//
// Both are dependency-free of the live emulator (parseCli's Phase-C runner
// lives in CliRunner.cpp), so this links just CliDispatcher.cpp + DiskImage.cpp.

#include "CliDispatcher.h"
#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Build a mutable argv from string args (parseCli takes char*[]).
std::optional<pom2::CliPlan> parse(const std::vector<std::string>& args,
                                   bool& help)
{
    std::vector<std::vector<char>> store;
    std::vector<char*> argv;
    for (const auto& a : args) {
        store.emplace_back(a.begin(), a.end());
        store.back().push_back('\0');
        argv.push_back(store.back().data());
    }
    return pom2::parseCli(static_cast<int>(argv.size()), argv.data(), help);
}

// `--fujinet-slot` is what separates "the user chose slot N" from "POM2
// prefers 7". Without the distinction, a bare `--fujinet` was refused on a
// stock configuration, because slot 7's own first-run default is the Le Chat
// Mauve — so the documented headline invocation never worked, and the fallback
// docs/fujinet_plan.md specifies had nothing to key off.
void testFujiNetSlotExplicitness()
{
    bool help = false;

    auto bare = parse({"POM2", "--fujinet"}, help);
    assert(bare.has_value());
    assert(bare->fujiNet == pom2::CliPlan::FujiNetTransport::Tcp);
    assert(bare->fujiNetSlot == 7);          // still the preference…
    assert(!bare->fujiNetSlotExplicit);      // …but only a preference
    assert(bare->fujiNetPort == 1985);

    auto chosen = parse({"POM2", "--fujinet=1990", "--fujinet-slot", "3"}, help);
    assert(chosen.has_value());
    assert(chosen->fujiNetSlot == 3);
    assert(chosen->fujiNetSlotExplicit);     // an occupied 3 must stay an error
    assert(chosen->fujiNetPort == 1990);

    auto serial = parse({"POM2", "--fujinet-serial=/dev/ttyACM0"}, help);
    assert(serial.has_value());
    assert(serial->fujiNet == pom2::CliPlan::FujiNetTransport::Serial);
    assert(serial->fujiNetSerialPath == "/dev/ttyACM0");
    assert(!serial->fujiNetSlotExplicit);

    // Out-of-range slots are still rejected outright.
    assert(!parse({"POM2", "--fujinet", "--fujinet-slot", "8"}, help).has_value());
    // The same rule as every other numeric flag: trailing garbage is a
    // typo, not a slot. `atoi` used to read "3junk" as a perfectly good 3.
    assert(!parse({"POM2", "--fujinet", "--fujinet-slot", "3junk"}, help).has_value());
    assert(!parse({"POM2", "--fujinet", "--fujinet-slot", ""}, help).has_value());
    assert(!parse({"POM2", "--fujinet", "--fujinet-slot", "0"}, help).has_value());
}

// Bug hunt #10: `--ai-control=` and `--fujinet=` parsed their port with
// atoi, so `8080junk` armed 8080 and `4294967376` wrapped to a perfectly
// valid 80 — the control server bound to a port the user never typed.
void testPortValueRejectsGarbageAndOverflow()
{
    bool help = false;
    for (const char* bad : { "--ai-control=8080junk", "--ai-control=4294967376",
                             "--ai-control=99999", "--fujinet=1985x",
                             "--fujinet=4294968021" }) {
        assert(!parse({ "POM2", bad }, help).has_value() &&
               "a port that is not a port was accepted");
    }
    auto ok = parse({ "POM2", "--ai-control=6503", "--fujinet=1985" }, help);
    assert(ok && ok->aiControlPort == 6503 && ok->fujiNetPort == 1985);
    std::printf("  ok: --ai-control= / --fujinet= refuse garbage and overflow\n");
}

void testPositionalDisk()
{
    bool help = false;
    auto p = parse({"POM2", "game.dsk"}, help);
    assert(p.has_value());
    assert(!help);
    assert(p->bootDiskPath == "game.dsk");
    assert(!p->kiosk);
}

void testKioskFlagWithDisk()
{
    bool help = false;
    auto p = parse({"POM2", "--kiosk", "game.dsk"}, help);
    assert(p.has_value());
    assert(p->kiosk);
    assert(p->bootDiskPath == "game.dsk");
}

void testKioskFlagOnly()
{
    bool help = false;
    auto p = parse({"POM2", "--kiosk"}, help);
    assert(p.has_value());
    assert(p->kiosk);
    assert(p->bootDiskPath.empty());
}

void testPositionalComposesWithFlags()
{
    bool help = false;
    auto p = parse({"POM2", "--preset", "iie", "game.po"}, help);
    assert(p.has_value());
    assert(p->preset == pom2::CliPreset::AppleIIe);
    assert(p->bootDiskPath == "game.po");
}

void testTwoPositionalsRejected()
{
    bool help = false;
    auto p = parse({"POM2", "a.dsk", "b.dsk"}, help);
    assert(!p.has_value());     // only one disk image allowed
}

void testUnknownFlagStillRejected()
{
    bool help = false;
    auto p = parse({"POM2", "--bogus"}, help);
    assert(!p.has_value());
}

void testNoArgsCleanPlan()
{
    bool help = false;
    auto p = parse({"POM2"}, help);
    assert(p.has_value());
    assert(!p->kiosk);
    assert(p->bootDiskPath.empty());
}

void testAddrParsingHex()
{
    // Bare addresses are HEX (Apple II convention): "2000" → $2000,
    // "0300" → $0300. "$"/"0x" prefixes also hex. (R3-#1)
    bool help = false;
    auto p = parse({"POM2", "--load", "2000:dummy.bin", "--run", "0300"}, help);
    assert(p.has_value());
    assert(p->deferredActions.size() == 2);
    assert(p->deferredActions[0].kind == pom2::CliAction::Kind::Load);
    assert(p->deferredActions[0].addressI == 0x2000);
    assert(p->deferredActions[0].pathS == "dummy.bin");
    assert(p->deferredActions[1].kind == pom2::CliAction::Kind::Run);
    assert(p->deferredActions[1].addressI == 0x0300);

    assert(parse({"POM2", "--run", "768"},   help)->deferredActions[0].addressI == 0x768);
    assert(parse({"POM2", "--run", "$4000"}, help)->deferredActions[0].addressI == 0x4000);
    assert(parse({"POM2", "--run", "0xC0"},  help)->deferredActions[0].addressI == 0x00C0);
    assert(parse({"POM2", "--run", "FFFF"},  help)->deferredActions[0].addressI == 0xFFFF);
}

void testAddrParsingRejectsGarbage()
{
    // Trailing garbage and out-of-range must be rejected (not silently
    // truncated to a partial parse). (R3-#2)
    bool help = false;
    assert(!parse({"POM2", "--run",  "12ZZ"}, help).has_value());
    assert(!parse({"POM2", "--run",  "3G"},   help).has_value());
    assert(!parse({"POM2", "--run",  "10000"},help).has_value());  // > $FFFF
    assert(!parse({"POM2", "--load", "ZZ:dummy.bin"}, help).has_value());
}

void testPresetIieUnenhanced()
{
    // The documented Unenhanced-//e preset family must be CLI-reachable. (R3-#3)
    bool help = false;
    assert(parse({"POM2","--preset","iie-u"},        help)->preset
               == pom2::CliPreset::AppleIIeUnenhanced);
    assert(parse({"POM2","--preset","iieunenhanced"},help)->preset
               == pom2::CliPreset::AppleIIeUnenhanced);
    assert(parse({"POM2","--preset","apple2e-1983"}, help)->preset
               == pom2::CliPreset::AppleIIeUnenhanced);
    assert(parse({"POM2","--preset","//e-u"},        help)->preset
               == pom2::CliPreset::AppleIIeUnenhanced);
    // Plain "iie" still resolves to the Enhanced profile (not the new one).
    assert(parse({"POM2","--preset","iie"}, help)->preset
               == pom2::CliPreset::AppleIIe);

    // The PAL Unenhanced machine (the French Touch 6502-only corpus:
    // OLDSKOOL FORT ET VERT needs NMOS + 50 Hz).
    assert(parse({"POM2","--preset","iie-u-pal"},   help)->preset
               == pom2::CliPreset::AppleIIeUnenhancedPAL);
    assert(parse({"POM2","--preset","frenchtouch"}, help)->preset
               == pom2::CliPreset::AppleIIeUnenhancedPAL);
    // …and "iie-pal" still means the Enhanced PAL machine.
    assert(parse({"POM2","--preset","iie-pal"}, help)->preset
               == pom2::CliPreset::AppleIIePAL);
}

// ── classifyDiskForSlot ──────────────────────────────────────────────────

// Create a temp file of exactly `size` bytes with the given name.
std::string makeFile(const fs::path& dir, const std::string& name, uint64_t size)
{
    const fs::path p = dir / name;
    std::ofstream f(p, std::ios::binary);
    if (size) { f.seekp(static_cast<std::streamoff>(size) - 1); f.put('\0'); }
    f.close();
    return p.string();
}

// Write a REAL 2IMG envelope: 64-byte header declaring `payload` bytes of
// data at offset 64, then the payload, then `trailer` bytes of comment.
// `makeFile` above produces all-zero files, so a `.2mg` built by it carries
// no magic and exercises the size-heuristic fallback instead — both paths
// matter, hence the two helpers.
std::string make2mg(const fs::path& dir, const std::string& name,
                    uint64_t payload, uint64_t trailer = 0,
                    uint64_t declaredLenOverride = 0)
{
    const fs::path p = dir / name;
    std::ofstream f(p, std::ios::binary);
    unsigned char hdr[64] = {0};
    std::memcpy(hdr, "2IMG", 4);
    std::memcpy(hdr + 4, "POM2", 4);
    hdr[8] = 64;                                    // header length
    const auto put32 = [&hdr](int o, uint64_t v) {
        hdr[o]     = static_cast<unsigned char>( v        & 0xFF);
        hdr[o + 1] = static_cast<unsigned char>((v >>  8) & 0xFF);
        hdr[o + 2] = static_cast<unsigned char>((v >> 16) & 0xFF);
        hdr[o + 3] = static_cast<unsigned char>((v >> 24) & 0xFF);
    };
    put32(24, 64);                                  // data offset
    put32(28, declaredLenOverride ? declaredLenOverride : payload);
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    const std::vector<char> body(static_cast<size_t>(payload), '\0');
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    const std::vector<char> tail(static_cast<size_t>(trailer), 'C');
    if (trailer) f.write(tail.data(), static_cast<std::streamsize>(tail.size()));
    f.close();
    return p.string();
}

// The 2IMG envelope is classified by PARSING its header, not by guessing
// from the file size: the format allows an arbitrary data offset plus a
// comment/creator trailer, so size arithmetic mis-buckets ordinary
// CiderPress output. Pins the three payload sizes that pick a slot class,
// each with a trailer that used to push the file into no bucket at all.
void test2mgHeaderClassification()
{
    fs::path dir = fs::temp_directory_path() / "pom2_cli_kiosk_2mg";
    fs::create_directories(dir);

    // 800K payload + a comment block → still a 3.5" disk. Before the header
    // parse, `sz = 64 + 819200 + 900` fell past the `< 819200 + 4096` window
    // only for large comments, and any comment ≥ 4 KB was refused outright.
    assert(classifyDiskForSlot(make2mg(dir, "c35.2mg", 819200, 5000))
           == DiskSlotClass::Sony35);
    // Hard-disk payload + comment: `sz % 512` and `(sz - 64) % 512` both
    // non-zero → the old rules returned Unknown and the drop was refused.
    assert(classifyDiskForSlot(make2mg(dir, "chd.2mg", 4u * 1024 * 1024, 100))
           == DiskSlotClass::Hdv);
    // 5.25" ProDOS payload.
    assert(classifyDiskForSlot(make2mg(dir, "c525.2mg", 143360, 100))
           == DiskSlotClass::Floppy525);
    // A ProDOS volume between 143 KB and 800 KB belongs on the HDV card —
    // the 5.25" loader rejects it by size, so routing it there dead-ended.
    assert(classifyDiskForSlot(make2mg(dir, "mid.2mg", 400 * 512))
           == DiskSlotClass::Hdv);
    // A payload window running past EOF is a malformed envelope: fall back
    // to the size heuristics rather than trusting the header.
    assert(classifyDiskForSlot(
               make2mg(dir, "bad.2mg", 819200, 0, /*declaredLen=*/99u << 20))
           == DiskSlotClass::Sony35);

    fs::remove_all(dir);
}

void testClassifier()
{
    fs::path dir = fs::temp_directory_path() / "pom2_cli_kiosk_test";
    fs::create_directories(dir);

    // 5.25" Disk II by extension (size-agnostic for these).
    assert(classifyDiskForSlot(makeFile(dir, "a.dsk", 143360)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "a.do",  143360)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "a.nib", 232960)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "a.woz", 200000)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "a.d13", 116480)) == DiskSlotClass::Floppy525);
    // .po @ 143360 = 5.25" ProDOS; @ 800K = 3.5".
    assert(classifyDiskForSlot(makeFile(dir, "p525.po", 143360)) == DiskSlotClass::Floppy525);
    assert(classifyDiskForSlot(makeFile(dir, "p35.po",  819200)) == DiskSlotClass::Sony35);
    assert(classifyDiskForSlot(makeFile(dir, "m35.2mg", 819200)) == DiskSlotClass::Sony35);
    // HDV: 512-aligned. A `.hdv` is a hard disk at ANY size — including
    // EXACTLY 800K (1600 blocks). Regression: the old `> 819200` bound dropped
    // exactly-800K .hdv images (AppleWorks_AW.hdv) into Unknown.
    assert(classifyDiskForSlot(makeFile(dir, "aw.hdv", 819200)) == DiskSlotClass::Hdv);
    assert(classifyDiskForSlot(makeFile(dir, "small.hdv", 143360)) == DiskSlotClass::Hdv);
    assert(classifyDiskForSlot(makeFile(dir, "h.hdv", 1024 * 1024)) == DiskSlotClass::Hdv);
    assert(classifyDiskForSlot(makeFile(dir, "h.2mg", 4 * 1024 * 1024)) == DiskSlotClass::Hdv);
    // A `.2mg` stays ambiguous: exactly 800K is a 3.5" disk (asserted above),
    // not an HDV — only LARGER .2mg reach the HDV bucket.
    // An 800K `.dsk` / `.image` is a Sony 3.5" payload, NOT a 5.25" one:
    // Disk35Image takes a bare 819200-byte image under either name. Routing
    // them to the 5.25" loader made a droppable disk fail with a message
    // that listed only 5.25" sizes.
    assert(classifyDiskForSlot(makeFile(dir, "big.dsk",   819200)) == DiskSlotClass::Sony35);
    assert(classifyDiskForSlot(makeFile(dir, "s.image",   819200)) == DiskSlotClass::Sony35);
    assert(classifyDiskForSlot(makeFile(dir, "small.dsk", 143360)) == DiskSlotClass::Floppy525);
    // Unknown: wrong extension, wrong size, or missing file.
    assert(classifyDiskForSlot(makeFile(dir, "x.txt", 143360)) == DiskSlotClass::Unknown);
    assert(classifyDiskForSlot(makeFile(dir, "odd.po", 999))   == DiskSlotClass::Unknown);
    assert(classifyDiskForSlot((dir / "does_not_exist.dsk").string()) == DiskSlotClass::Unknown);

    fs::remove_all(dir);
}

void testIntArgOverflowRejected()
{
    bool help = false;
    // Values > INT_MAX must be REJECTED, not truncated by long->int to a
    // bogus-but-positive cycles/frame or step count (the n<=0 guard alone
    // missed wrap-to-positive). 9999999999 and 0x1_0000_220D both overflow.
    assert(!parse({"POM2", "--speed", "9999999999"}, help).has_value());
    assert(!parse({"POM2", "--step",  "4294984461"}, help).has_value());
    // Sane in-range values still parse.
    assert(parse({"POM2", "--speed", "1000000"}, help).has_value());
    assert(parse({"POM2", "--step",  "100"}, help).has_value());
}

void testSpeedClampedToAiServerCeiling()
{
    bool help = false;
    // --speed is the worker's per-frame cycle budget; values past the AI
    // server's 2M ceiling (AiControlServer kMaxCpf) produced multi-second
    // uninterruptible frames. They are CLAMPED (with a warning), not
    // rejected, so scripted launches keep working.
    auto p = parse({"POM2", "--speed", "2000000000"}, help);
    assert(p.has_value());
    assert(p->executionSpeed.has_value());
    assert(*p->executionSpeed == 2'000'000);
    // The boundary and in-range values pass through unchanged.
    p = parse({"POM2", "--speed", "2000000"}, help);
    assert(p.has_value() && *p->executionSpeed == 2'000'000);
    p = parse({"POM2", "--speed", "17045"}, help);
    assert(p.has_value() && *p->executionSpeed == 17045);
}

// ── "--flag VALUE" for a flag whose value only attaches with "=" ────────
// Hunt #4 item #17. `--ai-control`, `--fujinet`, `--fujinet-serial` and
// `--rgb-card-invert-bit7` take their value after an `=`. Written with a
// space, the value fell straight through to the positional-disk branch:
// `POM2 --ai-control 6503` armed the server on the DEFAULT port and then
// reported "unrecognised disk image: 6503" — two wrong outcomes from one
// plausible command line, neither of them an error.
void testSpaceSeparatedValueRejected()
{
    bool help = false;

    assert(!parse({"POM2", "--ai-control", "6503"}, help).has_value());
    assert(!parse({"POM2", "--fujinet", "1985"}, help).has_value());
    assert(!parse({"POM2", "--rgb-card-invert-bit7", "on"}, help).has_value());
    assert(!parse({"POM2", "--rgb-card-invert-bit7", "OFF"}, help).has_value());
    assert(!parse({"POM2", "--fujinet-serial", "/dev/ttyUSB0"}, help).has_value());
    assert(!parse({"POM2", "--fujinet-serial", "COM3"}, help).has_value());

    // The `=` form is the one that works, and still does.
    auto ok = parse({"POM2", "--ai-control=6503"}, help);
    assert(ok.has_value() && ok->aiControl && ok->aiControlPort == 6503);
    auto fj = parse({"POM2", "--fujinet=1985"}, help);
    assert(fj.has_value() && fj->fujiNetPort == 1985);

    // And the check must stay NARROW: a bare flag followed by a positional
    // disk image is a legitimate invocation and must not be collateral.
    auto disk = parse({"POM2", "--ai-control", "game.dsk"}, help);
    assert(disk.has_value());
    assert(disk->aiControl && disk->bootDiskPath == "game.dsk");
    auto disk2 = parse({"POM2", "--rgb-card-invert-bit7", "extasie.woz"}, help);
    assert(disk2.has_value());
    assert(disk2->rgbCardInvertBit7.has_value() && *disk2->rgbCardInvertBit7);
    assert(disk2->bootDiskPath == "extasie.woz");
    // A bare flag as the LAST argument is fine too.
    auto last = parse({"POM2", "--fujinet"}, help);
    assert(last.has_value() &&
           last->fujiNet == pom2::CliPlan::FujiNetTransport::Tcp);
}

// `POM2 --version` used to be rejected as an unknown flag while
// `pom2_headless --version` worked — the one thing a user reaching for it
// (attaching a version to a bug report) cannot work around. It takes the
// --help exit path: nullopt, with helpRequested set so the caller exits 0.
void testVersionFlag()
{
    bool help = false;
    assert(!parse({"POM2", "--version"}, help).has_value());
    assert(help);
    help = false;
    assert(!parse({"POM2", "-v"}, help).has_value());
    assert(help);
}

}  // namespace

int main()
{
    testPositionalDisk();
    std::printf("parseCli positional disk: OK\n");
    testKioskFlagWithDisk();
    std::printf("parseCli --kiosk + disk: OK\n");
    testKioskFlagOnly();
    std::printf("parseCli --kiosk only: OK\n");
    testPositionalComposesWithFlags();
    std::printf("parseCli positional + --preset: OK\n");
    testTwoPositionalsRejected();
    std::printf("parseCli rejects two positionals: OK\n");
    testUnknownFlagStillRejected();
    testPortValueRejectsGarbageAndOverflow();
    std::printf("parseCli rejects unknown flag: OK\n");
    testNoArgsCleanPlan();
    std::printf("parseCli no-args clean plan: OK\n");
    testAddrParsingHex();
    std::printf("parseCli --load/--run hex addresses: OK\n");
    testAddrParsingRejectsGarbage();
    std::printf("parseCli rejects garbage/out-of-range addresses: OK\n");
    testPresetIieUnenhanced();
    std::printf("parseCli --preset iie-u family: OK\n");
    testIntArgOverflowRejected();
    std::printf("parseCli rejects --speed/--step int overflow: OK\n");
    testSpeedClampedToAiServerCeiling();
    std::printf("parseCli clamps --speed to the 2M cycles/frame ceiling: OK\n");
    testClassifier();
    std::printf("classifyDiskForSlot 5.25/3.5/HDV/unknown: OK\n");
    test2mgHeaderClassification();
    std::printf("classifyDiskForSlot 2IMG header (trailer/offset): OK\n");
    testFujiNetSlotExplicitness();
    std::printf("parseCli --fujinet slot preference vs explicit: OK\n");
    testSpaceSeparatedValueRejected();
    std::printf("parseCli rejects \"--flag VALUE\" for =VALUE flags: OK\n");
    testVersionFlag();
    std::printf("parseCli --version / -v: OK\n");

    std::printf("cli_kiosk OK\n");
    return 0;
}
